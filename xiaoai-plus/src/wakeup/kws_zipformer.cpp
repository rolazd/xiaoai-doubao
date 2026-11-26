#include "wakeup/kws_zipformer.hpp"
#include "wakeup/exit_echo_guard.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include <unistd.h>

#include <sherpa-onnx/c-api/cxx-api.h>

#include "common/log.hpp"

namespace xiaoai_plus::wakeup {

namespace {


const auto kLog = xiaoai_plus::GetLogger("kws");
constexpr const char* kExitLabel = "结束";

std::string Trim(std::string s) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

// Wake words form the spotter's default list; exit words are only added to
// the stream used during a session (sherpa-onnx merges per-stream keywords
// with the defaults).
struct KeywordSplit {
  std::string wake_lines;  // newline separated, for keywords_file
  std::string strong_lines;
  std::string exit_lines;  // '/' separated, for CreateStream(keywords)
  int wake_count{0};
  int exit_count{0};
};

KeywordSplit SplitKeywords(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    throw std::runtime_error("failed to open wakeup.keywords_file: " + path);
  }
  KeywordSplit out;
  std::string line;
  while (std::getline(in, line)) {
    line = Trim(std::move(line));
    if (line.empty() || line.front() == '#' || line.front() == ';') continue;
    if (line.find('/') != std::string::npos) {
      throw std::runtime_error("wakeup.keywords_file line must not contain '/': " + line);
    }
    const auto at = line.rfind('@');
    if (at != std::string::npos && Trim(line.substr(at + 1)) == kExitLabel) {
      const bool goodbye=line.rfind("z ài j iàn",0)==0;
      auto& lines=goodbye?out.exit_lines:out.strong_lines;
      if(!goodbye) {
        const auto label=StrongExitLabel(line);
        if(label.empty()) throw std::runtime_error("unrecognized strong exit keyword");
        line.replace(at,std::string::npos,"@"+label);
      }
      if (!lines.empty()) lines += '/';
      lines += line;
      ++out.exit_count;
    } else {
      out.wake_lines += line;
      out.wake_lines += '\n';
      ++out.wake_count;
    }
  }
  if (out.wake_count == 0) {
    throw std::runtime_error("wakeup.keywords_file has no wake keyword: " + path);
  }
  return out;
}

// The C++ wrapper only accepts a keywords file, so hand it the wake-only
// subset through a private temp file that is removed right after loading.
sherpa_onnx::cxx::KeywordSpotter CreateSpotter(const config::Wakeup& cfg,
                                              const std::string& wake_lines) {
  char path[] = "/tmp/xiaoai-kws-XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) throw std::runtime_error("kws: cannot create temp keywords file");
  size_t done = 0;
  while (done < wake_lines.size()) {
    const ssize_t n = write(fd, wake_lines.data() + done, wake_lines.size() - done);
    if (n <= 0) {
      close(fd);
      unlink(path);
      throw std::runtime_error("kws: cannot write temp keywords file");
    }
    done += static_cast<size_t>(n);
  }
  close(fd);

  sherpa_onnx::cxx::KeywordSpotterConfig c;
  c.model_config.model_type = "zipformer2";
  c.model_config.tokens = cfg.tokens_path;
  c.model_config.transducer.encoder = cfg.encoder_path;
  c.model_config.transducer.decoder = cfg.decoder_path;
  c.model_config.transducer.joiner = cfg.joiner_path;
  c.model_config.num_threads = cfg.kws_num_threads;
  c.model_config.provider = "cpu";
  c.keywords_threshold = cfg.kws_threshold;
  c.keywords_score = 3.0f;
  c.num_trailing_blanks = 0;
  c.max_active_paths = 8;
  c.keywords_file = path;
  auto s = sherpa_onnx::cxx::KeywordSpotter::Create(c);
  unlink(path);
  if (s.Get() == nullptr) {
    kLog->error("kws model load failed: spotter is null "
                "(check model files exist and memory/disk is sufficient)");
    throw std::runtime_error(
        "kws model load failed: "
        "check model files exist and memory/disk is sufficient");
  }
  return s;
}

}  // namespace

struct ZipformerKwsEngine::Impl {
  KeywordSplit words;
  sherpa_onnx::cxx::KeywordSpotter spotter;
  sherpa_onnx::cxx::OnlineStream stream;
  bool exit_enabled{false},goodbye_enabled{false};
  std::vector<float> samples;

  explicit Impl(const config::Wakeup& cfg)
      : words(SplitKeywords(cfg.keywords_file)),
        spotter(CreateSpotter(cfg, words.wake_lines)),
        stream(MakeStream(false,false)) {
    // Validate the exit words now rather than at the first session.
    if (words.exit_count > 0) (void)MakeStream(true,true);
    kLog->info("kws keywords: {} wake, {} exit (exit only during a session)",
               words.wake_count, words.exit_count);
  }

  sherpa_onnx::cxx::OnlineStream MakeStream(bool strong,bool goodbye) const {
    std::string lines=strong?words.strong_lines:"";
    if(goodbye && !words.exit_lines.empty()){if(!lines.empty())lines+='/';lines+=words.exit_lines;}
    auto s = !lines.empty() ? spotter.CreateStream(lines) : spotter.CreateStream();
    if (s.Get() == nullptr) {
      kLog->error("kws model load failed: CreateStream returned null");
      throw std::runtime_error("kws model load failed: CreateStream returned null");
    }
    return s;
  }

  std::optional<KwsHit> Accept(const uint8_t* pcm, size_t size_bytes, int sample_rate,
                               int channels, int bits_per_sample) {
    if (!pcm || size_bytes < 2 || bits_per_sample != 16 || channels != 1 || sample_rate <= 0) {
      return std::nullopt;
    }

    const size_t n = size_bytes / sizeof(int16_t);
    if (n == 0) {
      return std::nullopt;
    }
    const auto* in = reinterpret_cast<const int16_t*>(pcm);

    samples.resize(n);
    for (size_t i = 0; i < n; ++i) {
      samples[i] = static_cast<float>(in[i]) / 32768.0f;
    }

    stream.AcceptWaveform(sample_rate, samples.data(), static_cast<int32_t>(n));

    while (spotter.IsReady(&stream)) {
      spotter.Decode(&stream);
      auto r = spotter.GetResult(&stream);
      if (!r.keyword.empty()) {
        spotter.Reset(&stream);
        const auto meta=nlohmann::json::parse(r.json);
        // Unpatched sherpa-onnx does not expose the match score; accept the
        // hit instead of failing the whole KWS stream.
        float score = 1.0f;
        if (meta.contains("score") && meta["score"].is_number())
          score = meta["score"].get<float>();
        return KwsHit{r.keyword,score};
      }
    }

    return std::nullopt;
  }

  void Reset() { stream = MakeStream(exit_enabled,goodbye_enabled); }

  void SetExitWordsEnabled(bool strong,bool goodbye) {
    if (strong==exit_enabled && goodbye==goodbye_enabled)return;
    exit_enabled=strong;goodbye_enabled=goodbye;
    stream=MakeStream(strong,goodbye);
  }
};

ZipformerKwsEngine::ZipformerKwsEngine(config::Wakeup cfg)
    : cfg_(std::move(cfg)), impl_(std::make_unique<Impl>(cfg_)) {}

ZipformerKwsEngine::~ZipformerKwsEngine() = default;

std::optional<KwsHit> ZipformerKwsEngine::AcceptPcm16(const uint8_t* pcm, size_t size_bytes,
                                                       int sample_rate, int channels,
                                                       int bits_per_sample) {
  if (!impl_) {
    return std::nullopt;
  }
  return impl_->Accept(pcm, size_bytes, sample_rate, channels, bits_per_sample);
}

void ZipformerKwsEngine::Reset() {
  if (impl_) {
    impl_->Reset();
  }
}

void ZipformerKwsEngine::SetExitWordsEnabled(bool enabled) {
  if (impl_) {
    impl_->SetExitWordsEnabled(enabled,enabled);
  }
}

void ZipformerKwsEngine::SetExitWordsEnabled(bool strong,bool goodbye) {
  if(impl_)impl_->SetExitWordsEnabled(strong,goodbye);
}
}  // namespace xiaoai_plus::wakeup

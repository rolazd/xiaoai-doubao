#include "realtime/duplex_protocol.hpp"

#include <stdexcept>
#include <array>
#include <algorithm>

#include <nlohmann/json.hpp>

namespace xiaoai_plus::realtime::duplex {
namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr size_t kMaxServerFrame = 1024 * 1024;
constexpr size_t kMaxDecodedAudio = 512 * 1024;

std::string Encode64(const std::vector<uint8_t>& bytes) {
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const uint32_t n = (uint32_t(bytes[i]) << 16) |
                       (i + 1 < bytes.size() ? uint32_t(bytes[i + 1]) << 8 : 0) |
                       (i + 2 < bytes.size() ? uint32_t(bytes[i + 2]) : 0);
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(i + 1 < bytes.size() ? kAlphabet[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < bytes.size() ? kAlphabet[n & 63] : '=');
  }
  return out;
}

int Digit(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::vector<uint8_t> Decode64(const std::string& encoded) {
  if (encoded.empty() || encoded.size() % 4 != 0 ||
      encoded.size() / 4 * 3 > kMaxDecodedAudio + 2) {
    throw std::runtime_error("invalid or oversized audio base64");
  }
  std::vector<uint8_t> out;
  out.reserve(encoded.size() / 4 * 3);
  for (size_t i = 0; i < encoded.size(); i += 4) {
    const bool final = i + 4 == encoded.size();
    const bool pad2 = encoded[i + 2] == '=';
    const bool pad3 = encoded[i + 3] == '=';
    if ((!final && (pad2 || pad3)) || (pad2 && !pad3)) {
      throw std::runtime_error("invalid audio base64 padding");
    }
    const int a = Digit(encoded[i]);
    const int b = Digit(encoded[i + 1]);
    const int c = pad2 ? 0 : Digit(encoded[i + 2]);
    const int d = pad3 ? 0 : Digit(encoded[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0 ||
        (pad2 && (b & 15) != 0) || (pad3 && !pad2 && (c & 3) != 0)) {
      throw std::runtime_error("invalid audio base64 alphabet");
    }
    const uint32_t n = (uint32_t(a) << 18) | (uint32_t(b) << 12) |
                       (uint32_t(c) << 6) | uint32_t(d);
    out.push_back(uint8_t(n >> 16));
    if (!pad2) out.push_back(uint8_t(n >> 8));
    if (!pad3) out.push_back(uint8_t(n));
  }
  if (out.size() > kMaxDecodedAudio) {
    throw std::runtime_error("invalid PCM audio size");
  }
  // Never pass an Ogg page to the raw PCM player if the server ignores
  // session.audio.output.format.type=pcm.
  if (out.size() >= 4 && out[0] == 'O' && out[1] == 'g' &&
      out[2] == 'g' && out[3] == 'S') {
    throw std::runtime_error("compressed audio received while PCM was requested");
  }
  return out;
}

std::string Simple(const char* type) {
  return nlohmann::json{{"type", type}}.dump();
}

std::optional<uint64_t> BoundedTokenCount(const nlohmann::json& usage, const char* key) {
  if (!usage.is_object() || !usage.contains(key)) return std::nullopt;
  const auto& value = usage[key];
  if (value.is_number_unsigned()) {
    const uint64_t count = value.get<uint64_t>();
    return count <= 1000000000 ? std::optional<uint64_t>(count) : std::nullopt;
  }
  if (value.is_number_integer()) {
    const int64_t count = value.get<int64_t>();
    return count >= 0 && count <= 1000000000 ?
        std::optional<uint64_t>(static_cast<uint64_t>(count)) : std::nullopt;
  }
  return std::nullopt;
}

// Only named token counters are exported; unknown keys and all text stay private.
void CollectUsage(const nlohmann::json& value, const std::string& prefix,
                  Event* event, int depth = 0) {
  if (!value.is_object() || depth > 3 || event->usage_numbers.size() >= 48) return;
  static constexpr std::array<const char*, 18> fields = {
      "total_tokens", "input_tokens", "output_tokens", "text_tokens", "audio_tokens",
      "cached_tokens", "input_text_tokens", "input_audio_tokens", "output_text_tokens",
      "output_audio_tokens", "cached_input_tokens", "cached_text_tokens",
      "cached_audio_tokens", "prompt_tokens", "completion_tokens",
      "text_token_count", "audio_token_count", "cache_tokens"};
  static constexpr std::array<const char*, 8> objects = {
      "input_token_details", "output_token_details", "input_tokens_details",
      "output_tokens_details", "prompt_tokens_details", "completion_tokens_details",
      "cached_tokens_details", "cache_details"};
  for (const char* key : fields) {
    if (auto count = BoundedTokenCount(value, key))
      event->usage_numbers.emplace_back(prefix + key, *count);
  }
  for (const char* key : objects) {
    if (value.contains(key)) CollectUsage(value[key], prefix + key + ".", event, depth + 1);
  }
}

std::optional<uint64_t> FindUsage(const Event& event, std::initializer_list<const char*> paths) {
  for (const char* path : paths)
    for (const auto& [key, count] : event.usage_numbers)
      if (key == path) return count;
  return std::nullopt;
}

void EstimateUsage(Event* event) {
  const auto in_text = FindUsage(*event, {"input_text_tokens", "input_token_details.text_tokens",
                                          "input_tokens_details.text_tokens"});
  const auto in_audio = FindUsage(*event, {"input_audio_tokens", "input_token_details.audio_tokens",
                                           "input_tokens_details.audio_tokens"});
  const auto out_text = FindUsage(*event, {"output_text_tokens", "output_token_details.text_tokens",
                                           "output_tokens_details.text_tokens"});
  const auto out_audio = FindUsage(*event, {"output_audio_tokens", "output_token_details.audio_tokens",
                                            "output_tokens_details.audio_tokens"});
  const auto cached = FindUsage(*event, {"cached_input_tokens", "cached_tokens",
                                          "input_token_details.cached_tokens"});
  if (in_text && in_audio && out_text && out_audio && cached &&
      *cached <= *in_text + *in_audio &&
      (!event->input_tokens || *in_text + *in_audio == *event->input_tokens) &&
      (!event->output_tokens || *out_text + *out_audio == *event->output_tokens)) {
    // Cached input replaces the most expensive input class first for a safe upper bound;
    // without its text/audio split, the exact charge remains a range.
    const auto cached_audio = FindUsage(*event, {"cached_audio_tokens", "input_token_details.cached_audio_tokens"});
    const auto cached_text = FindUsage(*event, {"cached_text_tokens", "input_token_details.cached_text_tokens"});
    if (cached_audio && cached_text && *cached_audio + *cached_text == *cached &&
        *cached_audio <= *in_audio && *cached_text <= *in_text) {
      event->estimated_yuan = (10.0 * (*in_text - *cached_text) +
                               80.0 * (*in_audio - *cached_audio) + 5.0 * *cached +
                               80.0 * *out_text + 300.0 * *out_audio) / 1000000.0;
      return;
    }
    const auto low_audio = std::min(*cached, *in_audio);
    const auto high_audio = *cached > *in_text ? *cached - *in_text : 0;
    const auto cost = [&](uint64_t audio_cached) {
      return (10.0 * (*in_text - (*cached - audio_cached)) +
              80.0 * (*in_audio - audio_cached) + 5.0 * *cached +
              80.0 * *out_text + 300.0 * *out_audio) / 1000000.0;
    };
    event->estimated_min_yuan = cost(low_audio);
    event->estimated_max_yuan = cost(high_audio);
    return;
  }
  if (event->total_tokens) {
    event->estimated_min_yuan = 5.0 * *event->total_tokens / 1000000.0;
    event->estimated_max_yuan = 300.0 * *event->total_tokens / 1000000.0;
  }
}

std::optional<uint32_t> ErrorCode(const nlohmann::json& detail) {
  if (!detail.is_object()) return std::nullopt;
  const char* field = detail.contains("code") ? "code" : "status_code";
  if (!detail.contains(field)) return std::nullopt;
  const auto& value = detail[field];
  if (value.is_number_unsigned()) {
    const auto code = value.get<uint64_t>();
    if (code <= UINT32_MAX) return static_cast<uint32_t>(code);
  } else if (value.is_number_integer()) {
    const auto code = value.get<int64_t>();
    if (code >= 0 && code <= UINT32_MAX) return static_cast<uint32_t>(code);
  } else if (value.is_string()) {
    const auto code = value.get<std::string>();
    if (code.empty() || code.size() > 10) return std::nullopt;
    uint64_t parsed = 0;
    for (char c : code) {
      if (c < '0' || c > '9') return std::nullopt;
      parsed = parsed * 10 + static_cast<uint64_t>(c - '0');
    }
    if (parsed <= UINT32_MAX) return static_cast<uint32_t>(parsed);
  }
  return std::nullopt;
}

}  // namespace

void Session::RequireActive() const {
  if (state_ != State::kActive) throw std::logic_error("duplex session is not active");
}

std::string Session::Create(const std::string& instructions, const std::string& voice, bool child_safety,
                            const std::string& resume_id, const std::string& extra_json) {
  if (state_ != State::kIdle || voice.empty()) {
    throw std::logic_error("duplex session cannot be created");
  }
  state_ = State::kAwaitingCreated;
  nlohmann::json session = {
      {"model", kModel},
      {"instructions", instructions},
      {"audio", {
          {"input", {{"format", {{"type", "pcm"}, {"rate", kInputRate}}}}},
          {"output", {{"format", {{"type", "pcm"}, {"rate", kOutputRate}}},
                      {"voice", voice}}}
      }}
  };
  if (child_safety) {
    session["extension"]["dialog"]["extra"] = {
      {"strict_audit", true}, {"enable_volc_websearch", false},
      {"audit_response", "这个话题请和爸爸妈妈聊，我们换个练习吧"}};
    session["tools"] = nlohmann::json::array();
  } else if (!extra_json.empty()) {
    const auto extra = nlohmann::json::parse(extra_json);
    if (extra.is_object() && !extra.empty()) session["extension"]["dialog"]["extra"] = extra;
  }
  if (!child_safety && !resume_id.empty()) session["id"] = resume_id;
  return nlohmann::json{{"type", "session.create"}, {"session", std::move(session)}}.dump();
}

std::string Session::AudioAppend(const std::vector<uint8_t>& pcm_20ms) const {
  RequireActive();
  if (pcm_20ms.size() != kInput20msBytes) {
    throw std::invalid_argument("input PCM must be one 20ms mono S16LE frame");
  }
  return nlohmann::json{{"type", "input_audio_buffer.append"},
                        {"audio", Encode64(pcm_20ms)}}.dump();
}

std::string Session::Greeting(const std::string& text) const {
  RequireActive();
  return nlohmann::json{{"type", "speech_text_buffer.commit"}, {"text", text}}.dump();
}

std::string Session::CommitInput() const { RequireActive(); return Simple("input_audio_buffer.commit"); }
std::string Session::MuteInput() const { RequireActive(); return Simple("input_audio_mute.commit"); }
std::string Session::UnmuteInput() const { RequireActive(); return Simple("input_audio_unmute.commit"); }
std::string Session::CancelResponse() const { RequireActive(); return Simple("response.cancel"); }

std::string Session::Close() {
  RequireActive();
  state_ = State::kClosing;
  return Simple("session.close");
}

Event ReadUsageOnly(const std::string& json_text) {
  if(json_text.size()>kMaxServerFrame)throw std::runtime_error("server frame too large");
  const auto msg=nlohmann::json::parse(json_text);
  Event event;
  if(!msg.is_object() || !msg.contains("type") || !msg["type"].is_string() || msg["type"]!="response.done")return event;
  event.type="response.done";
  const nlohmann::json* usage=nullptr;
  if(msg.contains("usage"))usage=&msg["usage"];
  else if(msg.contains("response") && msg["response"].is_object() && msg["response"].contains("usage"))usage=&msg["response"]["usage"];
  if(usage) {
    event.usage_present=true;
    event.total_tokens=BoundedTokenCount(*usage,"total_tokens");
    event.input_tokens=BoundedTokenCount(*usage,"input_tokens");
    event.output_tokens=BoundedTokenCount(*usage,"output_tokens");
    CollectUsage(*usage,"",&event);EstimateUsage(&event);
  }
  return event;
}

Event ReadBillingObservation(const std::string& json_text) {
  if(json_text.size()>kMaxServerFrame)throw std::runtime_error("server frame too large");
  const auto msg=nlohmann::json::parse(json_text);
  Event e;
  if(!msg.is_object() || !msg.contains("type") || !msg["type"].is_string())return e;
  e.type=msg["type"].get<std::string>();
  if(e.type.size()>64 || (e.type!="response.done" && e.type.rfind("response.output_",0)!=0)){e.type.clear();return e;}
  auto id=[&](const char* field,IdField& provenance) {
    provenance.path=std::string("$.")+field;
    if(!msg.contains(field))return std::string{};
    provenance.state=IdField::State::kInvalid;
    if(msg[field].is_string()) {
      auto value=msg[field].get<std::string>();
      if(value.empty())provenance.state=IdField::State::kEmpty;
      else if(value.size()<=128){provenance.state=IdField::State::kValid;return value;}
    }
    return std::string{};
  };
  e.event_id=id("event_id",e.event_field);e.response_id=id("response_id",e.response_field);e.question_id=id("question_id",e.question_field);
  if(msg.contains("response") && msg["response"].is_object() && msg["response"].contains("id") &&
     (!msg["response"]["id"].is_string() || (!e.response_id.empty() && msg["response"]["id"]!=e.response_id)))
    e.response_field.state=IdField::State::kConflict;
  if(e.type=="response.done") {
    const auto u=ReadUsageOnly(json_text);e.usage_present=u.usage_present;e.total_tokens=u.total_tokens;
  }
  return e;
}

Event Session::Receive(const std::string& json_text) {
  if (json_text.size() > kMaxServerFrame) throw std::runtime_error("server frame too large");
  const auto msg = nlohmann::json::parse(json_text);
  if (!msg.is_object() || !msg.contains("type") || !msg["type"].is_string()) {
    throw std::runtime_error("server event has no type");
  }
  Event event;
  event.type = msg["type"].get<std::string>();
  if (event.type == "error") {
    const auto* detail = &msg;
    if (msg.contains("error") && msg["error"].is_object()) detail = &msg["error"];
    event.error_code = ErrorCode(*detail);
    if (detail->contains("message") && (*detail)["message"].is_string())
      event.text = (*detail)["message"].get<std::string>();
    if (msg.contains("X-Tt-Logid") && msg["X-Tt-Logid"].is_string())
      event.log_id = msg["X-Tt-Logid"].get<std::string>();
    else if (detail->contains("X-Tt-Logid") && (*detail)["X-Tt-Logid"].is_string())
      event.log_id = (*detail)["X-Tt-Logid"].get<std::string>();
    state_ = event.error_code == 45000003 ? State::kClosed : State::kFailed;
    return event;
  }
  if (event.type == "session.created") {
    if (state_ != State::kAwaitingCreated || !msg.contains("session") ||
        !msg["session"].is_object() || !msg["session"].value("id", "").size()) {
      throw std::runtime_error("invalid session.created event");
    }
    event.session_id = msg["session"]["id"].get<std::string>();
    id_ = event.session_id;
    state_ = State::kActive;
    return event;
  }
  if (event.type == "session.closed") {
    state_ = State::kClosed;
    return event;
  }
  if (state_ != State::kActive && state_ != State::kClosing) {
    event.warning = true;
    return event;
  }
  auto id = [&](const char* field,IdField& provenance) {
    provenance.path=std::string("$.")+field;
    if(!msg.contains(field))return std::string{};
    provenance.state=IdField::State::kInvalid;
    if(msg[field].is_string()) {
      auto value=msg[field].get<std::string>();
      if(value.empty())provenance.state=IdField::State::kEmpty;
      else if(value.size()<=128){provenance.state=IdField::State::kValid;return value;}
    }
    return std::string{};
  };
  if (event.type == "input_audio_transcription.completed")
    event.type = "conversation.item.input_audio_transcription.completed";
  event.event_id=id("event_id",event.event_field); event.item_id=id("item_id",event.item_field);
  event.question_id=id("question_id",event.question_field); event.response_id=id("response_id",event.response_field);
  if(msg.contains("response") && msg["response"].is_object() && msg["response"].contains("id") &&
     (!msg["response"]["id"].is_string() || (!event.response_id.empty() && msg["response"]["id"]!=event.response_id)))
    event.response_field.state=IdField::State::kConflict;
  if(msg.contains("content_index") && msg["content_index"].is_number_integer()) {
    const auto index=msg["content_index"].get<int64_t>();
    if(index>=0 && index<=64) event.content_index=static_cast<int>(index);
  }
  if (event.type == "response.output_audio.delta") {
    if (!msg.contains("delta") || !msg["delta"].is_string() ||
        msg["delta"].get<std::string>().empty()) { event.warning = true; return event; }
    event.audio = Decode64(msg["delta"].get<std::string>());
    if (event.audio.size() % 2 != 0) { event.audio.pop_back(); event.warning = true; }
  } else if (event.type == "conversation.item.input_audio_transcription.completed") {
    if (msg.contains("transcript") && msg["transcript"].is_string()) {
      event.text = msg["transcript"].get<std::string>();
    } else if (msg.contains("text") && msg["text"].is_string()) {
      event.text = msg["text"].get<std::string>();
    }
  } else if (event.type == "response.output_text.done" ||
             event.type == "response.output_text.delta" ||
             event.type == "conversation.item.input_audio_transcription.delta") {
    const char* field = event.type == "response.output_text.done" ? "text" : "delta";
    if (msg.contains(field) && msg[field].is_string()) event.text=msg[field].get<std::string>();
  } else if (event.type == "response.output_audio.done") {
    if (msg.contains("status_code")) {
      const auto& status = msg["status_code"];
      event.exit_intent = (status.is_string() && status.get<std::string>() == "20000002") ||
                          (status.is_number_integer() && status.get<int64_t>() == 20000002);
    }
  } else if (event.type == "response.done") {
    const auto billing=ReadUsageOnly(json_text);
    event.usage_present=billing.usage_present;event.total_tokens=billing.total_tokens;
    event.input_tokens=billing.input_tokens;event.output_tokens=billing.output_tokens;
    event.usage_numbers=billing.usage_numbers;event.estimated_yuan=billing.estimated_yuan;
    event.estimated_min_yuan=billing.estimated_min_yuan;event.estimated_max_yuan=billing.estimated_max_yuan;
  }
  return event;
}

std::vector<std::vector<uint8_t>> InputFramer::Append(const std::vector<uint8_t>& bytes) {
  if (bytes.size() % 2 != 0 || bytes.size() > 16000 * 2) {
    throw std::invalid_argument("input chunk is not bounded mono S16LE PCM");
  }
  pending_.insert(pending_.end(), bytes.begin(), bytes.end());
  std::vector<std::vector<uint8_t>> frames;
  while (pending_.size() >= kInput20msBytes) {
    std::vector<uint8_t> frame;
    frame.reserve(kInput20msBytes);
    for (size_t i = 0; i < kInput20msBytes; ++i) {
      frame.push_back(pending_.front());
      pending_.pop_front();
    }
    frames.push_back(std::move(frame));
  }
  return frames;
}

std::vector<uint8_t> InputFramer::FlushWithSilence() {
  if (pending_.empty()) return {};
  std::vector<uint8_t> frame(pending_.begin(), pending_.end());
  frame.resize(kInput20msBytes, 0);
  pending_.clear();
  return frame;
}

}  // namespace xiaoai_plus::realtime::duplex

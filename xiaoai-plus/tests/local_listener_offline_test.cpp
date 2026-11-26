#include "wakeup/local_listener.hpp"
#include "wakeup/exit_echo_guard.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using xiaoai_plus::wakeup::IKwsEngine;
using xiaoai_plus::wakeup::KwsHit;

class FakeEngine final : public IKwsEngine {
 public:
  std::optional<KwsHit> AcceptPcm16(const uint8_t*, size_t, int, int, int) override {
    accept_active.store(true);
    std::unique_lock<std::mutex> lock(mu);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return !block; });
    std::optional<KwsHit> result;
    if (!hits.empty()) {
      result = hits.front();
      hits.pop_front();
    }
    accept_active.store(false);
    return result;
  }

  void Reset() override {
    if (accept_active.load()) reset_during_accept.store(true);
    ++reset_calls;
  }

  void SetExitWordsEnabled(bool enabled) override { exit_enabled.store(enabled); }
  void SetExitWordsEnabled(bool strong,bool goodbye) override { strong_enabled=strong;exit_enabled=goodbye; }

  void Push(std::string hit,float score=1.0f) {
    std::lock_guard<std::mutex> lock(mu);
    hits.push_back(KwsHit{std::move(hit),score});
  }

  void Block() {
    std::lock_guard<std::mutex> lock(mu);
    block = true;
    entered = false;
  }

  void WaitUntilEntered() {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 2s, [&] { return entered; }));
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mu);
    block = false;
    cv.notify_all();
  }

  std::atomic<bool> accept_active{false};
  std::atomic<bool> reset_during_accept{false};
  std::atomic<int> reset_calls{0};
  std::atomic<bool> exit_enabled{false},strong_enabled{false};

 private:
  std::mutex mu;
  std::condition_variable cv;
  std::deque<KwsHit> hits;
  bool block{false};
  bool entered{false};
};

int main() {
  using xiaoai_plus::wakeup::Gate;
  using xiaoai_plus::wakeup::Hooks;
  using xiaoai_plus::wakeup::LocalListener;
  using xiaoai_plus::wakeup::Trigger;
  const std::vector<uint8_t> silence(320, 0);

  Gate gate(60s, Hooks{});
  auto engine = std::make_shared<FakeEngine>();
  int wakes = 0;
  Trigger trigger({"豆包", "豆包豆包"}, [&](const std::string&) {
    ++wakes;
    return true;
  });
  LocalListener::Config cfg;
  cfg.gate = &gate;
  cfg.trigger = &trigger;
  cfg.kws_engine = engine;
  cfg.sample_rate = 16000;
  cfg.min_trigger_interval_ms = 0;
  LocalListener listener(cfg);

  // A pending short prefix must be cleared when another session takes Gate.
  engine->Push("豆包");
  listener.AcceptPcm(silence);
  assert(wakes == 0);
  gate.Activate("external");
  listener.AcceptPcm(silence);  // Observes active Gate and resets the pending prefix.
  gate.Disarm("test");
  std::this_thread::sleep_for(1150ms);
  listener.AcceptPcm(silence);
  assert(wakes == 0);
  listener.Close();

  // Close's model Reset must wait until in-flight inference has finished.
  auto blocking_engine = std::make_shared<FakeEngine>();
  blocking_engine->Block();
  LocalListener::Config blocking_cfg = cfg;
  blocking_cfg.kws_engine = blocking_engine;
  LocalListener blocking_listener(blocking_cfg);
  std::thread inference([&] { blocking_listener.AcceptPcm(silence); });
  blocking_engine->WaitUntilEntered();
  std::thread closer([&] { blocking_listener.Close(); });
  std::this_thread::sleep_for(50ms);
  assert(blocking_engine->reset_calls.load() == 0);
  blocking_engine->Release();
  inference.join();
  closer.join();
  assert(blocking_engine->reset_calls.load() == 1);
  assert(!blocking_engine->reset_during_accept.load());

  // Trigger callbacks run without the listener mutex and may call Close().
  auto reentrant_engine = std::make_shared<FakeEngine>();
  reentrant_engine->Push("豆包");
  LocalListener* reentrant_ptr = nullptr;
  int reentrant_wakes = 0;
  Trigger reentrant_trigger({"豆包"}, [&](const std::string&) {
    ++reentrant_wakes;
    reentrant_ptr->Close();
    return true;
  });
  LocalListener::Config reentrant_cfg = cfg;
  reentrant_cfg.trigger = &reentrant_trigger;
  reentrant_cfg.kws_engine = reentrant_engine;
  LocalListener reentrant_listener(reentrant_cfg);
  reentrant_ptr = &reentrant_listener;
  reentrant_listener.AcceptPcm(silence);
  assert(reentrant_wakes == 1);
  assert(reentrant_engine->reset_calls.load() == 1);
  assert(!reentrant_engine->reset_during_accept.load());

  // In half duplex, local KWS keeps listening while a response is playing.
  int interrupted = 0;
  Hooks interrupt_hooks;
  interrupt_hooks.on_interrupt = [&](const std::string&) { ++interrupted; };
  Gate interrupt_gate(60s, interrupt_hooks);
  auto interrupt_engine = std::make_shared<FakeEngine>();
  Trigger interrupt_trigger({"豆包同学"}, [&](const std::string& word) {
    return interrupt_gate.TryWakeup(word);
  });
  LocalListener::Config interrupt_cfg = cfg;
  interrupt_cfg.gate = &interrupt_gate;
  interrupt_cfg.trigger = &interrupt_trigger;
  interrupt_cfg.kws_engine = interrupt_engine;
  interrupt_cfg.allow_playback_interrupt = true;
  LocalListener interrupt_listener(interrupt_cfg);
  interrupt_gate.Activate("test");
  interrupt_gate.SetAiSpeaking(true);
  interrupt_engine->Push("豆包同学");
  interrupt_listener.AcceptPcm(silence);
  assert(interrupted == 1 && !interrupt_gate.IsAiSpeaking());
  assert(interrupt_gate.step() == xiaoai_plus::wakeup::Step::kActive);
  interrupt_listener.Close();
  interrupt_gate.Close();

  // One KWS stream routes @结束 by session state, never through wake Trigger.
  Gate end_gate(60s, Hooks{});
  auto end_engine = std::make_shared<FakeEngine>();
  int end_wakes = 0;
  int ends = 0;
  Trigger end_trigger({"豆包豆包"}, [&](const std::string& word) {
    ++end_wakes;
    return end_gate.TryWakeup(word);
  });
  LocalListener::Config end_cfg = cfg;
  end_cfg.gate = &end_gate;
  end_cfg.trigger = &end_trigger;
  end_cfg.kws_engine = end_engine;
  end_cfg.allow_playback_interrupt = true;
  end_cfg.on_end = [&] { ++ends; end_gate.Disarm("local_kws_exit"); };
  end_cfg.exit_after_wake = 0ms;
  end_cfg.exit_after_ai_speech = 0ms;
  LocalListener end_listener(end_cfg);
  end_engine->Push("结束");
  end_listener.AcceptPcm(silence);
  assert(ends == 0 && end_wakes == 0 && end_gate.step() == xiaoai_plus::wakeup::Step::kIdle);
  assert(!end_engine->exit_enabled.load());  // Idle stream never decodes exit words.
  end_engine->Push("豆包豆包");
  end_listener.AcceptPcm(silence);
  assert(end_wakes == 1 && end_gate.step() == xiaoai_plus::wakeup::Step::kActive);
  end_engine->Push("结束");
  end_listener.AcceptPcm(silence);
  assert(ends == 1 && end_wakes == 1 && end_gate.step() == xiaoai_plus::wakeup::Step::kIdle);
  // Identity remains off across repeated questions; resolution starts a NEW grace.
  bool identity_pending=true;
  end_cfg.exit_allowed=[&]{return !identity_pending;};
  LocalListener identity_listener(end_cfg);
  end_gate.Activate("identity");
  for(int n=0;n<3;++n) {
    end_engine->Push("结束");identity_listener.AcceptPcm(silence);
    assert(ends==1 && !end_engine->exit_enabled.load());
  }
  identity_pending=false;
  identity_listener.AcceptPcm(silence);assert(end_engine->exit_enabled.load());
  identity_pending=true;identity_listener.AcceptPcm(silence);assert(!end_engine->exit_enabled.load());
  identity_listener.Close();end_gate.Disarm("test");
  // While the AI speaks its own audio must not end the session.
  end_gate.Activate("playing");
  end_gate.SetAiSpeaking(true);
  end_engine->Push("结束");
  end_listener.AcceptPcm(silence);
  assert(ends == 1 && end_gate.step() == xiaoai_plus::wakeup::Step::kActive);
  assert(!end_engine->exit_enabled.load());
  end_gate.SetAiSpeaking(false);
  end_engine->Push("结束");
  end_listener.AcceptPcm(silence);
  assert(end_engine->exit_enabled.load());
  assert(ends == 2 && end_wakes == 1 && end_gate.step() == xiaoai_plus::wakeup::Step::kIdle);
  end_listener.AcceptPcm(silence);
  assert(!end_engine->exit_enabled.load());
  end_listener.Close();
  end_gate.Close();

  // Strong commands bypass identity/wake/playback/tail protection; goodbye does not.
  Gate strong_gate(60s,Hooks{});auto strong_engine=std::make_shared<FakeEngine>();
  auto strong_cfg=end_cfg;strong_cfg.gate=&strong_gate;strong_cfg.kws_engine=strong_engine;
  strong_cfg.exit_allowed=[] {return false;};strong_cfg.exit_after_wake=2500ms;
  int strong_ends=0;strong_cfg.on_end=[&]{++strong_ends;strong_gate.Disarm("test");};
  LocalListener strong_listener(strong_cfg);
  for(int stage=0;stage<3;++stage) {
    strong_gate.Activate("identity");strong_gate.SetAiSpeaking(stage==1);
    strong_engine->Push("结束");strong_listener.AcceptPcm(silence);assert(strong_ends==stage);
    assert(strong_engine->strong_enabled && !strong_engine->exit_enabled);
    if(stage==2)strong_gate.SetAiSpeaking(false);
    strong_engine->Push("强退:退出");strong_listener.AcceptPcm(silence);assert(strong_ends==stage+1);
  }
  strong_gate.Activate("playing thresholds");strong_gate.SetAiSpeaking(true);
  strong_engine->Push("强退:退出",0.44f);strong_listener.AcceptPcm(silence);assert(strong_ends==3);
  strong_engine->Push("强退:退出",0.45f);strong_listener.AcceptPcm(silence);assert(strong_ends==4);
  strong_gate.Activate("quiet thresholds");
  for(float score:{0.30f,0.33f,0.41f,0.44f}) {
    strong_engine->Push("强退:退出",score);strong_listener.AcceptPcm(silence);assert(strong_ends==4);
  }
  strong_engine->Push("强退:退出",0.45f);strong_listener.AcceptPcm(silence);assert(strong_ends==5);
  strong_cfg.exit_playback_threshold=0.60f;LocalListener configured(strong_cfg);
  strong_gate.Activate("configured playing");strong_gate.SetAiSpeaking(true);
  strong_engine->Push("强退:退出",0.59f);configured.AcceptPcm(silence);assert(strong_ends==5);
  strong_engine->Push("豆包豆包",0.05f);
  const auto wake=configured.AnalyzePcm(silence,std::chrono::steady_clock::now());
  assert(wake.keyword=="豆包豆包" && !wake.end_session); // Wake route ignores the exit threshold.
  configured.Close();
  strong_listener.Close();strong_gate.Close();

  // AI text blocks only the corresponding word during playback and 500ms tail.
  Gate echo_gate(60s,Hooks{}); auto echo_engine=std::make_shared<FakeEngine>();
  auto echo_cfg=end_cfg;echo_cfg.gate=&echo_gate;echo_cfg.kws_engine=echo_engine;
  xiaoai_plus::wakeup::ExitEchoGuard echo;
  echo_cfg.ai_text_contains=[&](const std::string& label){return echo.Contains(label);};
  int echo_ends=0;echo_cfg.on_end=[&]{++echo_ends;echo_gate.Disarm("exit");};
  echo_cfg.exit_after_ai_speech=500ms;
  LocalListener echo_listener(echo_cfg);
  echo_gate.Activate("identity");echo_gate.SetAiSpeaking(true);
  echo.Update("r1","可以说退出",false);
  echo_engine->Push("强退:退出");echo_listener.AcceptPcm(silence);assert(echo_ends==0);
  echo_gate.SetAiSpeaking(false);
  echo_engine->Push("强退:退出");echo_listener.AcceptPcm(silence);assert(echo_ends==0);
  std::this_thread::sleep_for(550ms);
  echo_engine->Push("强退:退出");echo_listener.AcceptPcm(silence);assert(echo_ends==1);
  echo_gate.Activate("playing");echo_gate.SetAiSpeaking(true);
  echo.Update("r2","恐龙很好玩",false);
  echo_engine->Push("强退:退出");echo_listener.AcceptPcm(silence);assert(echo_ends==2);
  echo_listener.Close();echo_gate.Close();

  // Exit words wait after wake (greeting "在") and after the AI's audio tail.
  Gate grace_gate(60s, Hooks{});
  auto grace_engine = std::make_shared<FakeEngine>();
  int grace_ends = 0;
  LocalListener::Config grace_cfg = end_cfg;
  grace_cfg.gate = &grace_gate;
  bool grace_pending=true;
  grace_cfg.exit_allowed=[&]{return !grace_pending;};
  grace_cfg.kws_engine = grace_engine;
  grace_cfg.on_end = [&] { ++grace_ends; grace_gate.Disarm("local_kws_exit"); };
  grace_cfg.exit_after_wake = 200ms;
  grace_cfg.exit_after_ai_speech = 200ms;
  LocalListener grace_listener(grace_cfg);
  grace_gate.Activate("wake");
  grace_engine->Push("结束");
  grace_listener.AcceptPcm(silence);
  assert(grace_ends == 0 && grace_gate.step() == xiaoai_plus::wakeup::Step::kActive);
  assert(!grace_engine->exit_enabled.load());
  std::this_thread::sleep_for(250ms);
  grace_listener.AcceptPcm(silence);assert(!grace_engine->exit_enabled.load());
  grace_pending=false;grace_listener.AcceptPcm(silence);assert(!grace_engine->exit_enabled.load());
  std::this_thread::sleep_for(250ms);
  grace_listener.AcceptPcm(silence);
  assert(grace_engine->exit_enabled.load());
  grace_gate.SetAiSpeaking(true);
  grace_listener.AcceptPcm(silence);
  assert(!grace_engine->exit_enabled.load());
  grace_gate.SetAiSpeaking(false);
  grace_engine->Push("结束");
  grace_listener.AcceptPcm(silence);
  assert(grace_ends == 0 && !grace_engine->exit_enabled.load());
  std::this_thread::sleep_for(250ms);
  grace_engine->Push("结束");
  grace_listener.AcceptPcm(silence);
  assert(grace_ends == 1 && grace_gate.step() == xiaoai_plus::wakeup::Step::kIdle);
  grace_listener.Close();
  grace_gate.Close();
  gate.Close();
  assert(!gate.TryWakeup("豆包"));
}

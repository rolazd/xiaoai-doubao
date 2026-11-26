#ifdef NDEBUG
#error requires active assertions
#endif
#include "memory/service.hpp"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>
using namespace xiaoai_plus;
using memory::Json;
using namespace std::chrono_literals;

static void Check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}

// All inputs and history markers are synthetic. The transport only blocks in RAM.
struct Harness {
  struct Call { std::string text, result; bool released{false}; };
  char dir[80] = "/tmp/xzopt-identity-supersession-XXXXXX";
  std::mutex mu;
  std::condition_variable cv;
  std::vector<memory::Action> actions;
  std::vector<Call> calls;
  bool stopping{false};
  std::unique_ptr<memory::Service> svc;
  Harness() {
    Check(mkdtemp(dir), "temporary directory");
    config::Memory cfg;cfg.child_names={"Amy","Ben"};cfg.identity_question=memory::IdentityQuestion(cfg.child_names); cfg.enabled = true; cfg.cache_dir = dir;
    svc = std::make_unique<memory::Service>(cfg,
      [this](const std::string& path, const Json& body) -> memory::Reply {
        if (path == "/device/snapshot") return {200, {
          {"generation", 1}, {"expires_at", std::time(nullptr) + 86400},
          {"children", Json::array({
            Json{{"id", "amy"}, {"version", 1}, {"profile", {{"name", "Amy"}, {"aliases", Json::array()}}},
              {"summaries", Json::array({Json{{"recap", "AMY_PRIVATE_SYNTHETIC_HISTORY"}}})}},
            Json{{"id", "ben"}, {"version", 1}, {"profile", {{"name", "Ben"}, {"aliases", Json::array()}}},
              {"summaries", Json::array({Json{{"recap", "BEN_PRIVATE_SYNTHETIC_HISTORY"}}})}}
          })}}};
        if (path == "/device/identity") {
          Check(body.size() == 1 && body.contains("text"), "classification payload is text only");
          std::unique_lock<std::mutex> lock(mu);
          const size_t index = calls.size();
          calls.push_back({body.at("text"), "", false}); cv.notify_all();
          cv.wait(lock, [&]{ return stopping || calls[index].released; });
          return {200, {{"result", calls[index].result}}};
        }
        return {200, {{"ok", true}}};
      }, [this](memory::Action action) {
        const bool restart = action.restart; const auto epoch = action.epoch;
        if (restart) Check(svc->Activate(epoch), "synthetic activation queued");
        { std::lock_guard<std::mutex> lock(mu); actions.push_back(std::move(action)); }
        cv.notify_all();
      });
    svc->Start();
    Wait([&]{ return svc->cache_revision() > 0; }, "startup snapshot");
    Check(svc->Begin(1), "begin queued"); wait_actions(1);
    check_no_history(last());
  }
  ~Harness() {
    { std::lock_guard<std::mutex> lock(mu); stopping = true; }
    cv.notify_all(); svc->Stop(); std::filesystem::remove_all(dir);
  }
  template<class Predicate> static void Wait(Predicate predicate, const char* why) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!predicate()) {
      Check(std::chrono::steady_clock::now() < deadline, why);
      std::this_thread::sleep_for(2ms);
    }
  }
  void wait_actions(size_t n) { Wait([&]{ return count() >= n; }, "action deadline"); }
  void wait_calls(size_t n) {
    Wait([&]{ std::lock_guard<std::mutex> lock(mu); return calls.size() >= n; }, "transport call deadline");
  }
  size_t count() { std::lock_guard<std::mutex> lock(mu); return actions.size(); }
  size_t call_count() { std::lock_guard<std::mutex> lock(mu); return calls.size(); }
  memory::Action last() { std::lock_guard<std::mutex> lock(mu); return actions.back(); }
  void release(size_t index, std::string result) {
    { std::lock_guard<std::mutex> lock(mu); Check(index < calls.size(), "call exists");
      calls[index].result = std::move(result); calls[index].released = true; }
    cv.notify_all();
  }
  void hear(const char* text, const char* id, uint64_t epoch = 1) {
    realtime::duplex::Event event; event.type = "conversation.item.input_audio_transcription.completed";
    event.text = text; event.event_id = id; Check(svc->Event(epoch, event), "text event queued");
  }
  void network_barrier() {
    const auto revision = svc->snapshot_receipts(); Check(svc->Prefetch(), "prefetch queued");
    // NetworkLoop is serial: this snapshot result follows the released classifier.
    Wait([&]{ return svc->snapshot_receipts() > revision; }, "snapshot result barrier");
  }
  static void check_no_history(const memory::Action& action) {
    Check(action.instructions.find("PRIVATE_SYNTHETIC_HISTORY") == std::string::npos,
      "unresolved/guest/adult instructions contain no child history");
  }
  static void check_child(const memory::Action& action, const char* name) {
    const bool amy = std::string(name) != "Ben";
    Check(action.restart && !action.identity_pending && !action.exit, "child activation action");
    Check(action.greeting.find(std::string("Hi, ")+name+"!")==0 && action.after_reply, "identity greeting follows current reply");
    Check(action.instructions.find(amy ? "AMY_PRIVATE_SYNTHETIC_HISTORY" : "BEN_PRIVATE_SYNTHETIC_HISTORY") != std::string::npos,
      "only confirmed child's synthetic history included");
    Check(action.instructions.find(amy ? "BEN_PRIVATE_SYNTHETIC_HISTORY" : "AMY_PRIVATE_SYNTHETIC_HISTORY") == std::string::npos,
      "other child's synthetic history absent");
  }
};

int main() {
  Json result = {{"fixture_version", "identity-supersession-v1"},
    {"scope", "real Service/Core; synthetic transport/text/history; no device/cloud/audio"}};
  int failures = 0;
  auto run = [&](const std::string& name, auto test) {
    try { test(); result["cases"][name] = "PASS"; }
    catch (const std::exception& e) { ++failures; result["cases"][name] = {{"status", "FAIL"}, {"reason", e.what()}}; }
  };
  for (const char* name : {"Amy", "Ben"}) run(std::string("single_") + name, [=]{
    Harness h; h.hear(name, "explicit"); h.wait_actions(2);
    Harness::check_child(h.last(), name); Check(h.call_count() == 0, "explicit name bypasses classifier");
  });
  for (const char* name : {"Amy", "Ben"}) run(std::string("prior_adult_after_") + name, [=]{
    Harness h; h.hear("opaque synthetic adult", "old"); h.wait_calls(1);
    h.hear(name, "explicit"); h.wait_actions(2); Harness::check_child(h.last(), name);
    // Activate was queued before publishing the action. Give the real worker
    // multiple iterations to consume it before releasing the controlled result.
    std::this_thread::sleep_for(100ms); h.release(0, "adult"); h.network_barrier();
    Check(h.count() == 2, "old adult result reopened the confirmed child session");
    Harness::check_child(h.last(), name);
  });
  run("old_seq_does_not_consume_new_pending_same_epoch", []{
    Harness h; h.hear("opaque first adult", "old"); h.wait_calls(1);
    // A public Begin resets identity, even when the caller retains the epoch.
    Check(h.svc->Begin(1), "same-epoch reset queued"); h.wait_actions(2);
    h.hear("opaque second adult", "new");
    std::this_thread::sleep_for(100ms); h.release(0, "adult"); h.wait_calls(2);
    std::this_thread::sleep_for(100ms); Check(h.count() == 2, "old seq consumed new pending");
    h.release(1, "ben"); h.wait_actions(3); Harness::check_no_history(h.last());
  });
  run("old_result_after_new_epoch", []{
    Harness h; h.hear("opaque synthetic adult", "old"); h.wait_calls(1);
    Check(h.svc->Begin(2), "new epoch queued"); h.wait_actions(2);
    h.hear("Amy", "new-explicit", 2); h.wait_actions(3);
    std::this_thread::sleep_for(100ms); h.release(0, "adult"); h.network_barrier();
    Check(h.count() == 3, "old epoch reopened session"); Harness::check_child(h.last(), "Amy");
  });
  run("explicit_exit_cancels_pending", []{
    Harness h; h.hear("opaque synthetic adult", "old"); h.wait_calls(1);
    h.hear("退出", "exit"); h.wait_actions(2); Check(h.last().exit, "explicit exit action");
    h.release(0, "adult"); h.network_barrier(); Check(h.count() == 2, "late classifier after exit");
  });
  run("end_cancels_pending", []{
    Harness h; h.hear("opaque synthetic adult", "old"); h.wait_calls(1);
    Check(h.svc->End(1), "end queued"); std::this_thread::sleep_for(100ms);
    h.release(0, "adult"); h.network_barrier(); Check(h.count() == 1, "late classifier after end");
  });
  run("interrupt_keeps_identity_attempt", []{
    Harness h; h.hear("opaque synthetic child", "old"); h.wait_calls(1);
    Check(h.svc->Interrupt(1), "interrupt queued"); std::this_thread::sleep_for(100ms);
    h.release(0, "amy"); h.wait_actions(2); Harness::check_no_history(h.last());
  });
  run("unknown_timeout_retry_then_late_ignored", [&]{
    Harness h; const auto begin = std::chrono::steady_clock::now();
    h.hear("opaque synthetic adult", "old"); h.wait_calls(1); h.wait_actions(2);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    Check(elapsed >= 1900 && elapsed < 2200, "two-second deadline unchanged");
    const auto action = h.last(); Check(action.identity_pending && action.restart && action.after_reply && action.greeting=="Hi! Are you Amy or Ben? Or a grown-up?", "timeout retries after first attempt");
    Harness::check_no_history(action); h.release(0, "adult"); h.network_barrier();
    Check(h.count() == 2, "late classifier after timeout"); result["timeout_elapsed_ms"] = elapsed;
  });
  run("unknown_twice_then_guest_without_history", []{
    Harness h; h.hear("opaque first", "first"); h.wait_calls(1); h.release(0, "evil"); h.wait_actions(2);
    const auto retry=h.last(); Check(retry.identity_pending && retry.restart && retry.after_reply && retry.greeting=="Hi! Are you Amy or Ben? Or a grown-up?", "first unclear answer retries");
    Harness::check_no_history(retry);h.hear("opaque second", "second");h.wait_calls(2);h.release(1,"unknown");h.wait_actions(3);
    Check(!h.last().identity_pending && h.last().restart && h.last().greeting.find("Let's play! ")==0 && h.last().after_reply,"second unknown becomes guest with greeting");
    h.hear("opaque third","third");h.network_barrier();Check(h.count()==3 && h.call_count()==2,"guest stops asking");
    h.hear("我是大人","adult-correction");h.wait_actions(4);Harness::check_no_history(h.last());
    h.hear("我是 Ben","child-correction");h.wait_actions(5);Harness::check_child(h.last(),"Ben");
    Check(h.svc->End(1),"end queued");Check(h.svc->Begin(2),"new session queued");h.wait_actions(6);
  });
  run("unresolved_end_resets_retry_count", []{
    Harness h;h.hear("opaque first","first");h.wait_calls(1);h.release(0,"unknown");h.wait_actions(2);
    Check(h.svc->End(1),"end queued");Check(h.svc->Begin(2),"new identity session queued");h.wait_actions(3);
    h.hear("opaque second","second",2);h.wait_calls(2);h.release(1,"unknown");h.wait_actions(4);
    Check(h.last().identity_pending && h.last().greeting=="Hi! Are you Amy or Ben? Or a grown-up?","new identity session starts from attempt one");
  });
  run("classifier_exit_keeps_safe_exit", []{
    Harness h; h.hear("opaque", "first"); h.wait_calls(1); h.release(0, "exit"); h.wait_actions(2);
    Check(h.last().exit && !h.last().restart, "classifier exit action"); Harness::check_no_history(h.last());
  });
  run("ordinary_chat_keeps_Amy", []{
    Harness h;h.hear("Amy","first");h.wait_actions(2);Harness::check_child(h.last(),"Amy");
    int id=0;
    for(const char* text:{"我是 Emmy","我是 Aimee","我是 Emmy","我是艾米","我是 Ben 的同学","It's a cat."}) {
      h.hear(text,("chat-"+std::to_string(++id)).c_str());h.network_barrier();
      Check(h.count()==2,"ordinary chat creates no identity action");
      Check(h.call_count()==0,"ordinary chat does not classify");
      Harness::check_child(h.last(),"Amy");
    }
  });
  for(const char* claim:{"不是 Amy","我是奶奶","我是 Ben"}) run(std::string("child_handover_")+claim,[=]{
    Harness h;h.hear("Amy","first");h.wait_actions(2);Harness::check_child(h.last(),"Amy");
    h.hear("SYNTHETIC_OLD_CONTEXT","old-context");
    h.hear(claim,"handover");h.wait_actions(3);const auto a=h.last();
    Check(!a.after_reply && !a.greeting.empty(),"child handover stops old reply then greets");
    Check(a.instructions.find("AMY_PRIVATE_SYNTHETIC_HISTORY")==std::string::npos && a.instructions.find("SYNTHETIC_OLD_CONTEXT")==std::string::npos,"old child's history and context cleared");
    if(std::string(claim)!="我是 Ben")Harness::check_no_history(a);
    Check(h.call_count()==0,"explicit handover does not classify or add aliases");
    Check(h.svc->End(1),"end");std::this_thread::sleep_for(30ms);
    Check(h.svc->Begin(2),"rewake");h.wait_actions(4);
    if(std::string(claim)=="我是 Ben")Check(h.last().greeting=="Hi Ben, want to continue?","short rewake names retained child");
  });
  result["failures"] = failures; std::cout << result.dump(2) << std::endl; return failures ? 1 : 0;
}

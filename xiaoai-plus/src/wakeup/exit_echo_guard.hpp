#pragma once
#include <mutex>
#include <string>
#include "app/conversation_policy.hpp"

namespace xiaoai_plus::wakeup {
// The only exit word on this build is 退出. Distinct internal labels preserve
// the unchanged on-device keyword file.
inline std::string StrongExitWord(const std::string& label) {
  if (label.rfind("强退:", 0) == 0) return label.substr(std::string("强退:").size());
  return {};
}
inline std::string StrongExitLabel(const std::string& line) {
  for (const auto& pair : {std::pair<const char*, const char*>{"t uì ch ū", "退出"}})
    if (line.rfind(pair.first, 0) == 0) return std::string("强退:") + pair.second;
  return {};
}
class ExitEchoGuard {
 public:
  void Reset() { std::lock_guard<std::mutex> l(mu_); Clear(); }
  void Update(const std::string& id, const std::string& text, bool done, bool created=false) {
    std::lock_guard<std::mutex> l(mu_);
    if (created || (!id.empty() && id != id_)) Clear();
    if (!id.empty()) id_=id;
    // Keep flags for the complete reply; only a small suffix is needed for
    // words split across deltas. No conversation text is persisted or logged.
    if (done) { seen_=false; tail_.clear(); }
    auto joined=tail_+app::NormalizeConversationCommand(text);
    if (joined.find("退出")!=std::string::npos) seen_=true;
    tail_=joined.substr(joined.size()>32?joined.size()-32:0);
  }
  bool Contains(const std::string& label) {
    std::lock_guard<std::mutex> l(mu_);
    return StrongExitWord(label)=="退出" && seen_;
  }
 private:
  void Clear() { id_.clear();tail_.clear();seen_=false; }
  std::mutex mu_;
  std::string id_,tail_;
  bool seen_{false};
};
} // namespace xiaoai_plus::wakeup

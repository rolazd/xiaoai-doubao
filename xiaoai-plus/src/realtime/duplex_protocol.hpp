#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace xiaoai_plus::realtime::duplex {

inline constexpr const char* kUrl =
    "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue";
inline constexpr const char* kModel = "1.2.6.1";
inline constexpr int kInputRate = 16000;
inline constexpr int kOutputRate = 24000;
inline constexpr size_t kInput20msBytes = 640;

enum class State { kIdle, kAwaitingCreated, kActive, kClosing, kClosed, kFailed };

struct IdField {
  enum class State { kAbsent, kEmpty, kValid, kInvalid, kConflict };
  State state{State::kAbsent};
  std::string path;
  bool valid() const { return state==State::kValid; }
};

struct Event {
  std::string type;
  std::string text;
  std::string session_id;
  std::string event_id, item_id, question_id, response_id;
  IdField event_field, item_field, question_field, response_field;
  bool correlation_proven{true}; // Legacy contract; fresh-session ingress sets false.
  int content_index{0};
  std::vector<uint8_t> audio;
  bool exit_intent{false};
  bool warning{false};
  std::optional<uint32_t> error_code;
  std::string log_id;
  bool usage_present{false};
  std::optional<uint64_t> total_tokens;
  std::optional<uint64_t> input_tokens;
  std::optional<uint64_t> output_tokens;
  std::vector<std::pair<std::string, uint64_t>> usage_numbers;
  std::optional<double> estimated_yuan;
  std::optional<double> estimated_min_yuan;
  std::optional<double> estimated_max_yuan;
};

// Billing-only extraction, independent of dialogue Session state. No IDs,
// ASR, output or state transitions are authorized by this entry.
Event ReadUsageOnly(const std::string& json_text);
// Billing provenance only; recognizes done/output ID fields without decoding
// output, authorizing an owner or touching any Session state.
Event ReadBillingObservation(const std::string& json_text);

// Pure protocol component. It never opens a socket, reads a key or plays audio.
class Session {
 public:
  State state() const { return state_; }
  // resume_id: a previous session.id; the provider replays that conversation's recent context.
  // extra_json: object merged into extension.dialog.extra (web search, audit level).
  std::string Create(const std::string& instructions, const std::string& voice, bool child_safety = false,
                     const std::string& resume_id = "", const std::string& extra_json = "");
  const std::string& id() const { return id_; }
  std::string AudioAppend(const std::vector<uint8_t>& pcm_20ms) const;
  std::string Greeting(const std::string& text) const;
  std::string CommitInput() const;
  std::string MuteInput() const;
  std::string UnmuteInput() const;
  std::string CancelResponse() const;
  std::string Close();
  Event Receive(const std::string& json_text);
  void Fail() { state_ = State::kFailed; }

 private:
  void RequireActive() const;
  State state_{State::kIdle};
  std::string id_;
};

// Reframes recorder callbacks (90 ms on OH2P) without dropping the 10 ms tail.
class InputFramer {
 public:
  std::vector<std::vector<uint8_t>> Append(const std::vector<uint8_t>& bytes);
  std::vector<uint8_t> FlushWithSilence();
  void Reset() { pending_.clear(); }
  size_t pending_bytes() const { return pending_.size(); }

 private:
  std::deque<uint8_t> pending_;
};

}  // namespace xiaoai_plus::realtime::duplex

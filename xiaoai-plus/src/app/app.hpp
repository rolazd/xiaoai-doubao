#pragma once
#include "audio/session_cue.hpp"
#include "audio/volume.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "memory/service.hpp"
#include "input/stop_key.hpp"
#include "audio/player.hpp"
#include "app/native_wake_monitor.hpp"
#include "audio/recorder.hpp"
#include "audio/kws_preprocess.hpp"
#include "audio/kws_diagnostics.hpp"
#include "config/config.hpp"
#include "audio/aec_webrtc.hpp"
#include "audio/led_control.hpp"
#include "audio/ordinary_barge_capture.hpp"
#include "audio/shadow_webrtc.hpp"
#include "realtime/client.hpp"
#include "wakeup/gate.hpp"
#include "wakeup/local_listener.hpp"
#include "wakeup/exit_echo_guard.hpp"
#include "wakeup/trigger.hpp"

namespace xiaoai_plus::app {

enum class Lifecycle {
  kInit,
  kRunning,
  kStopping,
  kStopped,
};

class App {
 public:
  explicit App(config::Config cfg, bool kws_only = false, bool trial_once = false,
               bool kws_diagnostics = false, int kws_s32_shift = 8,
               int kws_input_channel = -1);
  ~App();

  bool Run();
  void Stop();

  Lifecycle State() const;

 private:
  void RecordUplinkLevel(const std::vector<uint8_t>& pcm);
  void UplinkLevelEvent(int event); // 0 ready/reset, 1 started, 2 terminal
  std::mutex uplink_level_mu_;
  std::array<double,3000> uplink_levels_{};
  size_t uplink_level_count_{0}, uplink_samples_{0};
  double uplink_square_sum_{0}, uplink_peak_{0}, uplink_floor50_{0}, uplink_floor90_{0};
  bool uplink_measuring_{false}, uplink_turn_{false};
  friend struct AppOfflineProbe;
  static void ConvertS32ToS16(const std::vector<uint8_t>& chunk, std::vector<uint8_t>* out,
                              int shift);
  static void DownmixToMono(const std::vector<uint8_t>& chunk, int channels,
                            std::vector<uint8_t>* out);
  static void ExtractChannelS16(const std::vector<uint8_t>& chunk, int channels, int channel,
                                std::vector<uint8_t>* out);

  void OnInputAudio(const std::vector<uint8_t>& chunk);
  void OnInputAudioStamped(const std::vector<uint8_t>& chunk, const audio::CaptureStamp& stamp);
  void ProcessOrdinaryBargeCapture(const audio::CaptureStamp& stamp);
  void HandleOrdinaryBarge();
  void OnFreshEvent(const realtime::TransportEnvelope&,const realtime::duplex::Event&);
  void MaybeConfirmFreshPlaybackDrained();
  void RetireBargeOwner();
  bool StartRecorder();
  void RestartBargeCapture();
  void LogKwsDiagnostics();
  void OnAudio(const std::vector<uint8_t>& chunk);
  void OnPlaybackChunkProcessed(bool success);
  void OnPlaybackStall();
  void FailWorker(const char* name);
  void OnSetAiSpeaking(bool is_speaking);
  void OnAsrFinal(const std::string& text);
  void OnUserActivity();
  void OnSessionClosed(const std::string& reason);
  void OnChatEnded();
  void OnServerExitIntent();
  void OnNativeWake();
  void OnStopKey();
  void EndLocalConversation(const char* reason);
  void OnWakeInterrupt(const std::string& reason);
  void MaybeFinishExitIntent();
  void MaybeStartNextTurnWindow();
  void TryFinalizeFarewell();
  void ResetFarewellStateLocked();
  void BeginFarewellStateLocked();

  struct KwsTicket {
    uint64_t serial{0}, epoch{0}, capture{0}, identity{0};
    wakeup::Gate::WakeCommit wake;
    wakeup::Gate::DisarmCommit exit;
    explicit operator bool() const { return serial != 0; }
  };
  struct KwsBlock {
    std::vector<uint8_t> pcm;
    audio::CaptureStamp stamp;
    uint64_t epoch{0}, identity{0}, gate_revision{0};
  };
  void StartKwsConsumer();
  void KwsConsumerLoop();
  void InvalidateKws(uint64_t expected_epoch = 0);
  void EnqueueKws(const std::vector<uint8_t>&,const audio::CaptureStamp&);
  bool KwsBlockCurrentLocked(const KwsBlock&) const;
  bool KwsTicketCurrentLocked(const KwsTicket&) const;
  bool KwsTicketCurrent(const KwsTicket&) const;
  void DispatchKwsDecision(const KwsBlock&,const wakeup::LocalListener::Decision&);
  void OnKwsEnd(const KwsTicket&);
  void AfterDisarm(const std::string&);
  void AfterDisarmScoped(const std::string&,uint64_t);
  void OnWakeInterrupt(const std::string& reason,const KwsTicket& ticket);
  void OnArm(const std::string& reason);
  void OnArm(const std::string& reason,const KwsTicket& ticket);

  void QueueSessionClose();
  void QueueSessionStart();
  void QueueSessionStart(const KwsTicket& ticket);
  void QueueMemoryAction(memory::Action action);
  void ControlLoop();
  void PlaybackControlLoop();
  void InterruptPlayback(audio::SessionCue cue = audio::SessionCue::kNone);
  void PlaySessionCue();

  config::Config cfg_;
  audio::VolumeControl volume_;
  bool kws_only_{false};
  bool trial_once_{false};
  bool kws_diagnostics_{false};
  int kws_s32_shift_{16};
  int kws_input_channel_{-1};

  mutable std::mutex state_mu_;
  Lifecycle lifecycle_{Lifecycle::kInit};

  std::unique_ptr<audio::ArecordRecorder> recorder_;
  std::unique_ptr<audio::KwsDiagnostics> kws_stats_;
  std::chrono::steady_clock::time_point last_kws_diag_log_{};
  std::unique_ptr<audio::AplayPlayer> player_;
  std::unique_ptr<realtime::Client> client_;
  std::unique_ptr<realtime::Client> fresh_client_;
  std::unique_ptr<audio::shadow::PairedWebrtcAec> barge_aec_;
  std::unique_ptr<audio::shadow::WebrtcVoiceDetector> barge_vad_;
  std::unique_ptr<audio::OrdinaryBargeCapture> barge_capture_;
  std::recursive_mutex transport_effect_mu_;
  void OnSmartTurn(const realtime::duplex::Event& event);
  void CheckSmartTurn();
  bool smart_turn_active_{false}, smart_turn_sent_{false};
  std::string smart_turn_text_;
  std::vector<std::string> smart_turn_words_;
  std::chrono::steady_clock::time_point smart_turn_changed_;
  int64_t smart_turn_quiet_ms_{0};
  std::atomic<bool> legacy_effects_live_{true};
  std::atomic<bool> capture_healthy_{false};
  uint64_t healthy_capture_generation_{0};
  size_t healthy_capture_deliveries_{0}; // Recorder-thread owned; reset only after its real join.
  std::atomic<bool> rephrase_speaking_{false};
  std::mutex barge_mu_;
  std::condition_variable barge_cv_;
  realtime::TransportEnvelope fresh_envelope_;
  realtime::ResumePermit fresh_permit_;
  audio::shadow::Context capture_context_;
  audio::shadow::Context barge_origin_;
  std::deque<audio::CapturedVoiceFrame> barge_pcm_;
  std::string fresh_session_id_;
  uint64_t capture_last_ns_{0}, capture_last_generation_{0};
  uint64_t echo_path_log_ns_{0};
  uint64_t transport_sequence_{1}, input_sequence_{1}, barge_playback_{0};
  uint64_t playback_owner_generation_{1};
  bool fresh_mode_{false}, fresh_granted_{false}, fresh_done_{false};
  bool fresh_audio_done_{false}, fresh_output_started_{false}, fresh_input_ended_{false};
  size_t fresh_silence_frames_{0};
  bool fresh_seen_voice_{false};
  bool barge_collecting_{false}, barge_buffer_failed_{false}, usage_inherited_{false};
  bool barge_requested_{false}, capture_restart_requested_{false};
  uint64_t capture_fault_generation_{0};
  uint64_t identity_session_sequence_{0};
  std::chrono::steady_clock::time_point capture_restart_window_{};
  size_t capture_restart_attempts_{0};
  std::vector<uint8_t> barge_cleaned_buf_;
  audio::CaptureStamp current_capture_stamp_;
  std::unique_ptr<memory::Service> memory_;
  std::atomic<uint64_t> memory_epoch_{0};
  std::atomic<bool> memory_switching_{false};
  std::atomic<bool> identity_pending_{true};
  std::deque<memory::Action> memory_actions_;
  std::unique_ptr<dsp::AecWebrtc> aec_;
  // Stock light ring via ledd (wake / listening / speaking, level-animated).
  std::unique_ptr<audio::LedController> led_;

  std::unique_ptr<wakeup::Gate> gate_;
  std::unique_ptr<wakeup::Trigger> trigger_;
  std::unique_ptr<wakeup::LocalListener> kws_listener_;
  std::unique_ptr<NativeWakeMonitor> native_wake_monitor_;
  std::unique_ptr<input::StopKey> stop_key_;
  // dispatch fence never nests KWS queue/Listener locks, inference or hooks.
  mutable std::mutex kws_dispatch_mu_;
  uint64_t kws_epoch_{1}, kws_capture_generation_{0}, kws_serial_{0}, kws_delivery_ns_{0};
  KwsTicket kws_live_ticket_;
  bool kws_suspended_{false}, kws_disarm_pending_{false};
  std::atomic<bool> kws_accepting_{false};
  std::mutex kws_queue_mu_;
  std::condition_variable kws_queue_cv_;
  std::deque<KwsBlock> kws_queue_; // <=3 x90ms; one <=90ms block in consumer.
  bool kws_consumer_stop_{false};
  std::thread kws_consumer_thread_;

  std::mutex mu_;
  bool is_ai_speaking_{false};
  bool stream_audio_done_{false};
  bool reply_pending_{false};
  bool turn_complete_pending_{false};
  bool farewell_pending_{false};
  bool farewell_chat_ended_{false};
  bool farewell_tts_started_{false};
  bool exit_after_playback_{false};
  size_t pending_playback_chunks_{0};

  // Reused capture-thread buffers to reduce per-frame allocations.
  std::vector<uint8_t> s16_buf_;
  std::vector<uint8_t> capture_mono_buf_;
  std::vector<uint8_t> kws_mono_buf_;
  audio::KwsAgc kws_agc_;
  wakeup::ExitEchoGuard exit_echo_guard_;
  std::vector<uint8_t> reverse_ref_buf_;

  std::atomic<bool> running_{false};
  std::atomic<bool> recorder_failed_{false};
  std::atomic<bool> session_ready_{false};
  std::atomic<bool> stopping_{false};
  std::mutex run_mu_;
  std::condition_variable run_cv_;
  std::thread control_thread_;
  std::mutex control_mu_;
  std::condition_variable control_cv_;
  bool control_stop_{false};
  bool close_requested_{false};
  bool rephrase_requested_{false};
  std::string rephrase_text_{"请再说一次。"};
  bool start_requested_{false};
  KwsTicket start_ticket_;
  std::thread playback_thread_;
  std::mutex playback_mu_;
  std::condition_variable playback_cv_;
  bool playback_stop_{false};
  bool interrupt_requested_{false};
  std::atomic<audio::SessionCue> cue_requested_{audio::SessionCue::kNone};
  uint64_t interrupt_generation_{0};
};

}  // namespace xiaoai_plus::app

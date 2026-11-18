#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "realtime/duplex_protocol.hpp"

using xiaoai_plus::realtime::duplex::Session;
using xiaoai_plus::realtime::duplex::State;
using xiaoai_plus::realtime::duplex::InputFramer;

int main() {
  InputFramer framer;
  std::vector<uint8_t> two_callbacks(5760);
  for (size_t i = 0; i < two_callbacks.size(); ++i)
    two_callbacks[i] = static_cast<uint8_t>(i % 251);
  auto first = framer.Append(std::vector<uint8_t>(two_callbacks.begin(),
                                                  two_callbacks.begin() + 2880));
  assert(first.size() == 4 && framer.pending_bytes() == 320);
  auto second = framer.Append(std::vector<uint8_t>(two_callbacks.begin() + 2880,
                                                   two_callbacks.end()));
  assert(second.size() == 5 && framer.pending_bytes() == 0);
  std::vector<uint8_t> reconstructed;
  for (const auto& frame : first) reconstructed.insert(reconstructed.end(), frame.begin(), frame.end());
  for (const auto& frame : second) reconstructed.insert(reconstructed.end(), frame.begin(), frame.end());
  assert(reconstructed == two_callbacks);
  auto partial = framer.Append(std::vector<uint8_t>(320, 7));
  assert(partial.empty());
  auto padded = framer.FlushWithSilence();
  assert(padded.size() == 640 && framer.pending_bytes() == 0);
  for (size_t i = 0; i < 320; ++i) assert(padded[i] == 7);
  for (size_t i = 320; i < 640; ++i) assert(padded[i] == 0);

  Session session;
  const auto create = nlohmann::json::parse(session.Create("你是豆包。", "zh_female_vv_jupiter_bigtts"));
  assert(create.at("type") == "session.create");
  assert(create.at("session").at("model") == "1.2.6.1");
  assert(create.at("session").at("audio").at("input").at("format").at("rate") == 16000);
  assert(create.at("session").at("audio").at("output").at("format").at("type") == "pcm");
  assert(create.at("session").at("audio").at("output").at("format").at("rate") == 24000);
  assert(!create.at("session").contains("extension"));
  assert(session.state() == State::kAwaitingCreated);

  bool rejected = false;
  try { (void)session.AudioAppend(std::vector<uint8_t>(640)); }
  catch (const std::logic_error&) { rejected = true; }
  assert(rejected);

  auto created = session.Receive(R"({"type":"session.created","session":{"id":"test-sid"}})");
  assert(session.id() == "test-sid");
  {
    Session resumed;
    const auto c = nlohmann::json::parse(resumed.Create("x", "v", false, "old-sid",
        R"({"enable_volc_websearch":true,"volc_websearch_api_key":"k"})"));
    assert(c.at("session").at("id") == "old-sid");
    assert(c.at("session").at("extension").at("dialog").at("extra").at("enable_volc_websearch") == true);
    Session child;
    const auto k = nlohmann::json::parse(child.Create("x", "v", true, "old-sid", R"({"enable_volc_websearch":true})"));
    assert(!k.at("session").contains("id"));
    assert(k.at("session").at("extension").at("dialog").at("extra").at("strict_audit") == true);
    assert(k.at("session").at("extension").at("dialog").at("extra").at("enable_volc_websearch") == false);
  }
  assert(created.session_id == "test-sid" && session.state() == State::kActive);
  auto empty_audio = session.Receive(R"({"type":"response.output_audio.delta","delta":""})");
  assert(empty_audio.warning && empty_audio.audio.empty() && session.state() == State::kActive);
  auto odd_audio = session.Receive(R"({"type":"response.output_audio.delta","delta":"AQID"})");
  assert(odd_audio.warning && odd_audio.audio.size() == 2 && session.state() == State::kActive);

  std::vector<uint8_t> input(640);
  for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<uint8_t>(i);
  const auto append = nlohmann::json::parse(session.AudioAppend(input));
  assert(append.at("type") == "input_audio_buffer.append");
  assert(append.at("audio").get<std::string>().size() == 856);
  assert(nlohmann::json::parse(session.CommitInput()).at("type") == "input_audio_buffer.commit");
  assert(nlohmann::json::parse(session.MuteInput()).at("type") == "input_audio_mute.commit");
  assert(nlohmann::json::parse(session.UnmuteInput()).at("type") == "input_audio_unmute.commit");
  assert(nlohmann::json::parse(session.CancelResponse()).at("type") == "response.cancel");
  assert(nlohmann::json::parse(session.Greeting("在")).at("text") == "在");

  session.Receive(R"({"type":"conversation.item.input_audio_transcription.started"})");
  auto short_asr=session.Receive(R"({"type":"input_audio_transcription.completed","text":"再见"})");
  assert(short_asr.type=="conversation.item.input_audio_transcription.completed" && short_asr.text=="再见");
  auto asr = session.Receive(R"({"type":"conversation.item.input_audio_transcription.completed","transcript":"测试"})");
  assert(asr.text == "测试");
  session.Receive(R"({"type":"response.output_text.delta","delta":"你好"})");
  auto audio = session.Receive(R"({"type":"response.output_audio.delta","delta":"AQIDBA=="})");
  assert((audio.audio == std::vector<uint8_t>{1, 2, 3, 4}));
  auto done = session.Receive(R"({"type":"response.output_audio.done","status_code":"20000002"})");
  assert(done.exit_intent);
  auto usage = session.Receive(R"({"type":"response.done","usage":{"total_tokens":1}})");
  assert(usage.usage_present && usage.total_tokens == 1);
  auto nested_usage = session.Receive(R"({"type":"response.done","response":{"usage":{"input_tokens":2}}})");
  assert(nested_usage.usage_present && nested_usage.input_tokens == 2);
  auto detailed = session.Receive(R"({"type":"response.done","usage":{"total_tokens":15,"input_tokens":6,"output_tokens":9,"input_token_details":{"text_tokens":2,"audio_tokens":4,"cached_tokens":1,"cached_text_tokens":1,"cached_audio_tokens":0,"transcript":"PRIVATE"},"output_token_details":{"text_tokens":3,"audio_tokens":6},"secret":"PRIVATE"}})");
  assert(detailed.usage_numbers.size() == 10);
  for (const auto& [key, count] : detailed.usage_numbers) {
    assert(key.find("secret") == std::string::npos && key.find("transcript") == std::string::npos);
    (void)count;
  }
  assert(detailed.estimated_yuan && *detailed.estimated_yuan > 0.002);
  auto incomplete = session.Receive(R"({"type":"response.done","usage":{"total_tokens":10,"unknown":{"audio_tokens":999},"input_text_tokens":"8"}})");
  assert(incomplete.usage_numbers.size() == 1 && incomplete.estimated_min_yuan && incomplete.estimated_max_yuan);
  assert(session.state() == State::kActive);  // Usage is not a session-close or audio-end event.
  assert(nlohmann::json::parse(session.Close()).at("type") == "session.close");
  session.Receive(R"({"type":"session.closed"})");
  assert(session.state() == State::kClosed);

  Session invalid;
  invalid.Create("test", "voice");
  invalid.Receive(R"({"type":"session.created","session":{"id":"x"}})");
  rejected = false;
  try { invalid.Receive(R"({"type":"response.output_audio.delta","delta":"AQ=Z"})"); }
  catch (const std::runtime_error&) { rejected = true; }
  assert(rejected);
  rejected = false;
  try { invalid.Receive(R"({"type":"response.output_audio.delta","delta":"T2dnUw=="})"); }
  catch (const std::runtime_error&) { rejected = true; }
  assert(rejected);
  rejected = false;
  try { (void)invalid.AudioAppend(std::vector<uint8_t>(638)); }
  catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);
  invalid.Receive(R"({"type":"error","error":{"code":401}})");
  assert(invalid.state() == State::kFailed);
  Session early;
  early.Create("test", "voice");
  assert(early.Receive(R"({"type":"response.done"})").warning);
  assert(early.state() == State::kAwaitingCreated);
  early.Receive(R"({"type":"session.created","session":{"id":"early"}})");
  assert(!early.Receive(R"({"type":"session.closed"})").warning);
  assert(early.state() == State::kClosed);
  Session server_error;
  server_error.Create("test", "voice");
  auto error = server_error.Receive(R"({"type":"error","X-Tt-Logid":"offline-id","error":{"code":401,"message":"sensitive"}})");
  assert(error.error_code == 401 && error.log_id == "offline-id");
  assert(server_error.state() == State::kFailed);
  Session string_error;
  string_error.Create("test", "voice");
  auto string_code = string_error.Receive(R"({"type":"error","error":{"status_code":"20000002","X-Tt-Logid":"nested-id"}})");
  assert(string_code.error_code == 20000002 && string_code.log_id == "nested-id");
  Session idle_release;
  idle_release.Create("offline", "offline");
  idle_release.Receive(R"({"type":"session.created","session":{"id":"offline"}})");
  auto released = idle_release.Receive(R"({"type":"error","error":{"code":45000003}})");
  assert(released.error_code == 45000003);
  assert(idle_release.state() == State::kClosed);
}

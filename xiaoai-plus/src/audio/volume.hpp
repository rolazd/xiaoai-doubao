#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <sys/stat.h>

namespace xiaoai_plus::audio {

// The device's Playback PCM and native player share this ALSA control.
// Levels are kept as percentages so any mixer range works (0..25, 0..100,
// 0..255, ...).
class VolumeControl {
 public:
  explicit VolumeControl(std::string state = "volume", std::string card = "0",
                         std::string control = "mysoftvol")
      : state_(std::move(state)), card_(std::move(card)),
        control_(std::move(control)) {}

  static int Command(std::string text) {
    for (char& c : text) if (static_cast<unsigned char>(c) < 128)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(0, 1);
    for (const char* prefix : {"请你", "请", "麻烦你", "麻烦", "可以", "能不能", "再", "please "})
      if (text.rfind(prefix, 0) == 0) { text.erase(0, std::char_traits<char>::length(prefix)); break; }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(0, 1);
    for (const auto& item : {std::pair<const char*, int>{"大声点", 1}, {"大声一点", 1},
         {"声音大一点", 1}, {"声音大点", 1}, {"音量调大", 1}, {"调大音量", 1},
         {"加大音量", 1}, {"声音调大", 1}, {"小声点", -1}, {"小声一点", -1}, {"声音小一点", -1},
         {"声音小点", -1}, {"音量调小", -1}, {"调小音量", -1}, {"减小音量", -1},
         {"声音调小", -1}, {"louder", 1}, {"quieter", -1}, {"speak louder", 1}, {"speak quieter", -1}}) {
      const std::string prefix(item.first);
      if (text.rfind(prefix, 0) != 0) continue;
      // English words need a boundary: "loudermouth" is not a command.
      if (static_cast<unsigned char>(prefix.front()) >= 128 || text.size() == prefix.size() ||
          !std::isalpha(static_cast<unsigned char>(text[prefix.size()]))) return item.second;
    }
    return 0;
  }

  static int Next(int current, int direction) {
    constexpr std::array<int, 5> levels{56, 67, 78, 89, 100};
    if (direction > 0) {
      const auto it = std::upper_bound(levels.begin(), levels.end(), current);
      return it == levels.end() ? 100 : *it;
    }
    const auto it = std::lower_bound(levels.begin(), levels.end(), current);
    // Never turn a quieter native setting up in response to "quieter".
    return it == levels.begin() ? std::min(current, levels.front()) : *std::prev(it);
  }

  // Returns the control's current percentage, or -1 when unavailable.
  int Read() const {
    const std::string cmd =
        "amixer -c " + card_ + " sget '" + control_ + "' 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return -1;
    char line[512];
    int percent = -1;
    while (fgets(line, sizeof(line), pipe)) {
      const char* p = std::strchr(line, '[');
      while (p) {
        int value = -1;
        if (std::sscanf(p, "[%d%%]", &value) == 1 && value >= 0 && value <= 100) {
          percent = value;
          break;
        }
        p = std::strchr(p + 1, '[');
      }
      if (percent >= 0) break;
    }
    return pclose(pipe) == 0 ? percent : -1;
  }

  bool Initialize() {
    int saved = -1; std::ifstream(state_) >> saved;
    if (saved >= 0 && saved <= 100) return Write(saved);
    return Snapshot();
  }

  bool Adjust(int direction) {
    const int current = Read();
    if (current < 0 || !direction) return false;
    const int next = Next(current, direction);
    return Write(next) && Save(next);
  }

  bool Snapshot() { const int current = Read(); return current >= 0 && Save(current); }

 private:
  bool Write(int percent) const {
    const std::string cmd = "amixer -q -c " + card_ + " sset '" + control_ + "' " +
        std::to_string(percent) + "% 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    return pipe && pclose(pipe) == 0;
  }

  bool Save(int percent) {
    const std::string temp = state_ + ".tmp";
    { std::ofstream out(temp); if (!out) return false; out << percent << '\n'; out.close();
      if (!out || chmod(temp.c_str(), 0600) != 0) { std::remove(temp.c_str()); return false; } }
    if (std::rename(temp.c_str(), state_.c_str()) == 0) return true;
    std::remove(temp.c_str()); return false;
  }

  std::string state_;
  std::string card_;
  std::string control_;
};
}  // namespace xiaoai_plus::audio

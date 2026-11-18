#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace xiaoai_plus::audio {

// Drives the speaker's own light ring through the stock ledd daemon, the same way the native
// assistant does (reverse engineered from /usr/sbin/ledd and /usr/bin/mipns-xiaomi on OH2P):
//   ubus call led show '{"L":<id>,"pos":0}' / ubus call led shut '{"L":<id>}'
//   ids: 1 = wake (mipns, pos = direction), 41 = listening, 3 = speaking.
//   UDP 127.0.0.1:7777, one byte 0..100 = current loudness; ledd animates the active
//   listen (41) or speaking (3) effect from it.
// ubus calls run on a worker thread; the audio thread only posts a mode and a level.
class LedController {
 public:
  enum class Mode { kOff, kWake, kListen, kSpeak };

  LedController() {
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(7777);
    addr_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    worker_ = std::thread([this] { Run(); });
  }
  ~LedController() {
    {
      std::lock_guard<std::mutex> l(mu_);
      stop_ = true;
      want_ = Mode::kOff;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (fd_ >= 0) close(fd_);
  }

  void SetMode(Mode m) {
    {
      std::lock_guard<std::mutex> l(mu_);
      if (want_ == m) return;
      want_ = m;
    }
    cv_.notify_all();
  }

  // rms of the current S16 block; floor_db/span_db map it to 0..100.
  void Level(double rms, double floor_db, double span_db) {
    if (fd_ < 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_level_ < std::chrono::milliseconds(80)) return;
    last_level_ = now;
    const double db = 20.0 * std::log10(std::max(rms, 1.0));
    const auto v = static_cast<uint8_t>(std::clamp((db - floor_db) * 100.0 / span_db, 0.0, 100.0));
    (void)sendto(fd_, &v, 1, MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&addr_), sizeof addr_);
  }

 private:
  static int Id(Mode m) { return m == Mode::kWake ? 1 : m == Mode::kListen ? 41 : m == Mode::kSpeak ? 3 : 0; }
  static void Ubus(const std::string& verb, int id) {
    const std::string arg = verb == "show" ? "{\"L\":" + std::to_string(id) + ",\"pos\":0}"
                                           : "{\"L\":" + std::to_string(id) + "}";
    const std::string cmd = "ubus -t 1 call led " + verb + " '" + arg + "' >/dev/null 2>&1";
    (void)std::system(cmd.c_str());
  }

  void Run() {
    Mode shown = Mode::kOff;
    std::unique_lock<std::mutex> l(mu_);
    while (true) {
      cv_.wait(l, [&] { return stop_ || want_ != shown; });
      const Mode next = want_;
      const bool stop = stop_;
      l.unlock();
      if (next != shown) {
        if (shown != Mode::kOff) Ubus("shut", Id(shown));
        if (next != Mode::kOff) Ubus("show", Id(next));
        shown = next;
      }
      l.lock();
      if (stop && want_ == shown) break;
    }
  }

  int fd_{-1};
  sockaddr_in addr_{};
  std::chrono::steady_clock::time_point last_level_{};
  std::mutex mu_;
  std::condition_variable cv_;
  Mode want_{Mode::kOff};
  bool stop_{false};
  std::thread worker_;
};

}  // namespace xiaoai_plus::audio

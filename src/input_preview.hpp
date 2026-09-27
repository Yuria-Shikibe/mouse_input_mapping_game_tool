#pragma once
#include "input_monitor.hpp"
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace mouse_mapping {
// Runs the production XY state machine against an in-memory output sink.
// No driver, hooks, keyboard injection, or runtime instance mutex is involved.
class input_preview {
public:
    ~input_preview();
    void start(const configuration& config, HWND owner);
    void stop() noexcept;
    void poll(std::vector<monitor_event>& events, std::string& error);
    bool running() const noexcept { return running_.load(); }
private:
    void run(std::stop_token stop, configuration config, HWND owner) noexcept;
    void record(monitor_event_kind kind, std::int32_t x = 0, std::int32_t y = 0) noexcept;
    direction_observer observer(bool y) noexcept;
    struct observer_context { input_preview* owner; bool y; };
    observer_context x_context_{this, false}, y_context_{this, true};
    std::mutex mutex_;
    std::deque<monitor_event> events_;
    std::string error_;
    std::atomic<bool> running_{false}, recording_failed_{false};
    unsigned state_ = 0;
    std::uint64_t sequence_ = 0;
    std::jthread worker_;
};
}

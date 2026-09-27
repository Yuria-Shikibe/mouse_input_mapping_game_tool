#pragma once
#include "config.hpp"
#include <windows.h>
#include <vector>

namespace mouse_mapping {

inline constexpr std::size_t monitor_capacity = 262144;
enum class monitor_event_kind : std::uint64_t { snapshot, motion, intent, absolute, stopped };

// Every record includes a complete intent snapshot, so a reader can recover
// after a ring overrun without inventing transitions in the missing interval.
struct monitor_event {
    std::uint64_t sequence = 0;
    double time = 0;
    std::int32_t x = 0, y = 0;
    monitor_event_kind kind = monitor_event_kind::snapshot;
    unsigned state = 0; // X intent bits 0..1, Y 2..3, ON 4, absolute seen 5.
};
struct monitor_info {
    std::uint64_t session = 0;
    double started = 0;
    unsigned pid = 0;
    bool user_mode = false;
    std::array<key_code, 4> keys{};
};
struct monitor_shared;
struct monitor_control;

double monitor_now() noexcept;

class monitor_publisher {
public:
    monitor_publisher() = default;
    ~monitor_publisher();
    monitor_publisher(const monitor_publisher&) = delete;
    monitor_publisher& operator=(const monitor_publisher&) = delete;
    void start(const configuration& config, bool user_mode) noexcept;
    void motion(std::int32_t x, std::int32_t y, bool absolute) noexcept;
    void enabled(bool value) noexcept;
    void heartbeat(time_point now) noexcept;
    bool recording() const noexcept { return shared_ != nullptr && !stopped_; }
    void stop() noexcept;
    direction_observer observer(bool y) noexcept;
private:
    void publish(monitor_event_kind kind, std::int32_t x = 0, std::int32_t y = 0) noexcept;
    void intent(bool y, direction value) noexcept;
    void begin_recording() noexcept;
    void end_recording() noexcept;
    struct observer_context { monitor_publisher* owner; bool y; };
    observer_context x_context_{this, false}, y_context_{this, true};
    HANDLE handle_ = nullptr;
    monitor_shared* shared_ = nullptr;
    std::uint64_t sequence_ = 0;
    unsigned state_ = 0;
    HANDLE control_handle_ = nullptr;
    monitor_control* control_ = nullptr;
    std::uint64_t keys_ = 0;
    bool user_mode_ = false;
    time_point next_maintenance_{};
    bool stopped_ = false;
};

class monitor_reader {
public:
    ~monitor_reader();
    monitor_reader() = default;
    monitor_reader(const monitor_reader&) = delete;
    monitor_reader& operator=(const monitor_reader&) = delete;
    // New session and overflow are reported separately from ordinary inactivity.
    void poll(std::vector<monitor_event>& events, bool& new_session, bool& gap);
    void subscribe(bool enabled) noexcept;
    const monitor_info& info() const noexcept { return info_; }
    bool connected() const noexcept { return connected_; }
private:
    HANDLE handle_ = nullptr;
    const monitor_shared* shared_ = nullptr;
    monitor_info info_;
    std::uint64_t cursor_ = 0;
    double last_time_ = 0, next_open_ = 0;
    bool connected_ = false, stopped_ = false;
    HANDLE control_handle_ = nullptr;
    monitor_control* control_ = nullptr;
    int lease_slot_ = -1;
    LONG64 lease_value_ = 0;
    ULONGLONG next_renew_ = 0;
};
} // namespace mouse_mapping

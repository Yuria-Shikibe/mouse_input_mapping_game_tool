#include "input_monitor.hpp"
#include <atomic>
#include <bit>
#include <new>

namespace mouse_mapping {
namespace {
constexpr wchar_t mapping_name[] = L"Local\\mouse_input_mapping_monitor_v3";
constexpr wchar_t control_name[] = L"Local\\mouse_input_mapping_monitor_control_v3";
constexpr std::uint64_t protocol = 3;
using word = std::atomic<std::uint64_t>;
static_assert(word::is_always_lock_free);
struct monitor_slot {
    word stamp{0}, time{0}, xy{0}, state_kind{0};
};
std::uint64_t ticks() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}
std::uint64_t frequency() noexcept {
    static const auto value = [] {
        LARGE_INTEGER result{};
        QueryPerformanceFrequency(&result);
        return static_cast<std::uint64_t>(result.QuadPart);
    }();
    return value;
}
}

// All shared fields are lock-free atomic words, including payloads. A readonly
// client only loads; no interlocked read/modify/write on FILE_MAP_READ views.
// Sequential consistency also orders the invalidation stamp before slot reuse.
struct monitor_shared {
    word session{0}, version{protocol}, pid{0}, user{0}, keys{0}, hz{0}, published{0};
    monitor_slot slots[monitor_capacity];
};

// Windows interlocked words live in a zero-initialized page-file mapping.
// Each reader owns an expiry value, renewed/released using compare-exchange.
struct monitor_control { alignas(8) volatile LONG64 leases[64]; };
namespace {
bool open_control(HANDLE& handle, monitor_control*& control) noexcept {
    if (control) return true;
    handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(monitor_control), control_name);
    if (!handle) return false;
    control = static_cast<monitor_control*>(MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(monitor_control)));
    if (!control) { CloseHandle(handle); handle = nullptr; }
    return control != nullptr;
}
}

double monitor_now() noexcept { return static_cast<double>(ticks()) / static_cast<double>(frequency()); }

monitor_publisher::~monitor_publisher() {
    stop();
    if (shared_) UnmapViewOfFile(shared_);
    if (handle_) CloseHandle(handle_);
    if (control_) UnmapViewOfFile(control_);
    if (control_handle_) CloseHandle(control_handle_);
}
void monitor_publisher::start(const configuration& config, bool user_mode) noexcept {
    user_mode_ = user_mode;
    y_enabled_ = y_enabled(config);
    keys_ = static_cast<std::uint64_t>(config.left_key)
        | (static_cast<std::uint64_t>(config.right_key) << 16)
        | (static_cast<std::uint64_t>(config.up_key) << 32)
        | (static_cast<std::uint64_t>(config.down_key) << 48);
    open_control(control_handle_, control_);
}
void monitor_publisher::begin_recording() noexcept {
    if (handle_) return;
    // Runtime instance exclusion is acquired before opening this object. An
    // old reader may keep it alive; a new session invalidates its previous data.
    handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(monitor_shared)), mapping_name);
    if (!handle_) return;
    const bool fresh = GetLastError() != ERROR_ALREADY_EXISTS;
    shared_ = static_cast<monitor_shared*>(MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(monitor_shared)));
    if (!shared_) { CloseHandle(handle_); handle_ = nullptr; return; }
    if (fresh) ::new (shared_) monitor_shared{};
    shared_->session.store(0);
    shared_->published.store(0);
    shared_->version.store(protocol);
    shared_->pid.store(GetCurrentProcessId());
    shared_->user.store((user_mode_ ? 1u : 0u) | (y_enabled_ ? 2u : 0u));
    shared_->hz.store(frequency());
    shared_->keys.store(keys_);
    sequence_ = 0;
    state_ &= (16u | 64u); // Direction observers supply a fresh snapshot on attachment.
    shared_->session.store(ticks());
    publish(monitor_event_kind::snapshot);
}
void monitor_publisher::end_recording() noexcept {
    if (shared_) {
        publish(monitor_event_kind::stopped);
        UnmapViewOfFile(shared_);
        shared_ = nullptr;
    }
    if (handle_) { CloseHandle(handle_); handle_ = nullptr; }
}
void monitor_publisher::publish(monitor_event_kind kind, std::int32_t x, std::int32_t y) noexcept {
    if (!shared_ || stopped_) return;
    const auto sequence = ++sequence_;
    auto& slot = shared_->slots[(sequence - 1) % monitor_capacity];
    slot.stamp.store(sequence * 2 - 1);
    slot.time.store(ticks());
    slot.xy.store(static_cast<std::uint32_t>(x) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) << 32));
    slot.state_kind.store(state_ | (static_cast<std::uint64_t>(kind) << 32));
    slot.stamp.store(sequence * 2);
    shared_->published.store(sequence);
}
void monitor_publisher::motion(std::int32_t x, std::int32_t y, bool absolute) noexcept {
    if (absolute) {
        if (!(state_ & 32)) { state_ |= 32; publish(monitor_event_kind::absolute); }
    } else if (x || y) publish(monitor_event_kind::motion, x, y);
}
void monitor_publisher::enabled(bool value) noexcept {
    state_ = (state_ & ~16u) | (value ? 16u : 0u);
    publish(monitor_event_kind::snapshot);
}
void monitor_publisher::bypassed(bool value) noexcept {
    state_ = (state_ & ~64u) | (value ? 64u : 0u);
    publish(monitor_event_kind::snapshot);
}
void monitor_publisher::heartbeat(time_point now) noexcept {
    if (stopped_ || now < next_maintenance_) return;
    next_maintenance_ = now + milliseconds(50);
    if (!open_control(control_handle_, control_)) return;
    const auto current = static_cast<LONG64>(GetTickCount64());
    bool wanted = false;
    for (auto& lease : control_->leases)
        if (InterlockedCompareExchange64(&lease, 0, 0) > current) { wanted = true; break; }
    if (wanted) {
        if (!shared_) begin_recording();
        else publish(monitor_event_kind::snapshot);
    } else end_recording();
}
void monitor_publisher::stop() noexcept {
    if (stopped_) return;
    state_ &= ~16u;
    publish(monitor_event_kind::stopped);
    stopped_ = true;
}
void monitor_publisher::intent(bool y, direction value) noexcept {
    const auto shift = y ? 2u : 0u;
    const auto bits = value == direction::left ? 1u : value == direction::right ? 2u : 0u;
    state_ = (state_ & ~(3u << shift)) | (bits << shift);
    publish(monitor_event_kind::intent);
}
direction_observer monitor_publisher::observer(bool y) noexcept {
    if (!recording()) return {};
    return {y ? &y_context_ : &x_context_, [](void* pointer, direction value) noexcept {
        const auto& context = *static_cast<observer_context*>(pointer);
        context.owner->intent(context.y, value);
    }};
}

monitor_reader::~monitor_reader() {
    subscribe(false);
    if (shared_) UnmapViewOfFile(shared_);
    if (handle_) CloseHandle(handle_);
    if (control_) UnmapViewOfFile(control_);
    if (control_handle_) CloseHandle(control_handle_);
}
void monitor_reader::subscribe(bool enabled) noexcept {
    if (!enabled) {
        if (control_ && lease_slot_ >= 0)
            InterlockedCompareExchange64(&control_->leases[lease_slot_], 0, lease_value_);
        lease_slot_ = -1; lease_value_ = 0; next_renew_ = 0;
        connected_ = false;
        if (shared_) { UnmapViewOfFile(shared_); shared_ = nullptr; }
        if (handle_) { CloseHandle(handle_); handle_ = nullptr; }
        info_.session = 0; next_open_ = 0;
        return;
    }
    const auto now = GetTickCount64();
    if (now < next_renew_) return;
    next_renew_ = now + 250;
    if (!open_control(control_handle_, control_)) return;
    const auto expiry = static_cast<LONG64>(now + 1000);
    if (lease_slot_ >= 0 && InterlockedCompareExchange64(&control_->leases[lease_slot_], expiry, lease_value_) == lease_value_) {
        lease_value_ = expiry; return;
    }
    lease_slot_ = -1;
    for (int i = 0; i < 64; ++i) {
        const auto previous = InterlockedCompareExchange64(&control_->leases[i], 0, 0);
        if (previous <= static_cast<LONG64>(now)
            && InterlockedCompareExchange64(&control_->leases[i], expiry, previous) == previous) {
            lease_slot_ = i; lease_value_ = expiry; return;
        }
    }
}
void monitor_reader::poll(std::vector<monitor_event>& events, bool& new_session, bool& gap) {
    events.clear(); new_session = false; gap = false;
    const auto now = monitor_now();
    if (!shared_) {
        if (now < next_open_) return;
        next_open_ = now + 0.5;
        handle_ = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping_name);
        if (!handle_) return;
        shared_ = static_cast<const monitor_shared*>(MapViewOfFile(handle_, FILE_MAP_READ, 0, 0, sizeof(monitor_shared)));
        if (!shared_) { CloseHandle(handle_); handle_ = nullptr; return; }
    }
    const auto session = shared_->session.load();
    const auto hz = shared_->hz.load();
    if (!session || !hz || shared_->version.load() != protocol) { connected_ = false; return; }
    if (session != info_.session) {
        info_.session = session;
        info_.started = static_cast<double>(session) / static_cast<double>(hz);
        info_.pid = static_cast<unsigned>(shared_->pid.load());
        const auto flags = shared_->user.load();
        info_.user_mode = (flags & 1) != 0;
        info_.y_enabled = (flags & 2) != 0;
        const auto keys = shared_->keys.load();
        for (unsigned i = 0; i < 4; ++i) info_.keys[i] = static_cast<key_code>(keys >> (i * 16));
        cursor_ = 0; last_time_ = 0; stopped_ = false;
        new_session = true;
    }
    const auto latest = shared_->published.load();
    // Start from current state, including when another GUI kept recording.
    // A newly subscribed view must not replay history from before it attached.
    if (new_session && latest) cursor_ = latest - 1;
    if (latest > cursor_ && latest - cursor_ > monitor_capacity) {
        cursor_ = latest - monitor_capacity;
        gap = true;
    }
    // A finite batch prevents continuous mouse traffic from starving the UI.
    for (auto sequence = cursor_ + 1; sequence <= latest; ++sequence) {
        const auto& slot = shared_->slots[(sequence - 1) % monitor_capacity];
        const auto stamp = slot.stamp.load();
        if (stamp != sequence * 2) { gap = true; break; }
        const auto time = slot.time.load(), xy = slot.xy.load(), value = slot.state_kind.load();
        if (slot.stamp.load() != stamp) { gap = true; break; }
        monitor_event event;
        event.sequence = sequence;
        event.time = static_cast<double>(time) / static_cast<double>(hz);
        event.x = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(xy));
        event.y = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(xy >> 32));
        event.state = static_cast<unsigned>(value);
        event.kind = static_cast<monitor_event_kind>(value >> 32);
        if (new_session && events.empty() && event.kind != monitor_event_kind::stopped) {
            event.kind = monitor_event_kind::snapshot;
            event.x = event.y = 0;
        }
        events.push_back(event);
        cursor_ = sequence; last_time_ = event.time;
        stopped_ = event.kind == monitor_event_kind::stopped;
    }
    if (shared_->session.load() != session) {
        events.clear(); info_.session = 0; connected_ = false; return;
    }
    connected_ = !stopped_ && last_time_ > 0 && now - last_time_ < 2.0;
}
} // namespace mouse_mapping

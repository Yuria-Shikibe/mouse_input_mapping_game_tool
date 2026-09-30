#pragma once
#include "runtime.hpp"
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <optional>
#include <system_error>

namespace mouse_mapping {
inline DWORD wait_milliseconds(time_point due, time_point now, DWORD maximum) {
    if (due <= now) return 0;
    const auto delay = std::chrono::ceil<milliseconds>(due - now).count();
    return static_cast<DWORD>(std::min<std::int64_t>(delay, maximum));
}

class input_thread_priority {
public:
    explicit input_thread_priority(input_priority priority) {
        previous_ = GetThreadPriority(GetCurrentThread());
        if (priority == input_priority::above_normal) {
            changed_ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) != FALSE;
            if (!changed_) std::cerr << "Cannot raise input thread priority; using existing priority.\n";
        }
    }
    ~input_thread_priority() {
        if (changed_ && previous_ != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(GetCurrentThread(), previous_);
    }
private:
    int previous_ = THREAD_PRIORITY_NORMAL;
    bool changed_ = false;
};

class input_waiter {
public:
    input_waiter() {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~input_waiter() { if (timer_) CloseHandle(timer_); }
    input_waiter(const input_waiter&) = delete;
    input_waiter& operator=(const input_waiter&) = delete;

    // Returns true only for game exit; the caller owns the stop flag.
    bool wait(HANDLE stop, HANDLE game, time_point due) {
        HANDLE handles[3]{stop};
        DWORD count = 1;
        if (game) handles[count++] = game;
        const auto now = clock_type::now();
        DWORD timeout = wait_milliseconds(due, now, 50);
        if (timer_ && due > now) {
            // Keep an earlier timer: moving the release deadline forward on every
            // mouse packet must not cause a timer syscall at the polling rate.
            if (!armed_ || due < *armed_) {
                LARGE_INTEGER relative{};
                relative.QuadPart = -std::max<std::int64_t>(1,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(due - now).count() / 100);
                if (!SetWaitableTimer(timer_, &relative, 0, nullptr, nullptr, FALSE)) {
                    CloseHandle(timer_); timer_ = nullptr; armed_.reset();
                } else armed_ = due;
            }
            if (timer_) { handles[count++] = timer_; timeout = INFINITE; }
        }
        const auto result = MsgWaitForMultipleObjectsEx(count, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot wait for input");
        if (timer_ && count > (game ? 2u : 1u) && result == WAIT_OBJECT_0 + count - 1) armed_.reset();
        return game && result == WAIT_OBJECT_0 + 1;
    }
private:
    HANDLE timer_ = nullptr;
    std::optional<time_point> armed_;
};
}

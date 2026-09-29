#pragma once
#include "pie_mapping.hpp"
#include <windows.h>
#include <atomic>
#include <thread>

namespace mouse_mapping {
static_assert(std::atomic<unsigned long long>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<HWND>::is_always_lock_free);
// A latest-state mailbox: the input thread never waits for presentation.
class pie_overlay {
public:
    pie_overlay() = default;
    ~pie_overlay();
    pie_overlay(const pie_overlay&) = delete;
    pie_overlay& operator=(const pie_overlay&) = delete;
    void start();
    void publish(bool visible, pie_direction selected, HWND foreground) noexcept;
private:
    void run() noexcept;
    HANDLE wake_{}, stop_{};
    std::thread thread_;
    std::atomic<bool> notified_{false};
    std::atomic<unsigned long long> sequence_{0};
    std::atomic<unsigned> state_{0};
    std::atomic<HWND> foreground_{nullptr};
};
}

#pragma once
#include "mapping.hpp"
#include <exception>
#include <map>

namespace mouse_mapping {
class button_mapping {
public:
    explicit button_mapping(std::array<key_code, 5> codes) : codes_(codes) {}

    template<class Sink>
    void update(std::uintptr_t device, unsigned short flags, Sink&& send) {
        // Raw Input button bits are DOWN/UP pairs: left, right, middle, X1, X2.
        for (std::size_t index = 0; index < codes_.size(); ++index) {
            if (flags & (1u << (index * 2))) {
                mice_[device][index] = true;
                sync(index, send);
            }
            if (flags & (2u << (index * 2))) {
                mice_[device][index] = false;
                sync(index, send);
            }
        }
    }
    template<class Sink>
    void remove(std::uintptr_t device, Sink&& send) {
        mice_.erase(device);
        sync_all(send);
    }
    template<class Sink>
    bool physical(key_event event, Sink&& send) {
        for (std::size_t index = 0; index < codes_.size(); ++index) {
            if (event.code != codes_[index]) continue;
            const bool repeat = physical_[index] && event.down;
            const bool previous = output_[index];
            physical_[index] = event.down;
            sync(index, send);
            if ((!event.down && !previous && !output_[index]) || (repeat && previous && output_[index])) send(event);
            return true;
        }
        return false;
    }
    template<class Sink>
    void release(Sink&& send) {
        mice_.clear();
        sync_all(send);
    }
private:
    template<class Sink>
    void sync_all(Sink&& send) {
        std::exception_ptr failure;
        for (std::size_t index = 0; index < codes_.size(); ++index) {
            try { sync(index, send); }
            catch (...) { if (!failure) failure = std::current_exception(); }
        }
        if (failure) std::rethrow_exception(failure);
    }
    template<class Sink>
    void sync(std::size_t index, Sink&& send) {
        bool wanted = physical_[index];
        for (const auto& entry : mice_) wanted = wanted || entry.second[index];
        if (wanted == output_[index]) return;
        send(key_event{1, codes_[index], wanted});
        output_[index] = wanted;
    }
    std::array<key_code, 5> codes_;
    std::array<bool, 5> physical_{}, output_{};
    std::map<std::uintptr_t, std::array<bool, 5>> mice_;
};
} // namespace mouse_mapping

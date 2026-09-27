#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace mouse_mapping {
// Storage survives clear/expiry. Growth preserves sample order and never drops input.
template<class T> class sample_ring {
public:
    explicit sample_ring(std::size_t capacity = 512) : data_(capacity) {}
    bool empty() const noexcept { return size_ == 0; }
    const T& front() const noexcept { return data_[head_]; }
    void pop_front() noexcept { head_ = (head_ + 1) % data_.size(); --size_; }
    void clear() noexcept { head_ = size_ = 0; }
    void reserve(std::size_t capacity) {
        if (capacity <= data_.size()) return;
        std::vector<T> next(capacity);
        for (std::size_t i = 0; i < size_; ++i) next[i] = data_[(head_ + i) % data_.size()];
        data_.swap(next);
        head_ = 0;
    }
    void push_back(T value) {
        if (size_ == data_.size()) reserve(std::max<std::size_t>(1, size_ * 2));
        data_[(head_ + size_) % data_.size()] = value;
        ++size_;
    }
private:
    std::vector<T> data_;
    std::size_t head_ = 0, size_ = 0;
};

// Usually one or two mice. Cache an index, not a pointer invalidated by growth.
template<class T> class device_table {
public:
    device_table() { entries_.reserve(8); }
    T& operator[](std::uintptr_t device) {
        if (cached_ < entries_.size() && entries_[cached_].first == device) return entries_[cached_].second;
        for (std::size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].first == device) { cached_ = i; return entries_[i].second; }
        entries_.push_back({device, {}});
        cached_ = entries_.size() - 1;
        return entries_.back().second;
    }
    void erase(std::uintptr_t device) {
        std::erase_if(entries_, [device](const auto& entry) { return entry.first == device; });
        cached_ = entries_.size();
    }
    void clear() noexcept { entries_.clear(); cached_ = 0; }
    auto begin() const noexcept { return entries_.begin(); }
    auto end() const noexcept { return entries_.end(); }
private:
    std::vector<std::pair<std::uintptr_t, T>> entries_;
    std::size_t cached_ = 0;
};
}

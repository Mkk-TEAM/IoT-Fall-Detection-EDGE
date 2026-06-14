#pragma once
#include <vector>
#include <cstddef>

/// Dynamic-capacity circular buffer.  NOT thread-safe — callers must lock.
/// Index 0 = oldest element, index size()-1 = newest element.
template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : data_(capacity), cap_(capacity) {}

    void push(const T &v) {
        if (!cap_) return;
        data_[head_] = v;
        head_ = (head_ + 1) % cap_;
        if (sz_ < cap_) ++sz_;
    }

    size_t size()     const { return sz_; }
    bool   empty()    const { return sz_ == 0; }
    size_t capacity() const { return cap_; }

    const T& at(size_t i) const { return data_[(head_ - sz_ + i + cap_) % cap_]; }
    T&       at(size_t i)       { return data_[(head_ - sz_ + i + cap_) % cap_]; }

    /// Snapshot to vector, oldest first.
    std::vector<T> snapshot() const {
        std::vector<T> v; v.reserve(sz_);
        for (size_t i = 0; i < sz_; ++i) v.push_back(at(i));
        return v;
    }

    /// Return elements where T::gw_ms is in [from_ms, to_ms] inclusive.
    std::vector<T> slice_time(int64_t from_ms, int64_t to_ms) const {
        std::vector<T> v;
        for (size_t i = 0; i < sz_; ++i) {
            const T &x = at(i);
            if (x.gw_ms >= from_ms && x.gw_ms <= to_ms) v.push_back(x);
        }
        return v;
    }

    void clear() { head_ = 0; sz_ = 0; }

private:
    std::vector<T> data_;
    size_t cap_  = 0;
    size_t head_ = 0;
    size_t sz_   = 0;
};

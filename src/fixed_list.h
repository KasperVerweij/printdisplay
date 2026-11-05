#pragma once

#include <new>
#include <stddef.h>

// A list with a fixed capacity, allocated once without throwing. Exceptions
// aren't handled on this board, so a std::vector that fails to grow calls
// abort() and reboots it; this one just stops accepting items (or is empty
// if even the first allocation failed).
template <typename T> class FixedList {
  public:
    explicit FixedList(size_t cap) : data_(new (std::nothrow) T[cap]), cap_(data_ ? cap : 0) {}
    ~FixedList() { delete[] data_; }
    FixedList(const FixedList &) = delete;
    FixedList &operator=(const FixedList &) = delete;

    bool ok() const { return data_ != nullptr; }
    bool push(const T &v) {
        if (n_ >= cap_) return false;
        data_[n_++] = v;
        return true;
    }
    size_t size() const { return n_; }
    T &operator[](size_t i) { return data_[i]; }
    const T &operator[](size_t i) const { return data_[i]; }
    T *begin() { return data_; }
    T *end() { return data_ + n_; }

  private:
    T *data_;
    size_t cap_;
    size_t n_ = 0;
};

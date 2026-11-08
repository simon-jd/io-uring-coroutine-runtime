#pragma once
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace rol {

// A heap buffer with a single owner. Operations take it by value and hand it
// back with the result, so the runtime, not the caller, owns it while the kernel
// may read or write it.
class owned_buffer {
public:
    owned_buffer() = default;
    static owned_buffer allocate(std::size_t capacity);
    static owned_buffer copy_of(std::span<const std::byte> bytes);
    static owned_buffer copy_of(std::string_view text);

    owned_buffer(owned_buffer&& other) noexcept
        : data_(std::move(other.data_)),
          capacity_(std::exchange(other.capacity_, 0)),
          size_(std::exchange(other.size_, 0)) {}
    owned_buffer& operator=(owned_buffer&& other) noexcept {
        data_ = std::move(other.data_);
        capacity_ = std::exchange(other.capacity_, 0);
        size_ = std::exchange(other.size_, 0);
        return *this;
    }

    std::span<std::byte> storage() noexcept { return {data_.get(), capacity_}; }
    std::span<const std::byte> bytes() const noexcept { return {data_.get(), size_}; }
    std::string_view view() const noexcept {
        return {reinterpret_cast<const char*>(data_.get()), size_};
    }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t size() const noexcept { return size_; }
    void resize(std::size_t n);
    // Gives up the memory without freeing it (the kernel may still use it).
    void leak() noexcept {
        (void)data_.release();
        capacity_ = size_ = 0;
    }
    explicit operator bool() const noexcept { return data_ != nullptr; }

private:
    std::unique_ptr<std::byte[]> data_;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

// Owns a file descriptor.
class unique_fd {
public:
    unique_fd() = default;
    explicit unique_fd(int fd) noexcept : fd_(fd) {}
    unique_fd(unique_fd&& other) noexcept : fd_(other.release()) {}
    unique_fd& operator=(unique_fd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;
    ~unique_fd() { reset(); }

    int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept;
    explicit operator bool() const noexcept { return fd_ >= 0; }

private:
    int fd_ = -1;
};

} // namespace rol

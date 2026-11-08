#pragma once
#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace rol {

template <class T = void>
class task;

namespace detail {

struct promise_base {
    std::coroutine_handle<> continuation;
    std::exception_ptr error;

    struct final_awaiter {
        bool await_ready() const noexcept { return false; }
        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) const noexcept {
            auto next = h.promise().continuation;
            return next ? next : std::noop_coroutine();
        }
        void await_resume() const noexcept {}
    };

    std::suspend_always initial_suspend() const noexcept { return {}; }
    final_awaiter final_suspend() const noexcept { return {}; }
    void unhandled_exception() noexcept { error = std::current_exception(); }
};

template <class T>
struct task_promise : promise_base {
    std::optional<T> value;
    task<T> get_return_object() noexcept;
    template <class U>
    void return_value(U&& v) {
        value.emplace(std::forward<U>(v));
    }
    T take() {
        if (error) std::rethrow_exception(error);
        return std::move(*value);
    }
};

template <>
struct task_promise<void> : promise_base {
    task<void> get_return_object() noexcept;
    void return_void() noexcept {}
    void take() {
        if (error) std::rethrow_exception(error);
    }
};

} // namespace detail

// A lazily started coroutine. The task object owns the frame: destroying a task
// destroys its frame and, through the awaiters inside it, detaches any I/O the
// frame was waiting on. Detached I/O keeps its buffers in the reactor until the
// kernel's completion arrives.
template <class T>
class [[nodiscard]] task {
public:
    using promise_type = detail::task_promise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    task() = default;
    explicit task(handle_type h) noexcept : h_(h) {}
    task(task&& other) noexcept : h_(std::exchange(other.h_, {})) {}
    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (h_) h_.destroy();
            h_ = std::exchange(other.h_, {});
        }
        return *this;
    }
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    ~task() {
        if (h_) h_.destroy();
    }

    bool valid() const noexcept { return static_cast<bool>(h_); }
    bool done() const noexcept { return h_ && h_.done(); }

    struct awaiter {
        handle_type h;
        bool await_ready() const noexcept { return !h || h.done(); }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
            h.promise().continuation = caller;
            return h;
        }
        T await_resume() { return h.promise().take(); }
    };
    awaiter operator co_await() & noexcept { return awaiter{h_}; }
    awaiter operator co_await() && noexcept { return awaiter{h_}; }

    handle_type release() noexcept { return std::exchange(h_, {}); }

private:
    handle_type h_;
};

namespace detail {
template <class T>
task<T> task_promise<T>::get_return_object() noexcept {
    return task<T>(std::coroutine_handle<task_promise<T>>::from_promise(*this));
}
inline task<void> task_promise<void>::get_return_object() noexcept {
    return task<void>(std::coroutine_handle<task_promise<void>>::from_promise(*this));
}
} // namespace detail

} // namespace rol

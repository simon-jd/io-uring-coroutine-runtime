#pragma once
#include "rol/task.hpp"
#include <coroutine>
#include <cstddef>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace rol {

namespace detail {

struct join_state {
    std::size_t remaining = 0;
    std::coroutine_handle<> parent;
    std::exception_ptr error;
};

// Runs one child task and, when it finishes, counts down; the last child to
// finish resumes the parent.
struct join_promise {
    join_state* state = nullptr;
    std::coroutine_handle<join_promise> get_return_object() noexcept {
        return std::coroutine_handle<join_promise>::from_promise(*this);
    }
    std::suspend_always initial_suspend() const noexcept { return {}; }
    struct final_awaiter {
        bool await_ready() const noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<join_promise> h) const noexcept {
            join_state* st = h.promise().state;
            if (--st->remaining == 0) return st->parent;
            return std::noop_coroutine();
        }
        void await_resume() const noexcept {}
    };
    final_awaiter final_suspend() const noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept {
        if (!state->error) state->error = std::current_exception();
    }
};

struct join_coro {
    using promise_type = join_promise;
    std::coroutine_handle<join_promise> h;
    join_coro(std::coroutine_handle<join_promise> handle) noexcept : h(handle) {}
    join_coro(join_coro&& o) noexcept : h(std::exchange(o.h, {})) {}
    join_coro(const join_coro&) = delete;
    ~join_coro() {
        if (h) h.destroy();
    }
};

template <class T>
join_coro join_one(task<T>& t, std::optional<T>& out) {
    out.emplace(co_await std::move(t));
}
inline join_coro join_one_void(task<void>& t) { co_await std::move(t); }

} // namespace detail

// Awaits every task concurrently on the current reactor and returns their
// results in order. The first exception thrown by a child is rethrown after all
// children have finished. The awaiter owns the child frames: if the awaiting
// coroutine is destroyed, the children are destroyed with it and their
// operations detach, keeping their buffers until the kernel is done.
template <class T>
class [[nodiscard]] when_all_awaiter {
public:
    explicit when_all_awaiter(std::vector<task<T>> tasks) : tasks_(std::move(tasks)) {}
    when_all_awaiter(const when_all_awaiter&) = delete;
    when_all_awaiter& operator=(const when_all_awaiter&) = delete;

    bool await_ready() const noexcept { return tasks_.empty(); }
    bool await_suspend(std::coroutine_handle<> parent) {
        state_.parent = parent;
        state_.remaining = tasks_.size() + 1; // +1 until every child has started
        joins_.reserve(tasks_.size());
        if constexpr (!std::is_void_v<T>) results_.resize(tasks_.size());
        for (std::size_t i = 0; i < tasks_.size(); ++i) {
            if constexpr (std::is_void_v<T>) joins_.push_back(detail::join_one_void(tasks_[i]));
            else joins_.push_back(detail::join_one(tasks_[i], results_[i]));
            joins_.back().h.promise().state = &state_;
        }
        for (auto& j : joins_) j.h.resume();
        return --state_.remaining != 0;
    }
    auto await_resume() {
        if (state_.error) std::rethrow_exception(state_.error);
        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            std::vector<T> out;
            out.reserve(results_.size());
            for (auto& r : results_) out.push_back(std::move(*r));
            return out;
        }
    }

private:
    struct no_results {
        void resize(std::size_t) {}
    };
    // Destroyed in reverse order: join frames first, then the child frames.
    std::vector<task<T>> tasks_;
    std::conditional_t<std::is_void_v<T>, no_results, std::vector<std::optional<T>>> results_;
    std::vector<detail::join_coro> joins_;
    detail::join_state state_;
};

template <class T>
when_all_awaiter<T> when_all(std::vector<task<T>> tasks) {
    return when_all_awaiter<T>(std::move(tasks));
}

} // namespace rol

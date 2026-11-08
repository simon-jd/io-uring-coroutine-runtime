#pragma once
#include <coroutine>

namespace rol::detail {

class ready_list;

// A wake-up record embedded in the object that waits (an awaiter or a promise).
// If that object is destroyed while queued, it removes itself, so the reactor
// never resumes a destroyed coroutine.
struct ready_node {
    ready_node* prev = nullptr;
    ready_node* next = nullptr;
    ready_list* owner = nullptr;
    std::coroutine_handle<> handle;

    ready_node() = default;
    ready_node(const ready_node&) = delete;
    ready_node& operator=(const ready_node&) = delete;
    inline ~ready_node();
    bool queued() const noexcept { return owner != nullptr; }
};

class ready_list {
public:
    ready_list() { head_.prev = head_.next = &head_; }
    ready_list(const ready_list&) = delete;
    ready_list& operator=(const ready_list&) = delete;
    ~ready_list() {
        while (!empty()) remove(head_.next);
    }

    bool empty() const noexcept { return head_.next == &head_; }
    void push_back(ready_node* n) noexcept {
        if (n->owner) return; // already queued
        n->owner = this;
        n->prev = head_.prev;
        n->next = &head_;
        head_.prev->next = n;
        head_.prev = n;
    }
    ready_node* pop_front() noexcept {
        if (empty()) return nullptr;
        ready_node* n = head_.next;
        remove(n);
        return n;
    }
    void remove(ready_node* n) noexcept {
        n->prev->next = n->next;
        n->next->prev = n->prev;
        n->prev = n->next = nullptr;
        n->owner = nullptr;
    }

private:
    ready_node head_;
};

inline ready_node::~ready_node() {
    if (owner) owner->remove(this);
}

} // namespace rol::detail

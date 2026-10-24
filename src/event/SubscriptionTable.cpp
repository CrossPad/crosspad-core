// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/event/SubscriptionTable.hpp"
#include <algorithm>

namespace crosspad {

namespace {
// The tables the calling thread is dispatching, innermost first: a remove()
// from inside one of their handlers must not wait for a handler to return.
struct DispatchFrame {
    const SubscriptionTable* table;
    const DispatchFrame* outer;
};
thread_local const DispatchFrame* t_dispatching = nullptr;
} // namespace

SubscriptionHandle SubscriptionTable::add(EventType type, EventHandler handler, void* userCtx) {
    auto entry = std::make_shared<Entry>();
    entry->type = type;
    entry->handler = handler;
    entry->userCtx = userCtx;
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(entry);
    return static_cast<SubscriptionHandle>(entry.get());
}

bool SubscriptionTable::dispatchingOnThisThread() const {
    for (const DispatchFrame* f = t_dispatching; f; f = f->outer) {
        if (f->table == this) return true;
    }
    return false;
}

void SubscriptionTable::remove(SubscriptionHandle handle) {
    if (!handle) return;

    std::unique_lock<std::mutex> lock(mutex_);
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [handle](const std::shared_ptr<Entry>& e) { return e.get() == handle; });
    if (it == entries_.end()) return;

    std::shared_ptr<Entry> entry = std::move(*it);
    entries_.erase(it);
    entry->active = false;

    if (dispatchingOnThisThread()) return;
    idle_.wait(lock, [&entry] { return entry->running == 0; });
}

void SubscriptionTable::dispatch(EventType type, const void* data) {
    std::vector<std::shared_ptr<Entry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& e : entries_) {
            if (e->type == type) snapshot.push_back(e);
        }
    }
    if (snapshot.empty()) return;

    const DispatchFrame frame{this, t_dispatching};
    t_dispatching = &frame;

    for (const auto& e : snapshot) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!e->active) continue;
            ++e->running;
        }
        e->handler(data, type, e->userCtx);
        std::lock_guard<std::mutex> lock(mutex_);
        if (--e->running == 0 && !e->active) idle_.notify_all();
    }

    t_dispatching = frame.outer;
}

} // namespace crosspad

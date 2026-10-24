// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file SubscriptionTable.hpp
 * @brief The subscriber list of an event bus, with IEventBus's unsubscribe
 *        contract: after remove() returns, the handler is not running and
 *        will not be called again (see IEventBus::unsubscribe()).
 *
 * dispatch() never holds the table's lock while it calls a handler, so a
 * handler may subscribe, unsubscribe and post (a nested dispatch on the same
 * thread). It runs on any number of threads at once.
 *
 * Blocks with std::mutex / std::condition_variable, which must block through
 * the scheduler: true on ESP-IDF and Arduino-ESP32 (pthread over FreeRTOS)
 * and on a host OS, not inside the FreeRTOS POSIX simulator's tasks.
 */

#include "crosspad/event/IEventBus.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace crosspad {

class SubscriptionTable {
public:
    SubscriptionHandle add(EventType type, EventHandler handler, void* userCtx);

    /// IEventBus::unsubscribe(): a handle that is not (or no longer) in the
    /// table is ignored.
    void remove(SubscriptionHandle handle);

    /// Calls every handler of @p type subscribed when the dispatch starts and
    /// still subscribed when its turn comes, in subscription order.
    void dispatch(EventType type, const void* data);

private:
    struct Entry {
        EventType type;
        EventHandler handler;
        void* userCtx;
        bool active = true;
        int running = 0;
    };

    bool dispatchingOnThisThread() const;

    std::vector<std::shared_ptr<Entry>> entries_;
    std::mutex mutex_;
    std::condition_variable idle_;
};

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/event/SyncEventBus.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace crosspad;

TEST_CASE("SyncEventBus: init", "[event]") {
    SyncEventBus bus;
    REQUIRE(bus.init());
}

TEST_CASE("SyncEventBus: subscribe and receive NoteOn", "[event]") {
    SyncEventBus bus;
    bus.init();

    struct Ctx { bool received = false; uint8_t note = 0; uint8_t vel = 0; };
    Ctx ctx;

    auto handle = bus.subscribe(EventType::NoteOn,
        [](const void* data, EventType type, void* user) {
            auto* c = static_cast<Ctx*>(user);
            auto* d = static_cast<const NoteOnData*>(data);
            c->received = true;
            c->note = d->note;
            c->vel = d->velocity;
        }, &ctx);

    bus.postNoteOn(0, 60, 127, EventSource::PadManager);

    REQUIRE(ctx.received);
    REQUIRE(ctx.note == 60);
    REQUIRE(ctx.vel == 127);

    bus.unsubscribe(handle);
}

TEST_CASE("SyncEventBus: unsubscribe stops delivery", "[event]") {
    SyncEventBus bus;
    bus.init();

    int count = 0;
    auto handle = bus.subscribe(EventType::PadPressed,
        [](const void*, EventType, void* user) {
            (*static_cast<int*>(user))++;
        }, &count);

    bus.postPadPressed(0, true, 100, EventSource::PadManager);
    REQUIRE(count == 1);

    bus.unsubscribe(handle);

    bus.postPadPressed(0, true, 100, EventSource::PadManager);
    REQUIRE(count == 1); // no increment after unsubscribe
}

TEST_CASE("SyncEventBus: multiple subscribers", "[event]") {
    SyncEventBus bus;
    bus.init();

    int countA = 0, countB = 0;

    auto hA = bus.subscribe(EventType::NoteOn,
        [](const void*, EventType, void* user) { (*static_cast<int*>(user))++; }, &countA);
    auto hB = bus.subscribe(EventType::NoteOn,
        [](const void*, EventType, void* user) { (*static_cast<int*>(user))++; }, &countB);

    bus.postNoteOn(0, 60, 100, EventSource::System);

    REQUIRE(countA == 1);
    REQUIRE(countB == 1);

    bus.unsubscribe(hA);
    bus.unsubscribe(hB);
}

TEST_CASE("SyncEventBus: sendNoteOn is synchronous", "[event]") {
    SyncEventBus bus;
    bus.init();

    bool received = false;
    auto handle = bus.subscribe(EventType::NoteOn,
        [](const void*, EventType, void* user) { *static_cast<bool*>(user) = true; }, &received);

    bus.sendNoteOn(0, 60, 127, EventSource::System);
    REQUIRE(received); // synchronous — must be true immediately

    bus.unsubscribe(handle);
}

TEST_CASE("SyncEventBus: event type filtering", "[event]") {
    SyncEventBus bus;
    bus.init();

    int noteOnCount = 0, padCount = 0;

    auto h1 = bus.subscribe(EventType::NoteOn,
        [](const void*, EventType, void* user) { (*static_cast<int*>(user))++; }, &noteOnCount);
    auto h2 = bus.subscribe(EventType::PadPressed,
        [](const void*, EventType, void* user) { (*static_cast<int*>(user))++; }, &padCount);

    bus.postNoteOn(0, 60, 100, EventSource::System);
    bus.postPadPressed(0, true, 100);

    REQUIRE(noteOnCount == 1);
    REQUIRE(padCount == 1);

    bus.unsubscribe(h1);
    bus.unsubscribe(h2);
}

// ── Subscription contract (IEventBus::unsubscribe) ─────────────────────────

namespace {

struct Latch {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;

    void release() {
        { std::lock_guard<std::mutex> lock(m); open = true; }
        cv.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [this] { return open; });
    }
    bool waitFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, timeout, [this] { return open; });
    }
};

struct Reentrant {
    SyncEventBus* bus = nullptr;
    SubscriptionHandle self = nullptr;
    SubscriptionHandle other = nullptr;
    SubscriptionHandle added = nullptr;
    int selfCalls = 0;
    int otherCalls = 0;
    int addedCalls = 0;
};

} // namespace

TEST_CASE("SyncEventBus: a handler unsubscribes itself and another handler", "[event]") {
    SyncEventBus bus;
    bus.init();

    Reentrant r;
    r.bus = &bus;
    r.self = bus.subscribe(EventType::NoteOn,
        [](const void*, EventType, void* user) {
            auto* c = static_cast<Reentrant*>(user);
            c->selfCalls++;
            c->bus->unsubscribe(c->self);
            c->bus->unsubscribe(c->other);
        }, &r);
    r.other = bus.subscribe(EventType::NoteOn,
        [](const void*, EventType, void* user) { static_cast<Reentrant*>(user)->otherCalls++; }, &r);

    bus.postNoteOn(0, 60, 100, EventSource::System);
    bus.postNoteOn(0, 61, 100, EventSource::System);

    REQUIRE(r.selfCalls == 1);
    REQUIRE(r.otherCalls == 0);
}

TEST_CASE("SyncEventBus: a handler subscribed during a dispatch is called from the next one", "[event]") {
    SyncEventBus bus;
    bus.init();

    Reentrant r;
    r.bus = &bus;
    r.self = bus.subscribe(EventType::NoteOn,
        [](const void*, EventType, void* user) {
            auto* c = static_cast<Reentrant*>(user);
            c->selfCalls++;
            if (c->added) return;
            c->added = c->bus->subscribe(EventType::NoteOn,
                [](const void*, EventType, void* u) { static_cast<Reentrant*>(u)->addedCalls++; }, c);
        }, &r);

    bus.postNoteOn(0, 60, 100, EventSource::System);
    REQUIRE(r.addedCalls == 0);

    bus.postNoteOn(0, 61, 100, EventSource::System);
    REQUIRE(r.selfCalls == 2);
    REQUIRE(r.addedCalls == 1);

    bus.unsubscribe(r.self);
    bus.unsubscribe(r.added);
}

TEST_CASE("SyncEventBus: a handler posts on the same bus", "[event]") {
    SyncEventBus bus;
    bus.init();

    Reentrant r;
    r.bus = &bus;
    r.self = bus.subscribe(EventType::NoteOn,
        [](const void*, EventType, void* user) {
            auto* c = static_cast<Reentrant*>(user);
            c->selfCalls++;
            c->bus->postNoteOff(0, 60, 0, EventSource::System);
        }, &r);
    r.other = bus.subscribe(EventType::NoteOff,
        [](const void*, EventType, void* user) { static_cast<Reentrant*>(user)->otherCalls++; }, &r);

    bus.postNoteOn(0, 60, 100, EventSource::System);

    REQUIRE(r.selfCalls == 1);
    REQUIRE(r.otherCalls == 1);

    bus.unsubscribe(r.self);
    bus.unsubscribe(r.other);
}

TEST_CASE("SyncEventBus: unsubscribe from another thread waits for the running handler", "[event]") {
    SyncEventBus bus;
    bus.init();

    struct Ctx {
        Latch entered;
        Latch release;
        std::atomic<bool> unsubscribed{false};
        std::atomic<bool> unsubscribedWhileRunning{false};
        std::atomic<int> calls{0};
    } ctx;

    auto handle = bus.subscribe(EventType::NoteOn,
        [](const void*, EventType, void* user) {
            auto* c = static_cast<Ctx*>(user);
            c->calls++;
            c->entered.release();
            c->release.wait();
            c->unsubscribedWhileRunning = c->unsubscribed.load();
        }, &ctx);

    std::thread dispatcher([&] { bus.postNoteOn(0, 60, 100, EventSource::System); });
    ctx.entered.wait();

    Latch unsubscribeReturned;
    std::thread unsubscriber([&] {
        bus.unsubscribe(handle);
        ctx.unsubscribed = true;
        unsubscribeReturned.release();
    });

    // A broken bus returns at once; a correct one cannot return before the
    // release below, so the bounded wait only gives a broken one its chance.
    REQUIRE_FALSE(unsubscribeReturned.waitFor(std::chrono::milliseconds(50)));
    ctx.release.release();
    unsubscriber.join();
    dispatcher.join();

    REQUIRE_FALSE(ctx.unsubscribedWhileRunning.load());
    bus.postNoteOn(0, 61, 100, EventSource::System);
    REQUIRE(ctx.calls.load() == 1);
}

TEST_CASE("SyncEventBus: unsubscribe from inside a handler does not wait for another thread", "[event]") {
    SyncEventBus bus;
    bus.init();

    struct Ctx {
        SyncEventBus* bus = nullptr;
        SubscriptionHandle blocked = nullptr;
        Latch entered;
        Latch release;
        std::atomic<int> blockedCalls{0};
    } ctx;
    ctx.bus = &bus;

    ctx.blocked = bus.subscribe(EventType::NoteOn,
        [](const void*, EventType, void* user) {
            auto* c = static_cast<Ctx*>(user);
            c->blockedCalls++;
            c->entered.release();
            c->release.wait();
        }, &ctx);
    auto remover = bus.subscribe(EventType::NoteOff,
        [](const void*, EventType, void* user) {
            auto* c = static_cast<Ctx*>(user);
            c->bus->unsubscribe(c->blocked);
        }, &ctx);

    std::thread dispatcher([&] { bus.postNoteOn(0, 60, 100, EventSource::System); });
    ctx.entered.wait();

    bus.postNoteOff(0, 60, 0, EventSource::System);
    ctx.release.release();
    dispatcher.join();

    bus.postNoteOn(0, 61, 100, EventSource::System);
    REQUIRE(ctx.blockedCalls.load() == 1);
    bus.unsubscribe(remover);
}

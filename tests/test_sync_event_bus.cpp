#include <catch2/catch_test_macros.hpp>
#include "crosspad/event/SyncEventBus.hpp"

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

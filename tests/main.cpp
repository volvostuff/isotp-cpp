/*
 * isotp_cpp tests: exercise the wrapper through an in-memory CAN bus with a
 * fake clock. Deterministic: no threads, frames are queued and dispatched
 * explicitly, time only moves when the test advances the clock.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "isotp_cpp/isotp.hpp"

namespace
{

int failures = 0;
int checks = 0;

void report(const char* expr, const char* file, int line)
{
    ++failures;
    std::printf("  FAIL %s:%d: %s\n", file, line, expr);
}

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(cond)) {                                                                             \
            report(#cond, __FILE__, __LINE__);                                                     \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        ++checks;                                                                                  \
        auto va = (a);                                                                             \
        auto vb = (b);                                                                             \
        if (!(va == vb)) {                                                                         \
            std::printf("  FAIL %s:%d: %s == %s (left=%d right=%d)\n", __FILE__, __LINE__, #a, #b, \
                        static_cast<int>(va), static_cast<int>(vb));                               \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

struct FakeClock
{
    uint32_t now = 1000000;
    uint32_t operator()() { return now; }
    void advance(uint32_t us) { now += us; }
};

struct TestBus : public isotp_cpp::CanDriver
{
    struct Frame
    {
        uint32_t id;
        uint8_t data[8];
        uint8_t size;
    };

    std::vector<Frame> log;
    std::deque<Frame> pending;
    std::unordered_map<uint32_t, isotp_cpp::Link*> targets;

    bool sendCan(uint32_t arbitrationId, const uint8_t* data, uint8_t size) override
    {
        Frame f{};
        f.id = arbitrationId;
        f.size = size;
        std::memcpy(f.data, data, size);
        log.push_back(f);
        pending.push_back(f);
        return true;
    }

    void addTarget(uint32_t id, isotp_cpp::Link* link) { targets[id] = link; }

    // Deliver every queued frame to its target; receivers may queue more.
    void dispatch()
    {
        while (!pending.empty()) {
            Frame f = pending.front();
            pending.pop_front();
            auto it = targets.find(f.id);
            if (it != targets.end()) {
                it->second->onCanFrame(f.id, f.data, f.size);
            }
        }
    }

    bool hasPci(uint8_t pci) const { return countPci(pci) > 0; }

    int countPci(uint8_t pci) const
    {
        int n = 0;
        for (const Frame& f : log) {
            if (f.size > 0 && (f.data[0] >> 4) == pci) {
                ++n;
            }
        }
        return n;
    }
};

struct Recorder
{
    bool received = false;
    bool sent = false;
    std::vector<uint8_t> last;
    bool errored = false;
    std::string errorWhere;
    int errorCode = 0;
};

isotp_cpp::Link makeLink(TestBus& bus, uint32_t tx, uint32_t rx, Recorder& r)
{
    isotp_cpp::Link::Config cfg;
    cfg.txId = tx;
    cfg.rxId = rx;
    isotp_cpp::Link::Callbacks cb;
    cb.onReceived = [&r](std::vector<uint8_t> payload) {
        r.received = true;
        r.last = std::move(payload);
    };
    cb.onSent = [&r](uint32_t /*size*/) { r.sent = true; };
    cb.onError = [&r](const char* where, int code) {
        r.errored = true;
        r.errorWhere = where;
        r.errorCode = code;
    };
    return isotp_cpp::Link(bus, cfg, cb);
}

// Drive both links until `pred()` holds (or iterations run out).
void spin(TestBus& bus, isotp_cpp::Link& a, isotp_cpp::Link& b, FakeClock& clock,
          const std::function<bool()>& pred, int maxIter = 4000)
{
    for (int i = 0; i < maxIter && !pred(); ++i) {
        bus.dispatch();
        a.poll();
        b.poll();
        clock.advance(1000); // 1 ms steps
    }
}

void testSingleFrameRoundtrip()
{
    TestBus bus;
    FakeClock clock;
    isotp_cpp::setClock([&clock]() { return clock(); });

    Recorder ra;
    Recorder rb;
    isotp_cpp::Link a = makeLink(bus, 0x7E0, 0x7E8, ra);
    isotp_cpp::Link b = makeLink(bus, 0x7E8, 0x7E0, rb);
    bus.addTarget(0x7E0, &b);
    bus.addTarget(0x7E8, &a);
    CHECK(a.start());
    CHECK(b.start());

    const std::vector<uint8_t> payload = {0x02, 0x3E, 0x00};
    CHECK_EQ(a.send(payload), isotp_cpp::Result::Ok);
    spin(bus, a, b, clock, [&]() { return rb.received; });

    CHECK(ra.sent); // single frame completes synchronously
    CHECK(rb.received);
    CHECK(rb.last == payload);
    CHECK_EQ(bus.log.size(), 1u); // exactly one SF on the wire
    a.stop();
    b.stop();
}

void testMultiFrameRoundtrip()
{
    TestBus bus;
    FakeClock clock;
    isotp_cpp::setClock([&clock]() { return clock(); });

    Recorder ra;
    Recorder rb;
    isotp_cpp::Link a = makeLink(bus, 0x7E0, 0x7E8, ra);
    isotp_cpp::Link b = makeLink(bus, 0x7E8, 0x7E0, rb);
    bus.addTarget(0x7E0, &b);
    bus.addTarget(0x7E8, &a);
    CHECK(a.start());
    CHECK(b.start());

    // 2000 bytes: FF + many CF. With the ISO-TP block size (BS) default of 255
    // a 2000-byte payload spans several blocks, so the receiver emits more than
    // one flow-control frame.
    std::vector<uint8_t> payload;
    for (int i = 0; i < 2000; ++i) {
        payload.push_back(static_cast<uint8_t>(i & 0xFF));
    }
    CHECK_EQ(a.send(payload), isotp_cpp::Result::Ok);
    spin(bus, a, b, clock, [&]() { return rb.received && ra.sent && !a.sendInProgress(); });

    CHECK(ra.sent);
    CHECK(rb.received);
    CHECK(rb.last == payload);
    // Multi-frame exchange: first frame + many consecutive frames, and the
    // receiver (B) must have emitted flow control on its tx id (0x7E8).
    CHECK(bus.hasPci(0x1)); // FF
    CHECK(bus.countPci(0x2) > 1); // CF (BS=8 forces repeated FC blocks)
    CHECK(bus.countPci(0x3) > 1); // FC emitted by the receiver between blocks
    a.stop();
    b.stop();
}

void testTxTimeoutWithoutFlowControl()
{
    TestBus bus;
    FakeClock clock;
    isotp_cpp::setClock([&clock]() { return clock(); });

    Recorder ra;
    isotp_cpp::Link a = makeLink(bus, 0x7E0, 0x7E8, ra);
    CHECK(a.start());

    std::vector<uint8_t> payload(100, 0x55);
    CHECK_EQ(a.send(payload), isotp_cpp::Result::Ok); // FF queued, awaiting FC

    // No peer answers: frames are dropped, the clock advances -> N_Bs timeout
    // (compile-time ISO_TP_DEFAULT_RESPONSE_TIMEOUT_US = 5 s).
    for (int i = 0; i < 5600 && !ra.errored; ++i) {
        bus.pending.clear();
        a.poll();
        clock.advance(1000);
    }
    CHECK(ra.errored);
    CHECK(ra.errorWhere == "tx");
    CHECK(!a.sendInProgress());
    a.stop();
}

void testOversizeRejected()
{
    TestBus bus;
    FakeClock clock;
    isotp_cpp::setClock([&clock]() { return clock(); });
    Recorder r;
    isotp_cpp::Link a = makeLink(bus, 0x7E0, 0x7E8, r);
    CHECK(a.start());
    std::vector<uint8_t> payload(a.maxPayload() + 1, 0x11);
    CHECK_EQ(a.send(payload), isotp_cpp::Result::Overflow);
    a.stop();
}

void testUnknownIdIgnored()
{
    TestBus bus;
    FakeClock clock;
    isotp_cpp::setClock([&clock]() { return clock(); });
    Recorder r;
    isotp_cpp::Link a = makeLink(bus, 0x7E0, 0x7E8, r);
    CHECK(a.start());

    const uint8_t sf[] = {0x02, 0xAA, 0xBB};
    a.onCanFrame(0x1234, sf, sizeof(sf)); // not our rxId
    a.poll();
    CHECK(!r.received);
    CHECK(!r.errored);
    a.stop();
}

void testInvalidBuffersRejected()
{
    TestBus bus;
    Recorder r;
    isotp_cpp::Link::Config cfg;
    cfg.sendBufferBytes = 0;
    isotp_cpp::Link::Callbacks cb;
    isotp_cpp::Link a(bus, cfg, cb);
    std::string err;
    CHECK(!a.start(&err));
    CHECK(!err.empty());
}

} // namespace

int main()
{
    std::printf("[isotp_cpp]\n");
    testSingleFrameRoundtrip();
    testMultiFrameRoundtrip();
    testTxTimeoutWithoutFlowControl();
    testOversizeRejected();
    testUnknownIdIgnored();
    testInvalidBuffersRejected();

    std::printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

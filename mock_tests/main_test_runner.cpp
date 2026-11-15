/*
 * main_test_runner.cpp — single unified test entry (one main only)
 * ProcSentinel
 *
 * Covers:
 *   1. Low-level C ring helpers from shared_protocol.h
 *   2. Protocol / fromWire conversion
 *   3. High-level MockDriverClient
 *
 * Build:
 *   cmake -S .. -B ../build && cmake --build ../build
 *   cl /std:c++23 /EHsc /I..\shared /I..\usermode main_test_runner.cpp /Fe:procsentinel_unified_tests.exe
 */

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../shared/shared_protocol.h"
#include "../usermode/types.h"

using procsentinel::Event;
using procsentinel::EventType;
using procsentinel::ProcInfo;
using procsentinel::ProcessState;
using procsentinel::Stats;

// =============================================================================
// IDriverClient + Mock
// =============================================================================

class IDriverClient {
public:
    virtual ~IDriverClient() = default;
    virtual std::optional<Stats> getStats() = 0;
    virtual bool startMonitor() = 0;
    virtual bool stopMonitor() = 0;
    virtual bool resetStats() = 0;
    virtual std::vector<Event> getEvents(std::size_t maxCount) = 0;
    virtual std::vector<ProcInfo> getProcessList() = 0;
};

class MockDriverClient : public IDriverClient {
public:
    explicit MockDriverClient(bool shouldSucceed = true)
        : shouldSucceed_(shouldSucceed)
    {
    }

    void enqueue(Event e)
    {
        pool_.push_back(std::move(e));
        stats_.eventCount = static_cast<std::uint32_t>(pool_.size());
    }

    void setStats(Stats s) { stats_ = std::move(s); }

    std::optional<Stats> getStats() override
    {
        if (!shouldSucceed_) return std::nullopt;
        return stats_;
    }

    bool startMonitor() override
    {
        if (!shouldSucceed_) return false;
        monitoring_ = true;
        return true;
    }

    bool stopMonitor() override
    {
        if (!shouldSucceed_) return false;
        monitoring_ = false;
        return true;
    }

    bool resetStats() override
    {
        if (!shouldSucceed_) return false;
        stats_ = Stats{};
        pool_.clear();
        return true;
    }

    std::vector<Event> getEvents(std::size_t maxCount) override
    {
        std::vector<Event> out;
        if (!shouldSucceed_ || pool_.empty() || maxCount == 0) return out;
        const std::size_t n = (std::min)(maxCount, pool_.size());
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(std::move(pool_.front()));
            pool_.erase(pool_.begin());
            stats_.eventCount++;
        }
        return out;
    }

    std::vector<ProcInfo> getProcessList() override
    {
        if (!shouldSucceed_) return {};
        return processList_;
    }

    void setProcessList(std::vector<ProcInfo> list) { processList_ = std::move(list); }

    bool isMonitoring() const { return monitoring_; }
    std::size_t queued() const { return pool_.size(); }

private:
    bool shouldSucceed_{true};
    bool monitoring_{false};
    Stats stats_{};
    std::vector<Event> pool_;
    std::vector<ProcInfo> processList_;
};

// =============================================================================
// Assertion helpers
// =============================================================================

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__              \
                      << "  " << #cond << "\n";                                \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// =============================================================================
// 1. Low-level C ring
// =============================================================================

void run_ring_tests()
{
    std::cout << "--- Low-Level C Ring ---\n";

    WireEventRing ring;
    EventRingInit(&ring);
    CHECK(EventRingIsEmpty(&ring));
    CHECK(!EventRingIsFull(&ring));
    CHECK(EventRingCount(&ring) == 0);

    WireEvent ev{};
    ev.ProcessId = 99;
    ev.EventType = PROCSENTINEL_EVENT_CREATE;
    ev.State = PROCSENTINEL_STATE_RUNNING;
    std::strncpy(ev.ImageName, "test.exe", PROCSENTINEL_MAX_NAME - 1);

    EventRingPush(&ring, &ev);
    CHECK(EventRingCount(&ring) == 1);
    CHECK(!EventRingIsEmpty(&ring));

    WireEvent out{};
    CHECK(EventRingPop(&ring, &out) == 1);
    CHECK(out.ProcessId == 99);
    CHECK(out.EventType == PROCSENTINEL_EVENT_CREATE);
    CHECK(std::strcmp(out.ImageName, "test.exe") == 0);
    CHECK(EventRingIsEmpty(&ring));

    // Drop-oldest overflow
    EventRingInit(&ring);
    for (unsigned i = 0; i < PROCSENTINEL_MAX_EVENTS + 3; ++i) {
        ev.ProcessId = i;
        ev.EventType = PROCSENTINEL_EVENT_CREATE;
        EventRingPush(&ring, &ev);
    }
    CHECK(ring.count == PROCSENTINEL_MAX_EVENTS);
    CHECK(ring.dropped == 3);
    CHECK(EventRingPop(&ring, &out) == 1);
    CHECK(out.ProcessId == 3); // 0,1,2 dropped

    // Empty pop
    EventRingInit(&ring);
    CHECK(EventRingPop(&ring, &out) == 0);

    std::cout << "[PASS] Low-Level C Ring Assertions Complete\n";
}

// =============================================================================
// 2. Protocol / fromWire
// =============================================================================

void run_protocol_tests()
{
    std::cout << "--- Protocol / fromWire ---\n";

    CHECK(sizeof(WireEvent) == sizeof(PROCSENTINEL_EVENT));
    CHECK(sizeof(WireStats) == sizeof(PROCSENTINEL_STATS));
    CHECK(PROCSENTINEL_MAX_PATH == 260);
    CHECK(PROCSENTINEL_MAX_NAME == 16);

    WireEvent we{};
    we.Timestamp = 1234567890LL;
    we.ProcessId = 42;
    we.ParentProcessId = 4;
    we.EventType = PROCSENTINEL_EVENT_EXIT;
    we.State = PROCSENTINEL_STATE_EXITING;
    std::strncpy(we.ImageName, "cmd.exe", PROCSENTINEL_MAX_NAME - 1);
    std::strncpy(we.ImagePath, "C:\\Windows\\System32\\cmd.exe", PROCSENTINEL_MAX_PATH - 1);

    Event e = Event::fromWire(we);
    CHECK(e.timestamp == 1234567890LL);
    CHECK(e.processId == 42);
    CHECK(e.parentProcessId == 4);
    CHECK(e.eventType == EventType::Exit);
    CHECK(e.state == ProcessState::Exiting);
    CHECK(e.imageName == "cmd.exe");
    CHECK(e.imagePath.find("cmd.exe") != std::string::npos);

    WireStats ws{};
    ws.Creates = 10;
    ws.Exits = 3;
    ws.DroppedEvents = 1;
    Stats s = Stats::fromWire(ws);
    CHECK(s.creates == 10);
    CHECK(s.exits == 3);
    CHECK(s.droppedEvents == 1);

    CHECK(toString(EventType::Create) == "CREATE");
    CHECK(toString(ProcessState::Running) == "Running");

    std::cout << "[PASS] Protocol / fromWire Complete\n";
}

// =============================================================================
// 3. Mock client
// =============================================================================

void run_mock_client_tests()
{
    std::cout << "--- MockDriverClient ---\n";

    auto client = std::make_shared<MockDriverClient>(true);
    CHECK(client->startMonitor());
    CHECK(client->isMonitoring());

    Event e1;
    e1.processId = 100;
    e1.eventType = EventType::Create;
    e1.imageName = "notepad.exe";
    client->enqueue(e1);

    Event e2;
    e2.processId = 100;
    e2.eventType = EventType::Exit;
    e2.imageName = "notepad.exe";
    client->enqueue(e2);

    CHECK(client->queued() == 2);

    auto events = client->getEvents(10);
    CHECK(events.size() == 2);
    CHECK(events[0].processId == 100);
    CHECK(events[0].eventType == EventType::Create);
    CHECK(events[1].eventType == EventType::Exit);
    CHECK(client->queued() == 0);

    ProcInfo p;
    p.pid = 4;
    p.imageName = "System";
    p.protectedProcess = true;
    client->setProcessList({p});
    auto list = client->getProcessList();
    CHECK(list.size() == 1);
    CHECK(list[0].protectedProcess);

    CHECK(client->resetStats());
    CHECK(client->queued() == 0);

    // Failure mode
    auto bad = std::make_shared<MockDriverClient>(false);
    CHECK(!bad->startMonitor());
    CHECK(!bad->getStats().has_value());

    std::cout << "[PASS] MockDriverClient Complete\n";
}

// =============================================================================
// main
// =============================================================================

int main()
{
    std::cout << "=== ProcSentinel Unified Tests ===\n\n";

    run_ring_tests();
    run_protocol_tests();
    run_mock_client_tests();

    std::cout << "\n====================================\n";
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << "FAILED: " << g_failures << " assertion(s)\n";
    return 1;
}

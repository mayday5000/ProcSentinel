/*
 * main_test_runner.cpp — single unified test entry (one main only)
 * ProcSentinel
 *
 * Covers:
 *   1. Low-level C ring helpers from shared_protocol.h (push/pop/overflow/reset)
 *   2. Wire protocol: sizes, offsets, IOCTL codes, fromWire conversion
 *   3. Client parsing helpers from types.h (parseEvents / parseProcessList /
 *      parseDump / makeDumpRequest) — the same code procsentinel_app.cpp uses
 *   4. High-level MockDriverClient
 *
 * The kernel driver itself is not exercised here (needs a VM).
 *
 * Build:
 *   cmake -S .. -B ../build && cmake --build ../build
 *   cl /std:c++latest /EHsc /I..\shared /I..\usermode main_test_runner.cpp /Fe:procsentinel_unified_tests.exe
 *   g++ -std=c++23 -I shared -I usermode mock_tests/main_test_runner.cpp
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
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
using procsentinel::Protection;
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

    // Drains like the driver: events leave the queue and EventCount drops.
    std::vector<Event> getEvents(std::size_t maxCount) override
    {
        std::vector<Event> out;
        if (!shouldSucceed_ || pool_.empty() || maxCount == 0) return out;
        const std::size_t n = (std::min)(maxCount, pool_.size());
        out.assign(std::make_move_iterator(pool_.begin()),
                   std::make_move_iterator(pool_.begin() + static_cast<std::ptrdiff_t>(n)));
        pool_.erase(pool_.begin(), pool_.begin() + static_cast<std::ptrdiff_t>(n));
        stats_.eventCount = static_cast<std::uint32_t>(pool_.size());
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

static void reportSection(const char* name, int failuresBefore)
{
    const int failed = g_failures - failuresBefore;
    if (failed == 0) {
        std::cout << "[PASS] " << name << "\n";
    } else {
        std::cout << "[FAIL] " << name << " (" << failed << " assertion(s))\n";
    }
}

template <typename T>
static void appendBytes(std::vector<std::uint8_t>& buf, const T& value)
{
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    buf.insert(buf.end(), p, p + sizeof(T));
}

// =============================================================================
// 1. Low-level C ring
// =============================================================================

void run_ring_tests()
{
    std::cout << "--- Low-Level C Ring ---\n";
    const int before = g_failures;

    // ~600 KB: keep it off the stack.
    auto ringOwner = std::make_unique<WireEventRing>();
    WireEventRing* ring = ringOwner.get();

    EventRingInit(ring);
    CHECK(EventRingIsEmpty(ring));
    CHECK(!EventRingIsFull(ring));
    CHECK(EventRingCount(ring) == 0);

    WireEvent ev{};
    ev.ProcessId = 99;
    ev.EventType = PROCSENTINEL_EVENT_CREATE;
    ev.State = PROCSENTINEL_STATE_RUNNING;
    std::strncpy(ev.ImageName, "test.exe", PROCSENTINEL_MAX_NAME - 1);

    EventRingPush(ring, &ev);
    CHECK(EventRingCount(ring) == 1);
    CHECK(!EventRingIsEmpty(ring));

    WireEvent out{};
    CHECK(EventRingPop(ring, &out) == 1);
    CHECK(out.ProcessId == 99);
    CHECK(out.EventType == PROCSENTINEL_EVENT_CREATE);
    CHECK(std::strcmp(out.ImageName, "test.exe") == 0);
    CHECK(EventRingIsEmpty(ring));

    // Drop-oldest overflow
    EventRingInit(ring);
    for (unsigned i = 0; i < PROCSENTINEL_MAX_EVENTS + 3; ++i) {
        ev.ProcessId = i;
        ev.EventType = PROCSENTINEL_EVENT_CREATE;
        EventRingPush(ring, &ev);
    }
    CHECK(EventRingIsFull(ring));
    CHECK(ring->count == PROCSENTINEL_MAX_EVENTS);
    CHECK(ring->dropped == 3);
    CHECK(EventRingPop(ring, &out) == 1);
    CHECK(out.ProcessId == 3); // 0,1,2 dropped

    // Reset mirrors IOCTL_PROCSENTINEL_RESET_STATS: indices, count AND dropped
    EventRingReset(ring);
    CHECK(EventRingIsEmpty(ring));
    CHECK(ring->dropped == 0);
    CHECK(ring->head == 0 && ring->tail == 0);
    ev.ProcessId = 7;
    EventRingPush(ring, &ev);
    CHECK(EventRingPop(ring, &out) == 1 && out.ProcessId == 7);

    // Empty pop
    EventRingInit(ring);
    CHECK(EventRingPop(ring, &out) == 0);

    reportSection("Low-Level C Ring", before);
}

// =============================================================================
// 2. Protocol / fromWire
// =============================================================================

void run_protocol_tests()
{
    std::cout << "--- Protocol / fromWire ---\n";
    const int before = g_failures;

    // Layout (also enforced at compile time in shared_protocol.h)
    CHECK(sizeof(WireEvent) == 296);
    CHECK(sizeof(WireProcInfo) == 288);
    CHECK(sizeof(WireStats) == 44);
    CHECK(sizeof(WireDumpRequest) == 24);
    CHECK(sizeof(WireDumpHeader) == 32);
    CHECK(PROCSENTINEL_MAX_PATH == 260);
    CHECK(PROCSENTINEL_MAX_NAME == 16);

    // IOCTL codes: device 0x22, function, method and access bits.
    auto method = [](std::uint32_t c) { return c & 3u; };
    auto access = [](std::uint32_t c) { return (c >> 14) & 3u; };
    auto function = [](std::uint32_t c) { return (c >> 2) & 0xFFFu; };
    CHECK((IOCTL_PROCSENTINEL_GET_STATS >> 16) == 0x22u);
    CHECK(function(IOCTL_PROCSENTINEL_GET_STATS) == 0xB00u);
    CHECK(function(IOCTL_PROCSENTINEL_DUMP_PROCESS) == 0xB06u);
    CHECK(method(IOCTL_PROCSENTINEL_GET_EVENTS) == PROCSENTINEL_METHOD_BUFFERED);
    CHECK(method(IOCTL_PROCSENTINEL_DUMP_PROCESS) == PROCSENTINEL_METHOD_OUT_DIRECT);
    CHECK(access(IOCTL_PROCSENTINEL_GET_STATS) == PROCSENTINEL_FILE_READ_DATA);
    CHECK(access(IOCTL_PROCSENTINEL_GET_PROCESS_LIST) == PROCSENTINEL_FILE_READ_DATA);
    CHECK(access(IOCTL_PROCSENTINEL_START_MONITOR) == PROCSENTINEL_FILE_WRITE_DATA);
    CHECK(access(IOCTL_PROCSENTINEL_RESET_STATS) == PROCSENTINEL_FILE_WRITE_DATA);
    CHECK(access(IOCTL_PROCSENTINEL_DUMP_PROCESS) == (PROCSENTINEL_FILE_READ_DATA | PROCSENTINEL_FILE_WRITE_DATA));
    CHECK(IOCTL_PROCSENTINEL_GET_STATS == 0x00226C00u);   // CTL_CODE(0x22, 0xB00, BUFFERED, FILE_READ_DATA)

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

    // Unterminated fixed-size fields must not over-read.
    WireEvent full{};
    std::memset(full.ImageName, 'A', sizeof(full.ImageName));
    CHECK(Event::fromWire(full).imageName.size() == PROCSENTINEL_MAX_NAME);

    WireProcInfo wp{};
    wp.Pid = 640;
    wp.ParentPid = 4;
    wp.Protection = PROCSENTINEL_PROTECTION_PPL;
    std::strncpy(wp.ImageName, "lsass.exe", PROCSENTINEL_MAX_NAME - 1);
    ProcInfo p = ProcInfo::fromWire(wp);
    CHECK(p.pid == 640);
    CHECK(p.protection == Protection::PPL);
    CHECK(p.isProtected());
    wp.Protection = PROCSENTINEL_PROTECTION_PP;
    CHECK(ProcInfo::fromWire(wp).protection == Protection::PP);
    wp.Protection = PROCSENTINEL_PROTECTION_NONE;
    CHECK(!ProcInfo::fromWire(wp).isProtected());

    WireStats ws{};
    ws.Creates = 10;
    ws.Exits = 3;
    ws.DroppedEvents = 1;
    ws.DumpRequests = 2;
    ws.DumpFailures = 1;
    Stats s = Stats::fromWire(ws);
    CHECK(s.creates == 10);
    CHECK(s.exits == 3);
    CHECK(s.droppedEvents == 1);
    CHECK(s.dumpRequests == 2);
    CHECK(s.dumpFailures == 1);

    CHECK(toString(EventType::Create) == "CREATE");
    CHECK(toString(ProcessState::Running) == "Running");
    CHECK(toString(Protection::None) == "NO");
    CHECK(toString(Protection::PP) == "PP");
    CHECK(toString(Protection::PPL) == "PPL");

    reportSection("Protocol / fromWire", before);
}

// =============================================================================
// 3. Client parsing helpers (types.h)
// =============================================================================

void run_parser_tests()
{
    std::cout << "--- Client parsing helpers ---\n";
    const int before = g_failures;

    // parseEvents: whole records only, trailing partial record ignored
    std::vector<std::uint8_t> evBytes;
    for (std::uint32_t pid : {10u, 11u}) {
        WireEvent w{};
        w.ProcessId = pid;
        w.EventType = PROCSENTINEL_EVENT_CREATE;
        appendBytes(evBytes, w);
    }
    evBytes.push_back(0xCC);
    auto events = procsentinel::parseEvents(evBytes);
    CHECK(events.size() == 2);
    CHECK(events.size() == 2 && events[1].processId == 11);
    CHECK(procsentinel::parseEvents({}).empty());

    // parseProcessList
    std::vector<std::uint8_t> plBytes;
    WireProcInfo wp{};
    wp.Pid = 4;
    wp.Protection = PROCSENTINEL_PROTECTION_PP;
    std::strncpy(wp.ImageName, "System", PROCSENTINEL_MAX_NAME - 1);
    appendBytes(plBytes, wp);
    auto list = procsentinel::parseProcessList(plBytes);
    CHECK(list.size() == 1);
    CHECK(list.size() == 1 && list[0].imageName == "System" && list[0].protection == Protection::PP);

    // parseDump: valid, truncated + partial
    WireDumpHeader h{};
    h.Magic = PROCSENTINEL_DUMP_MAGIC;
    h.Flags = PROCSENTINEL_DUMP_FLAG_TRUNCATED | PROCSENTINEL_DUMP_FLAG_PARTIAL;
    h.ImageBase = 0x7FF600000000ull;
    h.Pid = 1234;
    h.SizeOfImage = 0x900000;
    h.DataLength = 8;
    h.UnreadablePages = 2;
    std::vector<std::uint8_t> dump;
    appendBytes(dump, h);
    for (int i = 0; i < 8; ++i) dump.push_back(static_cast<std::uint8_t>(i));
    auto parsed = procsentinel::parseDump(dump);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK(parsed->pid == 1234);
        CHECK(parsed->imageBase == 0x7FF600000000ull);
        CHECK(parsed->truncated);
        CHECK(parsed->partial);
        CHECK(parsed->unreadablePages == 2);
        CHECK(parsed->data.size() == 8);
        CHECK(parsed->data.size() == 8 && parsed->data[7] == 7);
    }

    // parseDump: rejects short buffer, bad magic, DataLength past the end
    CHECK(!procsentinel::parseDump(std::span<const std::uint8_t>(dump.data(), 10)).has_value());
    auto badMagic = dump;
    badMagic[0] ^= 0xFF;
    CHECK(!procsentinel::parseDump(badMagic).has_value());
    WireDumpHeader lying = h;
    lying.DataLength = 9;
    std::vector<std::uint8_t> lyingBytes;
    appendBytes(lyingBytes, lying);
    lyingBytes.resize(lyingBytes.size() + 8);
    CHECK(!procsentinel::parseDump(lyingBytes).has_value());

    // makeDumpRequest: PID wins; long names are truncated to 15 chars + NUL
    WireDumpRequest byPid = procsentinel::makeDumpRequest(4242, "ignored.exe");
    CHECK(byPid.Pid == 4242);
    CHECK(byPid.ImageName[0] == '\0');
    WireDumpRequest byName = procsentinel::makeDumpRequest(0, "SearchProtocolHost.exe");
    CHECK(byName.Pid == 0);
    CHECK(byName.ImageName[PROCSENTINEL_MAX_NAME - 1] == '\0');
    CHECK(std::string(byName.ImageName) == "SearchProtocolH");

    reportSection("Client parsing helpers", before);
}

// =============================================================================
// 4. Mock client
// =============================================================================

void run_mock_client_tests()
{
    std::cout << "--- MockDriverClient ---\n";
    const int before = g_failures;

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
    CHECK(client->getStats()->eventCount == 2);

    auto first = client->getEvents(1);
    CHECK(first.size() == 1);
    CHECK(client->getStats()->eventCount == 1);   // drain lowers the count

    auto events = client->getEvents(10);
    CHECK(events.size() == 1);
    CHECK(first.size() == 1 && first[0].eventType == EventType::Create);
    CHECK(events.size() == 1 && events[0].eventType == EventType::Exit);
    CHECK(client->queued() == 0);
    CHECK(client->getStats()->eventCount == 0);

    ProcInfo p;
    p.pid = 4;
    p.imageName = "System";
    p.protection = Protection::PP;
    client->setProcessList({p});
    auto list = client->getProcessList();
    CHECK(list.size() == 1);
    CHECK(list.size() == 1 && list[0].isProtected());

    CHECK(client->stopMonitor());
    CHECK(!client->isMonitoring());
    CHECK(client->resetStats());
    CHECK(client->queued() == 0);

    // Failure mode
    auto bad = std::make_shared<MockDriverClient>(false);
    CHECK(!bad->startMonitor());
    CHECK(!bad->getStats().has_value());
    CHECK(bad->getEvents(5).empty());

    reportSection("MockDriverClient", before);
}

// =============================================================================
// main
// =============================================================================

int main()
{
    std::cout << "=== ProcSentinel Unified Tests ===\n\n";

    run_ring_tests();
    run_protocol_tests();
    run_parser_tests();
    run_mock_client_tests();

    std::cout << "\n====================================\n";
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << "FAILED: " << g_failures << " assertion(s)\n";
    return 1;
}

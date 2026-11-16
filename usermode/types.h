#pragma once
/*
 * types.h — C++ domain types built on shared_protocol.h (single wire layout).
 * ProcSentinel
 *
 * Everything here is OS-independent so the host tests can exercise the same
 * parsing code the client uses (parseEvents / parseProcessList / parseDump).
 */

#include "../shared/shared_protocol.h"

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace procsentinel {

constexpr std::size_t kMaxNameChars = PROCSENTINEL_MAX_NAME;
constexpr std::size_t kMaxPathChars = PROCSENTINEL_MAX_PATH;
constexpr const char* kEventLogDirectoryName = "procsentinel_events";

enum class EventType : std::uint8_t {
    Create = PROCSENTINEL_EVENT_CREATE,
    Exit   = PROCSENTINEL_EVENT_EXIT,
    State  = PROCSENTINEL_EVENT_STATE
};

enum class ProcessState : std::uint8_t {
    Running   = PROCSENTINEL_STATE_RUNNING,
    Exiting   = PROCSENTINEL_STATE_EXITING,
    Zombie    = PROCSENTINEL_STATE_ZOMBIE,
    Suspended = PROCSENTINEL_STATE_SUSPENDED
};

enum class Protection : std::uint8_t {
    None = PROCSENTINEL_PROTECTION_NONE,
    PP   = PROCSENTINEL_PROTECTION_PP,
    PPL  = PROCSENTINEL_PROTECTION_PPL
};

using ::WireEvent;
using ::WireStats;
using ::WireProcInfo;
using ::WireDumpRequest;
using ::WireDumpHeader;

inline std::string_view toString(EventType t) noexcept
{
    switch (t) {
    case EventType::Create: return "CREATE";
    case EventType::Exit:   return "EXIT";
    case EventType::State:  return "STATE";
    default:                return "UNKNOWN";
    }
}

inline std::string_view toString(ProcessState s) noexcept
{
    switch (s) {
    case ProcessState::Running:   return "Running";
    case ProcessState::Exiting:   return "Exiting";
    case ProcessState::Zombie:    return "Zombie";
    case ProcessState::Suspended: return "Suspended";
    default:                      return "Unknown";
    }
}

inline std::string_view toString(Protection p) noexcept
{
    switch (p) {
    case Protection::None: return "NO";
    case Protection::PP:   return "PP";
    case Protection::PPL:  return "PPL";
    default:               return "?";
    }
}

/* Bounded string from a fixed-size wire field (never reads past N). */
template <std::size_t N>
inline std::string fieldToString(const char (&field)[N])
{
    std::size_t len = 0;
    while (len < N && field[len] != '\0') ++len;
    return std::string(field, len);
}

struct Event {
    int64_t       timestamp{};
    std::uint32_t processId{};
    std::uint32_t parentProcessId{};
    EventType     eventType{EventType::Create};
    ProcessState  state{ProcessState::Running};
    std::string   imageName;
    std::string   imagePath;

    static Event fromWire(const WireEvent& wire)
    {
        Event e;
        e.timestamp       = wire.Timestamp;
        e.processId       = wire.ProcessId;
        e.parentProcessId = wire.ParentProcessId;
        e.eventType       = static_cast<EventType>(wire.EventType);
        e.state           = static_cast<ProcessState>(wire.State);
        e.imageName       = fieldToString(wire.ImageName);
        e.imagePath       = fieldToString(wire.ImagePath);
        return e;
    }
};

struct ProcInfo {
    std::uint32_t pid{};
    std::uint32_t parentPid{};
    Protection    protection{Protection::None};
    ProcessState  state{ProcessState::Running};
    std::string   imageName;
    std::string   imagePath;

    [[nodiscard]] bool isProtected() const noexcept { return protection != Protection::None; }

    static ProcInfo fromWire(const WireProcInfo& wire)
    {
        ProcInfo p;
        p.pid        = wire.Pid;
        p.parentPid  = wire.ParentPid;
        p.protection = static_cast<Protection>(wire.Protection);
        p.state      = static_cast<ProcessState>(wire.State);
        p.imageName  = fieldToString(wire.ImageName);
        p.imagePath  = fieldToString(wire.ImagePath);
        return p;
    }
};

struct Stats {
    std::uint64_t creates{}, exits{}, stateChanges{};
    std::uint32_t currentProcessCount{}, eventCount{}, droppedEvents{};
    std::uint32_t dumpRequests{}, dumpFailures{};

    static Stats fromWire(const WireStats& wire)
    {
        Stats s;
        s.creates             = wire.Creates;
        s.exits               = wire.Exits;
        s.stateChanges        = wire.StateChanges;
        s.currentProcessCount = wire.CurrentProcessCount;
        s.eventCount          = wire.EventCount;
        s.droppedEvents       = wire.DroppedEvents;
        s.dumpRequests        = wire.DumpRequests;
        s.dumpFailures        = wire.DumpFailures;
        return s;
    }
};

/* Parsed DUMP_PROCESS result: header fields + a view of the image bytes. */
struct DumpInfo {
    std::uint32_t pid{};
    std::uint64_t imageBase{};
    std::uint32_t sizeOfImage{};
    std::uint32_t unreadablePages{};
    bool          truncated{false};
    bool          partial{false};
    std::span<const std::uint8_t> data;
};

/* ---------- Parsing helpers (shared by client and tests) ---------- */

/* Copy wire records out of a byte buffer (no alignment assumptions). */
template <typename Wire, typename Domain>
inline std::vector<Domain> parseRecords(std::span<const std::uint8_t> bytes)
{
    std::vector<Domain> out;
    const std::size_t count = bytes.size() / sizeof(Wire);
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Wire w;
        std::memcpy(&w, bytes.data() + i * sizeof(Wire), sizeof(Wire));
        out.push_back(Domain::fromWire(w));
    }
    return out;
}

inline std::vector<Event> parseEvents(std::span<const std::uint8_t> bytes)
{
    return parseRecords<WireEvent, Event>(bytes);
}

inline std::vector<ProcInfo> parseProcessList(std::span<const std::uint8_t> bytes)
{
    return parseRecords<WireProcInfo, ProcInfo>(bytes);
}

/* Validate a DUMP_PROCESS output buffer (header + data). */
inline std::expected<DumpInfo, std::string> parseDump(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < sizeof(WireDumpHeader))
        return std::unexpected(std::string("dump output shorter than its header"));

    WireDumpHeader h;
    std::memcpy(&h, bytes.data(), sizeof(h));
    if (h.Magic != PROCSENTINEL_DUMP_MAGIC)
        return std::unexpected(std::string("dump header has a bad magic value"));
    if (h.DataLength > bytes.size() - sizeof(WireDumpHeader))
        return std::unexpected(std::string("dump header claims more data than was returned"));
    if (h.DataLength > PROCSENTINEL_DUMP_MAX_SIZE)
        return std::unexpected(std::string("dump data exceeds PROCSENTINEL_DUMP_MAX_SIZE"));

    DumpInfo d;
    d.pid             = h.Pid;
    d.imageBase       = h.ImageBase;
    d.sizeOfImage     = h.SizeOfImage;
    d.unreadablePages = h.UnreadablePages;
    d.truncated       = (h.Flags & PROCSENTINEL_DUMP_FLAG_TRUNCATED) != 0;
    d.partial         = (h.Flags & PROCSENTINEL_DUMP_FLAG_PARTIAL) != 0;
    d.data            = bytes.subspan(sizeof(WireDumpHeader), h.DataLength);
    return d;
}

/* Build a DUMP_PROCESS request. A name longer than 15 chars is truncated:
 * the kernel short name is itself truncated, and the driver matches on it. */
inline WireDumpRequest makeDumpRequest(std::uint32_t pid, std::string_view name = {})
{
    WireDumpRequest r{};
    r.Pid = pid;
    if (pid == 0) {
        const std::size_t n = (std::min)(name.size(), static_cast<std::size_t>(PROCSENTINEL_MAX_NAME - 1));
        std::memcpy(r.ImageName, name.data(), n);
    }
    return r;
}

} // namespace procsentinel

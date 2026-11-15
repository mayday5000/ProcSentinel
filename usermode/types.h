#pragma once
/*
 * types.h — C++ domain types built on shared_protocol.h (single wire layout).
 * ProcessMonitor
 */

#include "../shared/shared_protocol.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace procmon {

constexpr size_t kMaxNameChars = PROCMON_MAX_NAME;
constexpr size_t kMaxPathChars = PROCMON_MAX_PATH;
constexpr const char* kEventLogDirectoryName = "procmon_events";

enum class EventType : std::uint8_t {
    Create = PROCMON_EVENT_CREATE,
    Exit   = PROCMON_EVENT_EXIT,
    State  = PROCMON_EVENT_STATE
};

enum class ProcessState : std::uint8_t {
    Running   = PROCMON_STATE_RUNNING,
    Exiting   = PROCMON_STATE_EXITING,
    Zombie    = PROCMON_STATE_ZOMBIE,
    Suspended = PROCMON_STATE_SUSPENDED
};

using ::WireEvent;
using ::WireStats;
using ::WireProcInfo;

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
        e.imageName       = wire.ImageName;
        e.imagePath       = wire.ImagePath;
        return e;
    }
};

struct ProcInfo {
    std::uint64_t pid{};
    std::uint64_t parentPid{};
    bool          protectedProcess{false};
    ProcessState  state{ProcessState::Running};
    std::string   imageName;
    std::string   imagePath;

    static ProcInfo fromWire(const WireProcInfo& wire)
    {
        ProcInfo p;
        p.pid               = wire.Pid;
        p.parentPid         = wire.ParentPid;
        p.protectedProcess  = (wire.Protected != 0);
        p.state             = static_cast<ProcessState>(wire.State);
        p.imageName         = wire.ImageName;
        p.imagePath         = wire.ImagePath;
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

} // namespace procmon

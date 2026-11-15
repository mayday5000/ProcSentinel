/*
 * procsentinel_app.cpp
 * User-mode companion for the ProcSentinelDrv kernel driver.
 *
 * Purpose:
 *   Open the control device (\\.\ProcSentinelDrv), issue METHOD_BUFFERED IOCTLs,
 *   and present process telemetry, live process lists, and memory dumps
 *   on an interactive console.
 *
 * How it works:
 *   1. DriverClient::open() creates a handle with CreateFileA.
 *   2. Menu commands map 1:1 to IOCTLs (GET_STATS, START/STOP, GET_EVENTS,
 *      GET_PROCESS_LIST, DUMP_PROCESS, RESET_STATS).
 *   3. Output buffers are wrapped in std::span; results are converted from
 *      the packed wire structs in shared_protocol.h into domain types.
 *   4. Errors are returned as std::expected and printed; the device handle
 *      is owned by UniqueHandle and closed on destruction.
 *
 * Build (see scripts/Build-UserMode.ps1):
 *   cl /std:c++latest /EHsc /I..\shared /I. procsentinel_app.cpp /Fe:procsentinel_app.exe
 */


#include "types.h"

#include <windows.h>
#include <winioctl.h>   /* CTL_CODE, METHOD_BUFFERED, FILE_DEVICE_UNKNOWN */

#include <conio.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace procsentinel {

/* =============================================================================
 * IOCTL codes — must match the kernel driver exactly
 * ============================================================================= */

constexpr DWORD IOCTL_PROCSENTINEL_GET_STATS        = CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB00, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD IOCTL_PROCSENTINEL_GET_EVENTS       = CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB01, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD IOCTL_PROCSENTINEL_START_MONITOR    = CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB02, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD IOCTL_PROCSENTINEL_STOP_MONITOR     = CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB03, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD IOCTL_PROCSENTINEL_RESET_STATS      = CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB04, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD IOCTL_PROCSENTINEL_GET_PROCESS_LIST = CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB05, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD IOCTL_PROCSENTINEL_DUMP_PROCESS     = CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB06, METHOD_BUFFERED, FILE_ANY_ACCESS);

/* =============================================================================
 * RAII HANDLE
 * ============================================================================= */

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE))
    {
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.handle_, INVALID_HANDLE_VALUE));
        }
        return *this;
    }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
    {
        // Guard against self-reset: h.reset(h.get()) must not close then store dangling.
        if (handle_ == handle) {
            return;
        }
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE release() noexcept
    {
        return std::exchange(handle_, INVALID_HANDLE_VALUE);
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

/* =============================================================================
 * DriverClient — thin wrapper around DeviceIoControl (move-only, by value)
 * ============================================================================= */

class DriverClient {
public:
    [[nodiscard]] static std::expected<DriverClient, std::string> open()
    {
        HANDLE h = CreateFileA(
            "\\\\.\\ProcSentinelDrv",
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
                return std::unexpected(
                    "Driver not loaded. Device \\\\.\\ProcSentinelDrv not found. "
                    "Run install script as Administrator first.");
            }
            return std::unexpected(std::format("CreateFile(ProcSentinelDrv) failed: Win32 error {}", err));
        }
        return DriverClient(UniqueHandle(h));
    }

    [[nodiscard]] std::expected<Stats, std::string> getStats()
    {
        WireStats wire{};
        DWORD returned = 0;
        if (!DeviceIoControl(handle_.get(), IOCTL_PROCSENTINEL_GET_STATS,
                             nullptr, 0, &wire, sizeof(wire), &returned, nullptr)) {
            return std::unexpected(std::format("GET_STATS failed: Win32 error {}", GetLastError()));
        }
        return Stats::fromWire(wire);
    }

    [[nodiscard]] bool startMonitor()
    {
        DWORD returned = 0;
        return DeviceIoControl(handle_.get(), IOCTL_PROCSENTINEL_START_MONITOR,
                               nullptr, 0, nullptr, 0, &returned, nullptr) != FALSE;
    }

    [[nodiscard]] bool stopMonitor()
    {
        DWORD returned = 0;
        return DeviceIoControl(handle_.get(), IOCTL_PROCSENTINEL_STOP_MONITOR,
                               nullptr, 0, nullptr, 0, &returned, nullptr) != FALSE;
    }

    [[nodiscard]] bool resetStats()
    {
        DWORD returned = 0;
        return DeviceIoControl(handle_.get(), IOCTL_PROCSENTINEL_RESET_STATS,
                               nullptr, 0, nullptr, 0, &returned, nullptr) != FALSE;
    }

    [[nodiscard]] std::expected<std::vector<Event>, std::string> getEvents(std::size_t maxCount = 512)
    {
        std::vector<Event> result;
        if (maxCount == 0) return result;

        const std::size_t n = (std::min)(maxCount, static_cast<std::size_t>(PROCSENTINEL_MAX_EVENTS));
        std::vector<std::uint8_t> buffer(n * sizeof(WireEvent));
        std::span<std::uint8_t> out{buffer};

        DWORD returned = 0;
        if (!DeviceIoControl(handle_.get(), IOCTL_PROCSENTINEL_GET_EVENTS,
                             nullptr, 0,
                             out.data(), static_cast<DWORD>(out.size_bytes()),
                             &returned, nullptr)) {
            return std::unexpected(std::format("GET_EVENTS failed: Win32 error {}", GetLastError()));
        }
        const std::size_t count = returned / sizeof(WireEvent);
        std::span<const WireEvent> events{
            reinterpret_cast<const WireEvent*>(out.data()), count};
        result.reserve(events.size());
        for (const auto& e : events) {
            result.push_back(Event::fromWire(e));
        }
        return result;
    }

    [[nodiscard]] std::expected<std::vector<ProcInfo>, std::string> getProcessList()
    {
        std::vector<std::uint8_t> buffer(256 * 1024);
        std::span<std::uint8_t> out{buffer};
        DWORD returned = 0;
        if (!DeviceIoControl(handle_.get(), IOCTL_PROCSENTINEL_GET_PROCESS_LIST,
                             nullptr, 0,
                             out.data(), static_cast<DWORD>(out.size_bytes()),
                             &returned, nullptr)) {
            return std::unexpected(std::format("GET_PROCESS_LIST failed: Win32 error {}", GetLastError()));
        }
        const std::size_t count = returned / sizeof(WireProcInfo);
        std::span<const WireProcInfo> list{
            reinterpret_cast<const WireProcInfo*>(out.data()), count};
        std::vector<ProcInfo> result;
        result.reserve(list.size());
        for (const auto& p : list) {
            result.push_back(ProcInfo::fromWire(p));
        }
        return result;
    }

    /* Returns number of bytes written to outFile, or unexpected on failure */
    [[nodiscard]] std::expected<std::size_t, std::string>
    dumpProcessByName(std::string_view name, const fs::path& outFile)
    {
        if (name.empty() || name.size() >= PROCSENTINEL_MAX_NAME) {
            return std::unexpected("process name empty or too long");
        }

        char nameBuf[PROCSENTINEL_MAX_NAME] = {};
        std::memcpy(nameBuf, name.data(), name.size());

        std::vector<std::uint8_t> dumpBuf(PROCSENTINEL_DUMP_MAX_SIZE);
        std::span<std::uint8_t> out{dumpBuf};
        DWORD returned = 0;
        if (!DeviceIoControl(handle_.get(), IOCTL_PROCSENTINEL_DUMP_PROCESS,
                             nameBuf, static_cast<DWORD>(name.size() + 1),
                             out.data(), static_cast<DWORD>(out.size_bytes()),
                             &returned, nullptr)) {
            return std::unexpected(std::format("DUMP_PROCESS failed: Win32 error {}", GetLastError()));
        }
        if (returned == 0) {
            return std::unexpected("DUMP_PROCESS returned 0 bytes");
        }

        std::ofstream ofs(outFile, std::ios::binary);
        if (!ofs) {
            return std::unexpected(std::format("cannot open output file {}", outFile.string()));
        }
        ofs.write(reinterpret_cast<const char*>(out.data()), returned);
        return static_cast<std::size_t>(returned);
    }

private:
    explicit DriverClient(UniqueHandle handle) : handle_(std::move(handle)) {}
    UniqueHandle handle_;
};

/* =============================================================================
 * Simple event log (optional persistence)
 * ============================================================================= */

class EventLog {
public:
    explicit EventLog(fs::path root = kEventLogDirectoryName)
        : root_(std::move(root))
    {
        std::error_code ec;
        fs::create_directories(root_, ec);
    }

    void append(const Event& e)
    {
        const fs::path file = root_ / "events.tsv";
        const bool isNew = !fs::exists(file);
        std::ofstream ofs(file, std::ios::app);
        if (!ofs) return;
        if (isNew) {
            ofs << "Timestamp\tPID\tPPID\tType\tState\tName\tPath\n";
        }
        ofs << e.timestamp << '\t'
            << e.processId << '\t'
            << e.parentProcessId << '\t'
            << toString(e.eventType) << '\t'
            << toString(e.state) << '\t'
            << e.imageName << '\t'
            << e.imagePath << '\n';
    }

    void appendBatch(const std::vector<Event>& events)
    {
        for (const auto& e : events) append(e);
    }

private:
    fs::path root_;
};

/* =============================================================================
 * Console helpers
 * ============================================================================= */

void printStats(const Stats& s)
{
    std::cout << "\n=== STATISTICS ===\n"
              << "Creates             : " << s.creates << '\n'
              << "Exits               : " << s.exits << '\n'
              << "StateChanges        : " << s.stateChanges << '\n'
              << "CurrentProcessCount : " << s.currentProcessCount << '\n'
              << "Events in ring      : " << s.eventCount << '\n'
              << "Dropped events      : " << s.droppedEvents << '\n'
              << "Dump requests       : " << s.dumpRequests << '\n'
              << "Dump failures       : " << s.dumpFailures << '\n';
}

void printEvents(const std::vector<Event>& events)
{
    std::cout << "\n=== TELEMETRY EVENTS (" << events.size() << ") ===\n";
    if (events.empty()) {
        std::cout << "(no events pending)\n";
        return;
    }
    std::cout << std::left
              << std::setw(12) << "PID"
              << std::setw(10) << "PPID"
              << std::setw(10) << "TYPE"
              << std::setw(10) << "STATE"
              << "NAME\n"
              << std::string(60, '-') << '\n';
    for (const auto& e : events) {
        std::cout << std::left
                  << std::setw(12) << e.processId
                  << std::setw(10) << e.parentProcessId
                  << std::setw(10) << toString(e.eventType)
                  << std::setw(10) << toString(e.state)
                  << e.imageName << '\n';
        if (!e.imagePath.empty()) {
            std::cout << "              Path: " << e.imagePath << '\n';
        }
    }
}

void printProcessList(const std::vector<ProcInfo>& list)
{
    std::cout << "\n=== LIVE PROCESS LIST (" << list.size() << ") ===\n";
    std::cout << std::left
              << std::setw(12) << "PID"
              << std::setw(12) << "PARENT"
              << std::setw(10) << "PROTECTED"
              << std::setw(10) << "STATE"
              << "NAME\n"
              << std::string(64, '-') << '\n';
    std::size_t protectedCount = 0;
    for (const auto& p : list) {
        std::cout << std::left
                  << std::setw(12) << p.pid
                  << std::setw(12) << p.parentPid
                  << std::setw(10) << (p.protectedProcess ? "YES" : "NO")
                  << std::setw(10) << toString(p.state)
                  << p.imageName << '\n';
        if (p.protectedProcess) ++protectedCount;
    }
    std::cout << "\n[SUMMARY] Protected processes: " << protectedCount
              << " / " << list.size() << '\n';
}

void printMenu()
{
    std::cout << "\n=== ProcSentinel User Application ===\n"
              << "1. Show statistics\n"
              << "2. Start monitoring\n"
              << "3. Stop monitoring\n"
              << "4. Reset stats / clear event ring\n"
              << "5. View telemetry events (create/exit)\n"
              << "6. Show live process list\n"
              << "7. Dump process memory by name\n"
              << "8. Drain events + append to event log\n"
              << "0. Exit\n"
              << "Choice: ";
}

} // namespace procsentinel

/* =============================================================================
 * main
 * ============================================================================= */

int main()
{
    using namespace procsentinel;

    std::cout << "=== ProcSentinel (C++23) ===\n"
              << "Built: " << __DATE__ << ' ' << __TIME__ << '\n'
              << "Requires ProcSentinelDrv kernel driver loaded.\n";

    auto opened = DriverClient::open();
    if (!opened) {
        std::cerr << "[ERROR] " << opened.error() << '\n';
        return 1;
    }
    DriverClient client = std::move(*opened);

    EventLog eventLog;
    int choice = -1;

    while (choice != 0) {
        printMenu();
        if (!(std::cin >> choice)) {
            std::cin.clear();
            std::string discard;
            std::getline(std::cin, discard);
            continue;
        }

        switch (choice) {
        case 1: {
            auto stats = client.getStats();
            if (stats) printStats(*stats);
            else std::cerr << "[ERROR] " << stats.error() << '\n';
            break;
        }
        case 2:
            std::cout << (client.startMonitor() ? "[OK] Monitoring STARTED\n"
                                                : "[ERROR] START failed\n");
            break;
        case 3:
            std::cout << (client.stopMonitor() ? "[OK] Monitoring STOPPED\n"
                                               : "[ERROR] STOP failed\n");
            break;
        case 4:
            std::cout << (client.resetStats() ? "[OK] Stats and event ring RESET\n"
                                              : "[ERROR] RESET failed\n");
            break;
        case 5: {
            auto events = client.getEvents();
            if (events) printEvents(*events);
            else std::cerr << "[ERROR] " << events.error() << '\n';
            break;
        }
        case 6: {
            auto list = client.getProcessList();
            if (list) printProcessList(*list);
            else std::cerr << "[ERROR] " << list.error() << '\n';
            break;
        }
        case 7: {
            auto list = client.getProcessList();
            if (list) printProcessList(*list);
            else std::cerr << "[ERROR] " << list.error() << '\n';
            std::cout << "\nEnter process short name (e.g. notepad.exe): ";
            std::string name;
            std::cin >> name;
            std::cout << "Enter output file path: ";
            std::string outPath;
            std::cin >> outPath;
            auto dumped = client.dumpProcessByName(name, outPath);
            if (dumped) {
                std::cout << "[SUCCESS] Dumped " << *dumped << " bytes of '" << name
                          << "' to " << outPath << '\n';
            } else {
                std::cerr << "[ERROR] " << dumped.error() << '\n';
            }
            break;
        }
        case 8: {
            auto events = client.getEvents(1024);
            if (events) {
                eventLog.appendBatch(*events);
                std::cout << "[OK] Appended " << events->size()
                          << " events to " << kEventLogDirectoryName << "/events.tsv\n";
            } else {
                std::cerr << "[ERROR] " << events.error() << '\n';
            }
            break;
        }
        case 0:
            break;
        default:
            std::cout << "Unknown choice\n";
            break;
        }
    }

    std::cout << "Goodbye.\n";
    return 0;
}

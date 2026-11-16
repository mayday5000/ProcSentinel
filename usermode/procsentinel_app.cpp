/*
 * procsentinel_app.cpp
 * User-mode companion for the ProcSentinelDrv kernel driver.
 *
 * Purpose:
 *   Open the control device (\\.\ProcSentinelDrv), issue IOCTLs (METHOD_BUFFERED,
 *   DUMP uses METHOD_OUT_DIRECT), and present process telemetry, live process
 *   lists, and memory dumps on an interactive console.
 *
 * Requirements:
 *   - Elevated prompt: the device only admits SYSTEM and Administrators.
 *   - SeDebugPrivilege is enabled at startup; the driver requires it for DUMP.
 *
 * How it works:
 *   1. DriverClient::open() creates a handle with CreateFileA.
 *   2. Menu commands map 1:1 to IOCTLs (GET_STATS, START/STOP, GET_EVENTS,
 *      GET_PROCESS_LIST, DUMP_PROCESS, RESET_STATS).
 *   3. Output buffers are wrapped in std::span and parsed with the helpers in
 *      types.h (parseEvents / parseProcessList / parseDump), which the host
 *      tests exercise too.
 *   4. Errors are returned as std::expected and printed; the device handle
 *      is owned by UniqueHandle and closed on destruction.
 *
 * Build (see scripts/Build-UserMode.ps1):
 *   cl /std:c++latest /EHsc /I..\shared /I. procsentinel_app.cpp advapi32.lib /Fe:procsentinel_app.exe
 */


#include "types.h"

#include <windows.h>
#include <winioctl.h>   /* CTL_CODE, METHOD_BUFFERED, FILE_DEVICE_UNKNOWN */

#include <conio.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace procsentinel {

/* =============================================================================
 * IOCTL codes come from shared_protocol.h; make sure the portable formula
 * matches the Windows CTL_CODE macro exactly.
 * ============================================================================= */

static_assert(IOCTL_PROCSENTINEL_GET_STATS        == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB00, METHOD_BUFFERED,   FILE_READ_DATA));
static_assert(IOCTL_PROCSENTINEL_GET_EVENTS       == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB01, METHOD_BUFFERED,   FILE_READ_DATA));
static_assert(IOCTL_PROCSENTINEL_START_MONITOR    == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB02, METHOD_BUFFERED,   FILE_WRITE_DATA));
static_assert(IOCTL_PROCSENTINEL_STOP_MONITOR     == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB03, METHOD_BUFFERED,   FILE_WRITE_DATA));
static_assert(IOCTL_PROCSENTINEL_RESET_STATS      == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB04, METHOD_BUFFERED,   FILE_WRITE_DATA));
static_assert(IOCTL_PROCSENTINEL_GET_PROCESS_LIST == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB05, METHOD_BUFFERED,   FILE_READ_DATA));
static_assert(IOCTL_PROCSENTINEL_DUMP_PROCESS     == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB06, METHOD_OUT_DIRECT, FILE_READ_DATA | FILE_WRITE_DATA));

/* =============================================================================
 * SeDebugPrivilege — the driver requires it (enabled) for DUMP_PROCESS.
 * Elevated administrators hold it, but it is disabled by default.
 * ============================================================================= */

[[nodiscard]] inline bool enableDebugPrivilege(std::string& error)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        error = std::format("OpenProcessToken failed: Win32 error {}", GetLastError());
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &tp.Privileges[0].Luid) != FALSE;
    if (!ok) {
        error = std::format("LookupPrivilegeValue failed: Win32 error {}", GetLastError());
    } else {
        ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) != FALSE;
        const DWORD err = GetLastError();
        if (!ok) {
            error = std::format("AdjustTokenPrivileges failed: Win32 error {}", err);
        } else if (err == ERROR_NOT_ALL_ASSIGNED) {
            ok = false;
            error = "SeDebugPrivilege is not held by this token (run elevated as Administrator)";
        }
    }
    CloseHandle(token);
    return ok;
}

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
            if (err == ERROR_ACCESS_DENIED) {
                return std::unexpected(
                    "Access denied opening \\\\.\\ProcSentinelDrv. The device only admits "
                    "SYSTEM and elevated Administrators: run this client from an elevated prompt.");
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

    [[nodiscard]] std::expected<std::vector<Event>, std::string> getEvents(std::size_t maxCount = PROCSENTINEL_MAX_EVENTS)
    {
        if (maxCount == 0) return std::vector<Event>{};

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
        return parseEvents(std::span<const std::uint8_t>{out.data(), (std::min)(static_cast<std::size_t>(returned), out.size())});
    }

    /* The driver returns at most PROCSENTINEL_MAX_PROCESS_LIST entries. */
    [[nodiscard]] std::expected<std::vector<ProcInfo>, std::string> getProcessList()
    {
        std::vector<std::uint8_t> buffer(PROCSENTINEL_MAX_PROCESS_LIST * sizeof(WireProcInfo));
        std::span<std::uint8_t> out{buffer};
        DWORD returned = 0;
        if (!DeviceIoControl(handle_.get(), IOCTL_PROCSENTINEL_GET_PROCESS_LIST,
                             nullptr, 0,
                             out.data(), static_cast<DWORD>(out.size_bytes()),
                             &returned, nullptr)) {
            return std::unexpected(std::format("GET_PROCESS_LIST failed: Win32 error {}", GetLastError()));
        }
        return parseProcessList(std::span<const std::uint8_t>{out.data(), (std::min)(static_cast<std::size_t>(returned), out.size())});
    }

    /* Dump by PID (pid != 0) or by short name. Writes only the image bytes
     * (not the protocol header) to outFile. */
    [[nodiscard]] std::expected<DumpInfo, std::string>
    dumpProcess(std::uint32_t pid, std::string_view name, const fs::path& outFile)
    {
        if (pid == 0 && name.empty()) {
            return std::unexpected("process name or PID required");
        }
        const WireDumpRequest request = makeDumpRequest(pid, name);

        dumpBuf_.assign(PROCSENTINEL_DUMP_BUFFER_SIZE, 0);
        DWORD returned = 0;
        if (!DeviceIoControl(handle_.get(), IOCTL_PROCSENTINEL_DUMP_PROCESS,
                             const_cast<WireDumpRequest*>(&request), static_cast<DWORD>(sizeof(request)),
                             dumpBuf_.data(), static_cast<DWORD>(dumpBuf_.size()),
                             &returned, nullptr)) {
            const DWORD err = GetLastError();
            std::string hint;
            switch (err) {
            case ERROR_PRIVILEGE_NOT_HELD: hint = " (SeDebugPrivilege not enabled)"; break;
            case ERROR_ACCESS_DENIED:      hint = " (target is a protected PP/PPL process)"; break;
            case ERROR_NOT_FOUND:          hint = " (no process with that name/PID)"; break;
            default: break;
            }
            return std::unexpected(std::format("DUMP_PROCESS failed: Win32 error {}{}", err, hint));
        }

        auto parsed = parseDump(std::span<const std::uint8_t>{
            dumpBuf_.data(), (std::min)(static_cast<std::size_t>(returned), dumpBuf_.size())});
        if (!parsed) {
            return std::unexpected(parsed.error());
        }

        std::ofstream ofs(outFile, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return std::unexpected(std::format("cannot open output file {}", outFile.string()));
        }
        ofs.write(reinterpret_cast<const char*>(parsed->data.data()),
                  static_cast<std::streamsize>(parsed->data.size()));
        ofs.close();
        if (!ofs) {
            return std::unexpected(std::format("failed writing {}", outFile.string()));
        }
        return parsed;
    }

private:
    explicit DriverClient(UniqueHandle handle) : handle_(std::move(handle)) {}
    UniqueHandle handle_;
    std::vector<std::uint8_t> dumpBuf_;   // header + up to PROCSENTINEL_DUMP_MAX_SIZE
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
              << "StateChanges        : " << s.stateChanges << "  (reserved)\n"
              << "CurrentProcessCount : " << s.currentProcessCount << "  (seeded on START)\n"
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
              << std::setw(11) << "PROTECTION"
              << std::setw(10) << "STATE"
              << "NAME\n"
              << std::string(65, '-') << '\n';
    std::size_t ppCount = 0;
    std::size_t pplCount = 0;
    for (const auto& p : list) {
        std::cout << std::left
                  << std::setw(12) << p.pid
                  << std::setw(12) << p.parentPid
                  << std::setw(11) << toString(p.protection)
                  << std::setw(10) << toString(p.state)
                  << p.imageName << '\n';
        if (p.protection == Protection::PP) ++ppCount;
        if (p.protection == Protection::PPL) ++pplCount;
    }
    std::cout << "\n[SUMMARY] Protected processes: " << (ppCount + pplCount)
              << " / " << list.size() << "  (PP: " << ppCount << ", PPL: " << pplCount << ")\n";
    if (list.size() >= PROCSENTINEL_MAX_PROCESS_LIST) {
        std::cout << "[NOTE] List capped at " << PROCSENTINEL_MAX_PROCESS_LIST << " entries.\n";
    }
}

void printMenu()
{
    std::cout << "\n=== ProcSentinel User Application ===\n"
              << "1. Show statistics\n"
              << "2. Start monitoring\n"
              << "3. Stop monitoring\n"
              << "4. Reset stats / clear event ring\n"
              << "5. Drain and view telemetry events (create/exit)\n"
              << "6. Show live process list\n"
              << "7. Dump process image (by name or PID)\n"
              << "8. Drain events + append to event log\n"
              << "0. Exit\n"
              << "Choice: ";
}

/* Read a whole line (paths may contain spaces). Trims surrounding blanks. */
std::string readLine(std::string_view prompt)
{
    std::cout << prompt;
    std::string line;
    std::getline(std::cin, line);
    const auto first = line.find_first_not_of(" \t\r\n\"");
    if (first == std::string::npos) return {};
    const auto last = line.find_last_not_of(" \t\r\n\"");
    return line.substr(first, last - first + 1);
}

bool isAllDigits(std::string_view s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
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
              << "Requires ProcSentinelDrv kernel driver loaded and an elevated prompt.\n";

    std::string privError;
    if (!enableDebugPrivilege(privError)) {
        std::cerr << "[WARNING] " << privError << ". Option 7 (dump) will be refused by the driver.\n";
    }

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
            if (std::cin.eof()) break;
            std::cin.clear();
            std::string discard;
            std::getline(std::cin, discard);
            continue;
        }
        {
            std::string rest;   // consume the rest of the menu line
            std::getline(std::cin, rest);
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

            const std::string target =
                readLine("\nEnter PID or process short name (e.g. 1234 or notepad.exe): ");
            if (target.empty()) {
                std::cerr << "[ERROR] no target given\n";
                break;
            }
            std::uint32_t pid = 0;
            std::string name;
            if (isAllDigits(target)) {
                try {
                    const unsigned long v = std::stoul(target);
                    if (v == 0 || v > 0xFFFFFFFFul) throw std::out_of_range("pid");
                    pid = static_cast<std::uint32_t>(v);
                } catch (const std::exception&) {
                    std::cerr << "[ERROR] invalid PID\n";
                    break;
                }
            } else {
                name = target;
                if (name.size() >= PROCSENTINEL_MAX_NAME) {
                    std::cout << "[NOTE] Name longer than " << (PROCSENTINEL_MAX_NAME - 1)
                              << " chars; the driver matches on the truncated kernel short name.\n";
                }
            }
            const std::string outPath = readLine("Enter output file path: ");
            if (outPath.empty()) {
                std::cerr << "[ERROR] no output path given\n";
                break;
            }

            auto dumped = client.dumpProcess(pid, name, fs::path(outPath));
            if (dumped) {
                std::cout << std::format("[SUCCESS] Dumped {} bytes of PID {} (ImageBase 0x{:X}, SizeOfImage {}) to {}\n",
                                         dumped->data.size(), dumped->pid, dumped->imageBase,
                                         dumped->sizeOfImage, outPath);
                if (dumped->truncated) {
                    std::cout << std::format("[WARNING] Truncated: SizeOfImage {} exceeds the {} byte cap.\n",
                                             dumped->sizeOfImage, PROCSENTINEL_DUMP_MAX_SIZE);
                }
                if (dumped->partial) {
                    std::cout << std::format("[WARNING] Partial: {} unreadable page(s) were zero-filled.\n",
                                             dumped->unreadablePages);
                }
            } else {
                std::cerr << "[ERROR] " << dumped.error() << '\n';
            }
            break;
        }
        case 8: {
            auto events = client.getEvents();
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

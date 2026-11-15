# ProcessMonitor

A Windows kernel-mode (WDM) process activity monitor paired with a C++23 user-mode control client and host-side unit tests.

The driver tracks real-time process creation and termination via kernel callbacks, stores telemetry in a spinlock-protected non-paged ring buffer, and exposes a control device (`\\.\ProcMonDrv`) over `METHOD_BUFFERED` IOCTLs. The user-mode console client queries live process snapshots, drains telemetry events to TSV logs, and performs bounded process image memory reads.

## Features

- **Kernel-mode WDM, not user-mode polling.** Process create and exit are delivered by `PsSetCreateProcessNotifyRoutineEx`. The client does not poll ETW or `CreateToolhelp32Snapshot` for lifecycle events.
- **SysProcTap.** Passive kernel callback registration. No SSDT hooks, no inline hooks, no DKOM.
- **Real-time lifecycle telemetry.** Create and exit events land in a fixed NonPaged ring (2048 entries, drop-oldest) protected by a `KSPIN_LOCK`.
- **Live process snapshots.** `GET_PROCESS_LIST` walks `SystemProcessInformation` and reports PID, parent, short name, state, and protected-process status.
- **Protected-process detection, not bypass.** Status is read with `PsIsProtectedProcess` / `PsIsProtectedProcessLight`. Protection flags are never written.
- **Bounded image dump by short name.** `DUMP_PROCESS` copies at most 4 MB with `MmCopyVirtualMemory` into the METHOD_BUFFERED system buffer.
- **METHOD_BUFFERED IOCTLs.** The I/O manager probes and copies user buffers. Dispatch never touches a raw user pointer.
- **Single packed wire layout.** `shared/shared_protocol.h` is the only definition of `WireEvent`, `WireProcInfo`, and `WireStats` for the driver, the C++23 client, and the host tests.
- **Host unit tests without a loaded driver.** Ring overflow, `fromWire`, and a mock client run on the build machine.
- **Interactive C++23 client.** RAII device handle, `std::expected` error returns, `std::span` buffers, menu for stats, start/stop, drain, list, dump, and TSV append.
- **Lab install path.** PowerShell scripts build the driver, create the kernel service, and generate KDNET / WinDbg helpers.

> **Scope & Design Boundaries:** This is a classic WDM kernel driver—not a file-system minifilter. It performs **no DKOM** (Direct Kernel Object Manipulation) and never patches `EPROCESS` protection flags; protected process status is strictly read via kernel APIs, and cross-process memory reads use `MmCopyVirtualMemory`.

---

## Architecture

```text
  User Mode (C++23)                     Kernel Mode (WDM C)
  +------------------+                  +-----------------------------------+

  | procmon_app.exe  |                  | procmon_driver.sys                |
  |                  |  CreateFileA     |                                   |
  |  Interactive CLI |----------------->|  \Device\ProcMonDrv               |
  |  DriverClient    |  DeviceIoControl |  Symlink: \\.\ProcMonDrv          |
  |  (RAII Handle)   |<-----------------|                                   |
  +------------------+                  |  PsSetCreateProcessNotifyRoutineEx|

                                        |            |                      |
                                        |            v                      |
                                        |  ProcessNotifyCallback            |
                                        |            |                      |
                                        |            v                      |
                                        |  NonPaged Ring Buffer + Stats     |
                                        |  (Guarded by KSPIN_LOCK)          |
                                        +-----------------------------------+
```

### Repository Layout & Single-Source Wire Protocol

```text
  shared/shared_protocol.h
           │
           │  #pragma pack(1) wire structs: WireEvent / WireProcInfo / WireStats
           │  Compile-time C_ASSERT layout guarantees across C and C++
           │
     ┌─────┴──────────────────┬──────────────────┐
     ▼                        ▼                  ▼
  driver/                 usermode/          mock_tests/
  procmon_driver.c        types.h            main_test_runner.cpp
  (WDM Kernel C)          procmon_app.cpp    (Ring logic, fromWire, mock client)
                          (C++23 Client)
```

* **Zero Schema Drift:** Wire structures live exclusively in `shared/shared_protocol.h` with `#pragma pack(1)` and `C_ASSERT` size checks so kernel C and user-mode C++ cannot disagree on alignment or field widths. Timestamps use 64-bit 100-nanosecond intervals matching `LARGE_INTEGER`.
* **Offline Host Testing:** `mock_tests/` validates ring wrap-around/overflow behavior, `fromWire()` deserialization, and client error paths on the host without loading a kernel driver.

---

## Kernel Driver Internals (`driver/procmon_driver.c`)

### 1. Process Notification Path

`DriverEntry` initializes the `KSPIN_LOCK` and global ring buffer, creates `\Device\ProcMonDrv` with `FILE_DEVICE_SECURE_OPEN`, and registers `ProcessNotifyCallback` via `PsSetCreateProcessNotifyRoutineEx`.

```text
  PsSetCreateProcessNotifyRoutineEx
            │
            ▼
  ProcessNotifyCallback (PASSIVE_LEVEL, kernel APCs disabled)
            │
            ├── CreateInfo != NULL ──► PROCMON_EVENT_CREATE
            │     • Short name via PsGetProcessImageFileName()
            │     • Full image path via CreateInfo->ImageFileName (when available)
            │
            └── CreateInfo == NULL ──► PROCMON_EVENT_EXIT
            │
            ▼
  KeAcquireSpinLock(&g_RingLock)  ──► Raises IRQL to DISPATCH_LEVEL
            │
            ▼
  EventRingPushKernel()
            │  • Fixed capacity: PROCMON_MAX_EVENTS (2048 entries)
            │  • Overflow policy: Drop-oldest (advances tail, increments DroppedEvents)
            ▼
  KeReleaseSpinLock(&g_RingLock)  ──► Restores IRQL to PASSIVE_LEVEL
```

### 2. IOCTL Control Interface

All control codes use `METHOD_BUFFERED`. The Windows I/O Manager allocates a non-paged `SystemBuffer` and handles user-mode buffer probing, keeping the driver dispatch routines safe from raw user-pointer dereferences.

| IOCTL Code | Direction | Kernel Behavior |
| :--- | :--- | :--- |
| `IOCTL_PROCMON_GET_STATS` | Out | Copies a consistent snapshot of `PROCMON_STATS` under `g_RingLock`. |
| `IOCTL_PROCMON_GET_EVENTS` | Out | Drains up to `outLen / sizeof(PROCMON_EVENT)` events from the ring tail under lock. |
| `IOCTL_PROCMON_START_MONITOR` | None | Sets `g_MonitoringEnabled = TRUE` to begin recording notify callbacks. |
| `IOCTL_PROCMON_STOP_MONITOR` | None | Sets `g_MonitoringEnabled = FALSE` to pause event ingestion. |
| `IOCTL_PROCMON_RESET_STATS` | None | Zeroes counters and resets ring indices under `g_RingLock`. |
| `IOCTL_PROCMON_GET_PROCESS_LIST` | Out | Queries `SystemProcessInformation`, looks up each `PEPROCESS`, and checks protection status. |
| `IOCTL_PROCMON_DUMP_PROCESS` | In / Out | Resolves target `PEPROCESS` by short image name, parses PE `SizeOfImage`, and copies up to `PROCMON_DUMP_MAX_SIZE` (4 MB) via `MmCopyVirtualMemory`. |

### 3. IRQL & Memory Residency Rules

| Execution Context | IRQL | Memory Constraints |
| :--- | :--- | :--- |
| `DriverEntry` / `DriverUnload` | `PASSIVE_LEVEL` | Paged or NonPaged allowed |
| `ProcessNotifyCallback` | `PASSIVE_LEVEL` (APCs disabled) | Ring buffer & globals reside in NonPaged `.bss` |
| Inside `g_RingLock` critical section | `DISPATCH_LEVEL` | Strictly NonPaged memory; zero page faults permitted |
| `DispatchDeviceControl` | `PASSIVE_LEVEL` | `Irp->AssociatedIrp.SystemBuffer` is NonPaged |

---

## User-Mode Client (`usermode/`)

`procmon_app.exe` opens `\\.\ProcMonDrv` via `DriverClient::open()` and manages the device descriptor using an RAII `UniqueHandle` wrapper.

```text
  main()
    │
    ▼
  DriverClient::open() ──► CreateFileA("\\\\.\\ProcMonDrv")
    │
    ▼
  Interactive Menu Loop
    │
    ├──► DriverClient methods ──► DeviceIoControl(METHOD_BUFFERED)
    │          │
    │          ▼
    │    std::span<const std::byte> over output buffer
    │          │
    │          ▼
    │    fromWire() ──► Domain structs (std::expected<T, std::string>)
    │
    └──► EventLog::appendBatch() ──► Appends TSV rows to procmon_events/events.tsv
```

### CLI Menu Reference

| Option | Operation | Underlying IOCTL |
| :---: | :--- | :--- |
| **1** | Print live telemetry counters (creates, exits, queued, dropped, dumps) | `IOCTL_PROCMON_GET_STATS` |
| **2** | Enable kernel process monitoring | `IOCTL_PROCMON_START_MONITOR` |
| **3** | Disable kernel process monitoring | `IOCTL_PROCMON_STOP_MONITOR` |
| **4** | Reset statistics and flush the kernel event ring | `IOCTL_PROCMON_RESET_STATS` |
| **5** | Drain and display pending create/exit events | `IOCTL_PROCMON_GET_EVENTS` |
| **6** | Display live process snapshot (PID, PPID, Protected flag, State, Name) | `IOCTL_PROCMON_GET_PROCESS_LIST` |
| **7** | Dump process memory image by short name (e.g., `notepad.exe`) to disk | `IOCTL_PROCMON_DUMP_PROCESS` |
| **8** | Drain pending events and append to `procmon_events/events.tsv` | `IOCTL_PROCMON_GET_EVENTS` |
| **0** | Close device handle and exit | — |

---

## Build & Lab Deployment Notes

1. **Linker Integrity Flag (Required):** Any driver registering `PsSetCreateProcessNotifyRoutineEx` **must** be linked with `/INTEGRITYCHECK` (`IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY`), or registration fails at runtime with `STATUS_ACCESS_DENIED` (`0xC0000022`).
2. **Test Signing:** Enable test-signed drivers on the target VM prior to loading:
   ```cmd
   bcdedit /set testsigning on
   ```
3. **Service Registration (Admin CMD/PowerShell):**
   ```cmd
   sc.exe create ProcMonDrv type= kernel binPath= C:\Windows\System32\drivers\procmon_driver.sys
   sc.exe start ProcMonDrv
   ```

---

## Requirements

- Windows 10 or later, x64
- Visual Studio 2019 or 2022 with the Desktop C++ workload
- Windows Driver Kit matching the installed Windows SDK
- Administrator rights to install or uninstall the service
- Test signing enabled on the machine that loads the driver (`bcdedit /set testsigning on`, reboot)

The user-mode client is C++23. The build script compiles it with `/std:c++latest`.

---

## Build

From the `scripts` directory.

### User-mode client and tests

```powershell
.\Build-UserMode.ps1
```

Compiles `procmon_app.exe` and `procmon_unified_tests.exe`.

```powershell
.\Build-UserMode.ps1 -RunTests
```

Compiles and runs the unified tests.

Example:

```
=== ProcessMonitor Unified Tests ===

--- Low-Level C Ring ---
[PASS] Low-Level C Ring Assertions Complete
--- Protocol / fromWire ---
[PASS] Protocol / fromWire Complete
--- MockDriverClient ---
[PASS] MockDriverClient Complete

====================================
ALL TESTS PASSED
```

### Driver

```powershell
.\Build-Driver.ps1
```

Builds `driver\x64\Debug\procmon_driver.sys` (or Release if requested). On success it also writes `driver\debug.wds`, `driver\WinDbgWorkspace.xml`, and `scripts\KDNet-ProcMonDrv.ps1`.

```powershell
.\Build-Driver.ps1 -Configuration Release -Platform x64
.\Build-Driver.ps1 -Clean
```

---

## Install and run

Run the install script as Administrator after the driver has been built.

```powershell
.\install_procmon.ps1
```

Typical successful output:

```
================================================
  ProcessMonitor - Driver Installation
  Service  : ProcMonDrv  (WDM kernel)
================================================

[OK] Found driver binary: ...\driver\x64\Debug\procmon_driver.sys
Stopping / deleting old service (if any)...
Copying procmon_driver.sys -> C:\Windows\System32\drivers\procmon_driver.sys ...
[SUCCESS] Driver binary copied.
[SUCCESS] PDB copied.
Creating kernel service...
[SC] CreateService SUCCESS
[SUCCESS] Service created.
Starting driver...

SERVICE_NAME: ProcMonDrv
        TYPE               : 1  KERNEL_DRIVER
        STATE              : 4  RUNNING
                                (STOPPABLE, NOT_PAUSABLE, IGNORES_SHUTDOWN)
        WIN32_EXIT_CODE    : 0  (0x0)
        SERVICE_EXIT_CODE  : 0  (0x0)

[SUCCESS] Driver started.
================================================
  INSTALLATION COMPLETE
  Run procmon_app.exe as Administrator.
================================================
```

Verify from PowerShell (use `sc.exe`, not `sc` — `sc` is an alias for `Set-Content`):

```powershell
sc.exe query ProcMonDrv
```

```
SERVICE_NAME: ProcMonDrv
        TYPE               : 1  KERNEL_DRIVER
        STATE              : 4  RUNNING
```

Start the client as Administrator:

```powershell
..\usermode\x64\Debug\procmon_app.exe
```

Example session:

```
=== ProcessMonitor (C++23) ===
Built: Nov 15 2026 13:40:00
Requires ProcMonDrv kernel driver loaded.

=== ProcessMonitor User Application ===
1. Show statistics
2. Start monitoring
3. Stop monitoring
4. Reset stats / clear event ring
5. View telemetry events (create/exit)
6. Show live process list
7. Dump process memory by name
8. Drain events + append to event log
0. Exit
Choice: 2
[OK] Monitoring STARTED

Choice: 1
=== STATISTICS ===
Creates             : 12
Exits               : 3
StateChanges        : 0
CurrentProcessCount : 0
Events in ring      : 15
Dropped events      : 0
Dump requests       : 0
Dump failures       : 0

Choice: 5
=== TELEMETRY EVENTS (15) ===
PID         PPID      TYPE      STATE     NAME
------------------------------------------------------------
1234        567       CREATE    Running   notepad.exe
              Path: C:\Windows\System32\notepad.exe
...

Choice: 6
=== LIVE PROCESS LIST (148) ===
PID         PARENT      PROTECTED STATE     NAME
----------------------------------------------------------------
4           0           NO        Running   System
...
[SUMMARY] Protected processes: 6 / 148

Choice: 0
Goodbye.
```

If the driver is not loaded:

```
[ERROR] Driver not loaded. Device \\.\ProcMonDrv not found. Run install script as Administrator first.
```

Uninstall (Administrator):

```powershell
.\uninstall_procmon.ps1
```

```
================================================
  ProcessMonitor - Uninstall
================================================
[OK] Service stopped.
[OK] Service deleted.
[OK] Binary removed.
```

---

## Scripts

All scripts live in `scripts/`. Run them from that directory.

### Build-Driver.ps1

Builds the WDM driver with MSBuild and, after a successful build, generates WinDbg / KDNET helpers.

**When to use:** after changing `procmon_driver.c` or the project files, before install.

```powershell
.\Build-Driver.ps1
.\Build-Driver.ps1 -Configuration Release -Platform x64
.\Build-Driver.ps1 -Clean
```

**Produces:** `driver\x64\<Config>\procmon_driver.sys`, `driver\debug.wds`, `driver\WinDbgWorkspace.xml`, `scripts\KDNet-ProcMonDrv.ps1`.

**Example output (success):**

```
Building procmon_driver (Debug|x64)...
[SUCCESS] procmon_driver.sys
[OK] Wrote driver\debug.wds
[OK] Wrote driver\WinDbgWorkspace.xml
[OK] Wrote scripts\KDNet-ProcMonDrv.ps1
```

### Build-UserMode.ps1

Compiles the C++23 client and the unified mock tests with `cl`. Does not build or load the driver.

**When to use:** after changing `procmon_app.cpp`, `types.h`, `shared_protocol.h`, or the test runner. Use `-RunTests` before packaging.

```powershell
.\Build-UserMode.ps1
.\Build-UserMode.ps1 -RunTests
.\Build-UserMode.ps1 -SkipApp
.\Build-UserMode.ps1 -SkipTests
```

**Produces:** `usermode\x64\<Config>\procmon_app.exe` and `mock_tests\x64\<Config>\procmon_unified_tests.exe`.

**Example output:**

```
App           : procmon_app.cpp
Tests         : main_test_runner.cpp
[OK] procmon_app.exe
[OK] procmon_unified_tests.exe
=== ProcessMonitor Unified Tests ===
ALL TESTS PASSED
```

### install_procmon.ps1 / install_procmon.bat

Stops any previous `ProcMonDrv` service, copies `procmon_driver.sys` (and the PDB) into `C:\Windows\System32\drivers`, creates a kernel service with `sc.exe`, and starts it.

**When to use:** after a successful driver build, on a machine with test signing enabled. Must run as Administrator.

```powershell
.\install_procmon.ps1
.\install_procmon.ps1 -Configuration Release
```

The `.bat` is a thin wrapper around the same PowerShell script.

### uninstall_procmon.ps1 / uninstall_procmon.bat

Stops the service, deletes it, and removes the copied `.sys` and `.pdb`.

**When to use:** before replacing the binary, or when you are done with the lab machine.

```powershell
.\uninstall_procmon.ps1
```

### Increment-Version.ps1

Clones the current tree into a sibling `ProcessMonitor_vN` folder so a stuck or still-loaded binary does not block the next build. Optional. See the dedicated section below for what it rewrites and what it does not isolate.

**When to use:** when you want a numbered sibling workspace for the next lab iteration.

```powershell
.\Increment-Version.ps1
```

### Typical order

```
Build-UserMode.ps1 -RunTests
Build-Driver.ps1
install_procmon.ps1
procmon_app.exe
uninstall_procmon.ps1
```

---

## Versioning & Parallel Lab Builds (`scripts/Increment-Version.ps1`)

During kernel driver development, an unloaded or stuck driver binary (`.sys`) can remain locked in memory until reboot, blocking subsequent builds or service restarts. `scripts/Increment-Version.ps1` streamlines iterative testing by cloning the workspace into a versioned sibling directory (`ProcessMonitor_vN`) with isolated build and deployment identifiers.

### What It Does

1. **Workspace Cloning:** Detects the current folder's `_vN` suffix (or initializes `_v1` from an unversioned root) and copies the full project tree into `../ProcessMonitor_v(N+1)`.
2. **Artifact & Script Renaming:** Updates version suffixes across build, install, and binary targets (e.g., `procmon_driver_v1` → `procmon_driver_v2`, `Build-Driver_v1.ps1` → `Build-Driver_v2.ps1`).
3. **Configuration & Altitude Rewriting:** Replaces project/version identifiers across `.c`, `.cpp`, `.h`, `.ps1`, `.inf`, and `.vcxproj` files, and increments the registration altitude (`+1`) in deployment scripts to avoid altitude conflicts if filter or callback altitudes are used.

### Usage

Run from the `scripts\` directory of your current version:

```powershell
cd ProcessMonitor_v1\scripts
.\Increment-Version.ps1
```

> **Note on Device Names:** While `Increment-Version.ps1` isolates binary filenames, service names, and altitude values across versions, `\Device\ProcMonDrv` and `\\.\ProcMonDrv` remain shared unless version-suffixed—so only one driver instance should be started at a time.

### What the script isolates, and what it does not

The script is a lab helper for stuck binaries. It is not a full multi-instance isolation tool. It copies the tree, then (when the current folder already has a `_vN` suffix) renames matching files and does a literal text replace of the old version tag and the altitude number.

**Handled when moving from a versioned folder (for example `_v1` → `_v2`):**

- Binary and script names that already contain the old tag are rewritten, so the new tree does not keep writing the same `.sys` name the previous service still holds.
- The altitude literal in install scripts is bumped by one. That only matters if an install script actually registers a minifilter altitude. This driver uses `PsSetCreateProcessNotifyRoutineEx`, which has no altitude.

**Not handled:**

- First run from an unversioned folder (`ProcessMonitor` → `ProcessMonitor_v1`): there is no old tag, so file renames of `procmon_driver` / `Build-Driver.ps1` are skipped. The clone keeps the same binary name.
- Device and symlink names in the driver (`\Device\ProcMonDrv`, `\DosDevices\ProcMonDrv`) do not contain a version tag. Two loaded copies try to create the same object and the second fails with `STATUS_OBJECT_NAME_COLLISION` (`0xC0000035`). Start only one instance.
- The text replace is unanchored. A tag such as `v1` is replaced wherever it appears in the scanned files. Do not rely on the script to rewrite only identifiers.
- The altitude number is replaced wherever that exact digit string appears, not only in an altitude assignment.

---

## Wire protocol

Defined in `shared/shared_protocol.h`. Packed 1-byte alignment.

`WireEvent` — timestamp, process id, parent process id, event type, state, short name (16), path (260).

`WireProcInfo` — pid, parent pid, protected flag, state, short name, path.

`WireStats` — creates, exits, state changes, current process count, event count, dropped events, dump requests, dump failures.

Event types: `CREATE` (1), `EXIT` (2), `STATE` (3).

States: `Running` (0), `Exiting` (1), `Zombie` (2), `Suspended` (3).

Limits: 2048 events in the ring, process list capped at 1024 entries, dump capped at 4 MB.

---

## Testing without the driver

The mock runner does not need Windows or a loaded driver. It checks:

1. C ring helpers (push, pop, drop-oldest when full, reset).
2. `fromWire` conversion of event, process, and stats structs.
3. A mock client that implements the same operations the real client exposes.

On a machine with a C++23 compiler:

```bash
g++ -std=c++23 -I shared -I usermode -o procmon_unified_tests mock_tests/main_test_runner.cpp
./procmon_unified_tests
```

Expected result: `ALL TESTS PASSED`.

---

## Failure modes

| Symptom | Likely cause |
|---------|----------------|
| `Device \\.\ProcMonDrv not found` | Service not installed or not started |
| `sc query` shows nothing in PowerShell | `sc` is `Set-Content`; use `sc.exe query ProcMonDrv` |
| Driver will not load | Test signing off, or binary not signed for this machine |
| `DUMP_PROCESS` returns 0 | Name mismatch (must be the short image name), or process exited |
| Ring count stays 0 | Monitoring was not started, or events were already drained |
| Dropped events increase | Ring is full (2048); oldest entries were discarded |
| `STATUS_OBJECT_NAME_COLLISION` on load | Another instance already created `\Device\ProcMonDrv` |

---

## License and scope

This is a lab driver for process telemetry and bounded image dumps. It is not a production security product. Load it only on a test machine with test signing enabled.

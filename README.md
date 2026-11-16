# ProcSentinel

**ProcSentinel** is a Windows kernel-mode (WDM) driver for process monitoring, PPL detection, and process memory image capture, paired with a C++23 user-mode client and host-side unit tests.

The driver receives process create and exit events through `PsSetCreateProcessNotifyRoutineEx`, buffers them in a spinlock-protected NonPaged ring, and exposes an admin-only control device (`\\.\ProcSentinelDrv`) over IOCTLs. From the client you can list live processes with their PP/PPL protection level, drain telemetry events to TSV logs, and capture up to 4 MB of a process's memory image by name or PID (protected processes are refused).

**Author:** Damian Andres Ulanowicz

## Features

- **Kernel-mode WDM, not user-mode polling.** Process create and exit are delivered by `PsSetCreateProcessNotifyRoutineEx`. The client does not poll ETW or `CreateToolhelp32Snapshot` for lifecycle events.
- **SysProcTap.** Passive kernel callback registration. No SSDT hooks, no inline hooks, no DKOM.
- **Real-time lifecycle telemetry.** Create and exit events land in a fixed NonPaged ring (2048 entries, drop-oldest) protected by a `KSPIN_LOCK`.
- **Live process snapshots.** `GET_PROCESS_LIST` walks `SystemProcessInformation` and reports PID, parent, short name, full image path, state, and protection level (none / PP / PPL). At most 1024 entries per call.
- **Protected-process detection, not bypass.** The level is read with `PsIsProtectedProcess` / `PsIsProtectedProcessLight`. Protection flags are never written, and PP/PPL processes are never dumped.
- **Locked-down control device.** `\Device\ProcSentinelDrv` is created with `IoCreateDeviceSecure` and `SDDL_DEVOBJ_SYS_ALL_ADM_ALL`, so only SYSTEM and elevated Administrators can open it. Each IOCTL carries the access bits it needs.
- **Bounded image dump by PID or short name.** `DUMP_PROCESS` requires an enabled `SeDebugPrivilege`, copies at most 4 MB of the main image with `MmCopyVirtualMemory`, and returns a header that flags truncated images and zero-filled unreadable pages.
- **No raw user pointers in dispatch.** Six IOCTLs are `METHOD_BUFFERED`. `DUMP_PROCESS` is `METHOD_OUT_DIRECT`: the output is an I/O-manager-probed MDL, and the PE headers are parsed from a private kernel copy.
- **Single packed wire layout.** `shared/shared_protocol.h` is the only definition of the IOCTL codes and of `WireEvent`, `WireProcInfo`, `WireStats`, `WireDumpRequest`, and `WireDumpHeader` for the driver, the C++23 client, and the host tests. Sizes and offsets are checked at compile time.
- **Host unit tests without a loaded driver.** Ring behavior, wire layout, IOCTL bits, the client's parsers, and a mock client run on the build machine.
- **Interactive C++23 client.** RAII device handle, `std::expected` error returns, `std::span` buffers, menu for stats, start/stop, drain, list, dump, and TSV append.
- **Lab install path.** PowerShell scripts build the driver, create the kernel service, and generate KDNET / WinDbg helpers.

> **Scope & Design Boundaries:** This is a classic WDM kernel driver—not a file-system minifilter. It performs **no DKOM** (Direct Kernel Object Manipulation) and never patches `EPROCESS` protection flags; protected process status is strictly read via kernel APIs, and cross-process memory reads use `MmCopyVirtualMemory`.

---

## Architecture

```text
  User Mode (C++23)                     Kernel Mode (WDM C)
  +-----------------------+             +--------------------------------------+
  | procsentinel_app.exe  |             | procsentinel_driver.sys              |
  |  (elevated)           |  CreateFileA|                                      |
  |  Interactive CLI      |------------>|  \Device\ProcSentinelDrv             |
  |  DriverClient         |  DeviceIo-  |  (SDDL: SYSTEM + Administrators)     |
  |  (RAII Handle)        |<------------|  Symlink: \\.\ProcSentinelDrv        |
  +-----------------------+  Control    |                                      |
                                        |  PsSetCreateProcessNotifyRoutineEx   |
                                        |            |                         |
                                        |            v                         |
                                        |  ProcessNotifyCallback               |
                                        |            |                         |
                                        |            v                         |
                                        |  NonPaged Ring Buffer + Stats        |
                                        |  (Guarded by KSPIN_LOCK)             |
                                        +--------------------------------------+
```

### Repository Layout & Single-Source Wire Protocol

```text
  shared/shared_protocol.h
           │
           │  IOCTL codes + #pragma pack(1) wire structs:
           │  WireEvent / WireProcInfo / WireStats / WireDumpRequest / WireDumpHeader
           │  Compile-time size and offset checks for C and C++
           │
     ┌─────┴────────────────────────┬──────────────────────────┐
     ▼                              ▼                          ▼
  driver/                        usermode/                  mock_tests/
  procsentinel_driver.c          types.h  (parsers)         main_test_runner.cpp
  (WDM Kernel C)                 procsentinel_app.cpp       (ring, protocol, parsers,
                                 (C++23 Client)              mock client)
```

* **Zero Schema Drift:** IOCTL codes and wire structures live only in `shared/shared_protocol.h`. Structs are `#pragma pack(1)`, and their sizes and key offsets are asserted at compile time in both C and C++. The driver and the client also assert that the portable IOCTL formula equals the Windows `CTL_CODE` macro. Timestamps are 64-bit 100-nanosecond system time, matching `LARGE_INTEGER`.
* **Offline Host Testing:** `mock_tests/` checks ring wrap-around, overflow and reset, wire layout and IOCTL bits, the `parseEvents` / `parseProcessList` / `parseDump` helpers the real client uses, and a mock client. None of it needs a kernel driver.

---

## Kernel Driver Internals (`driver/procsentinel_driver.c`)

### 1. Process Notification Path

`DriverEntry` (in the `INIT` section) initializes the `KSPIN_LOCK` and the ring buffer. It creates `\Device\ProcSentinelDrv` with `IoCreateDeviceSecure` (SDDL `D:P(A;;GA;;;SY)(A;;GA;;;BA)`, `FILE_DEVICE_SECURE_OPEN`, its own device class GUID) and registers `ProcessNotifyCallback` with `PsSetCreateProcessNotifyRoutineEx`. If registration fails, for example with `STATUS_ACCESS_DENIED` because the image was not linked with `/INTEGRITYCHECK`, the driver deletes its device and symlink and fails the load.

```text
  PsSetCreateProcessNotifyRoutineEx
            │
            ▼
  ProcessNotifyCallback (PASSIVE_LEVEL, critical region)
            │  • returns at once unless monitoring is enabled (ReadBooleanNoFence)
            │  • short name via PsGetProcessImageFileName()
            │
            ├── CreateInfo != NULL ──► PROCSENTINEL_EVENT_CREATE
            │     • Full image path (UTF-8) via CreateInfo->ImageFileName (when available)
            │     • Creates++, CurrentProcessCount++
            │
            └── CreateInfo == NULL ──► PROCSENTINEL_EVENT_EXIT
                  • Exits++, CurrentProcessCount-- (never below 0)
            │
            ▼
  KeAcquireSpinLock(&g_RingLock)  ──► Raises IRQL to DISPATCH_LEVEL
            │
            ▼
  EventRingPushKernel()
            │  • Fixed capacity: PROCSENTINEL_MAX_EVENTS (2048 entries)
            │  • Overflow policy: Drop-oldest (advances tail, increments DroppedEvents)
            ▼
  KeReleaseSpinLock(&g_RingLock)  ──► Restores IRQL to PASSIVE_LEVEL
```

`DriverUnload` removes the callback first (removal waits for in-flight callbacks), then deletes the symlink and the device.

### 2. IOCTL Control Interface

The device ACL admits only SYSTEM and elevated Administrators. On top of that, each IOCTL declares the handle access it needs, and `DUMP_PROCESS` checks `SeDebugPrivilege`.

| IOCTL Code | Method / Access | Direction | Kernel Behavior |
| :--- | :--- | :--- | :--- |
| `IOCTL_PROCSENTINEL_GET_STATS` | Buffered / Read | Out | Copies a consistent snapshot of `PROCSENTINEL_STATS` under `g_RingLock`. |
| `IOCTL_PROCSENTINEL_GET_EVENTS` | Buffered / Read | Out | Drains up to `outLen / sizeof(PROCSENTINEL_EVENT)` events from the ring tail under lock. The read is destructive. |
| `IOCTL_PROCSENTINEL_START_MONITOR` | Buffered / Write | None | Seeds `CurrentProcessCount` from a live snapshot, then enables recording. |
| `IOCTL_PROCSENTINEL_STOP_MONITOR` | Buffered / Write | None | Disables recording. Queued events stay in the ring. |
| `IOCTL_PROCSENTINEL_RESET_STATS` | Buffered / Write | None | Zeroes all counters, including dropped, and resets the ring indices under `g_RingLock` in O(1). |
| `IOCTL_PROCSENTINEL_GET_PROCESS_LIST` | Buffered / Read | Out | Takes a `SystemProcessInformation` snapshot (retried if the list grows), looks up each `PEPROCESS`, and fills a zeroed `WireProcInfo` per process. Capped at `min(outLen / 288, 1024)` entries. |
| `IOCTL_PROCSENTINEL_DUMP_PROCESS` | Out-Direct / Read+Write | In / Out | Input is a `WireDumpRequest` (PID, or short name when PID is 0). See below. |

**`DUMP_PROCESS` in detail**

1. The caller's token must have `SeDebugPrivilege` **enabled**. Otherwise the request fails with `STATUS_PRIVILEGE_NOT_HELD` (Win32 1314). The client enables the privilege at startup.
2. The target is found by PID, or by the first live process whose short name matches (case-insensitive). The kernel short name is only 14 characters plus NUL, so a longer requested name matches when the kernel name is a full-length prefix of it. One process reference is held for the whole operation.
3. **PP and PPL targets are refused** with `STATUS_ACCESS_DENIED` (Win32 5).
4. `SizeOfImage` comes from a zeroed, private kernel copy of the first 1 KB of the image. `e_lfanew` is checked against the bytes actually read. PE32 (WoW64) images work too, because `SizeOfImage` has the same offset in both optional-header formats.
5. The image is copied page by page into the output MDL. Pages that cannot be read are zero-filled and counted. If the process starts terminating, the request fails with `STATUS_PROCESS_IS_TERMINATING`.
6. The output buffer holds a 32-byte `WireDumpHeader` followed by `DataLength` image bytes. `Flags` reports:
   - `TRUNCATED` (0x1) — `SizeOfImage` was larger than the 4 MB cap or the buffer, so only the first `DataLength` bytes were copied.
   - `PARTIAL` (0x2) — `UnreadablePages` pages were zero-filled.

   The output buffer must be at least header + 4 KB. The client always passes header + 4 MB.

### 3. IRQL & Memory Residency Rules

| Execution Context | IRQL | Memory Constraints |
| :--- | :--- | :--- |
| `DriverEntry` / `DriverUnload` | `PASSIVE_LEVEL` | `DriverEntry` is discardable (`INIT`) |
| `ProcessNotifyCallback` | `PASSIVE_LEVEL` (critical region) | Ring buffer & globals reside in NonPaged `.bss` |
| Inside `g_RingLock` critical section | `DISPATCH_LEVEL` | Strictly NonPaged memory; zero page faults permitted |
| `DispatchDeviceControl` | `PASSIVE_LEVEL` | `SystemBuffer` (NonPaged) for buffered IOCTLs; MDL system mapping for `DUMP_PROCESS`; process snapshots use `ExAllocatePool2(POOL_FLAG_PAGED)` with tag `PsSn` |

---

## User-Mode Client (`usermode/`)

`procsentinel_app.exe` must run from an elevated prompt. At startup it enables `SeDebugPrivilege`, and prints a warning if it can't, because dumps are then refused. It then opens `\\.\ProcSentinelDrv` with `DriverClient::open()` and holds the handle in an RAII `UniqueHandle` wrapper.

```text
  main()
    │
    ├──► enableDebugPrivilege()  (AdjustTokenPrivileges, SeDebugPrivilege)
    │
    ▼
  DriverClient::open() ──► CreateFileA("\\\\.\\ProcSentinelDrv")
    │                      (ERROR_ACCESS_DENIED ⇒ not elevated)
    ▼
  Interactive Menu Loop
    │
    ├──► DriverClient methods ──► DeviceIoControl
    │          │
    │          ▼
    │    std::span over the output buffer
    │          │
    │          ▼
    │    parseEvents / parseProcessList / parseDump (types.h)
    │          ──► Domain structs (std::expected<T, std::string>)
    │
    └──► EventLog::appendBatch() ──► Appends TSV rows to procsentinel_events/events.tsv
```

### CLI Menu Reference

| Option | Operation | Underlying IOCTL |
| :---: | :--- | :--- |
| **1** | Print live telemetry counters (creates, exits, queued, dropped, dumps) | `IOCTL_PROCSENTINEL_GET_STATS` |
| **2** | Enable kernel process monitoring | `IOCTL_PROCSENTINEL_START_MONITOR` |
| **3** | Disable kernel process monitoring | `IOCTL_PROCSENTINEL_STOP_MONITOR` |
| **4** | Reset statistics and flush the kernel event ring | `IOCTL_PROCSENTINEL_RESET_STATS` |
| **5** | Drain and display pending create/exit events (up to 2048) | `IOCTL_PROCSENTINEL_GET_EVENTS` |
| **6** | Display live process snapshot (PID, PPID, protection `NO`/`PP`/`PPL`, State, Name) | `IOCTL_PROCSENTINEL_GET_PROCESS_LIST` |
| **7** | Dump a process image by PID or short name (e.g. `1234` or `notepad.exe`) to a file. Paths may contain spaces | `IOCTL_PROCSENTINEL_DUMP_PROCESS` |
| **8** | Drain pending events and append to `procsentinel_events/events.tsv` | `IOCTL_PROCSENTINEL_GET_EVENTS` |
| **0** | Close device handle and exit | — |

Options 5 and 8 both drain the ring, so events shown by option 5 are not written by a later option 8.

The dump file contains only the image bytes, not the protocol header. The client reports the PID, `ImageBase`, `SizeOfImage`, and any truncation or zero-filled pages.

---

## Build & Lab Deployment Notes

1. **Linker flags (already set in `procsentinel_driver.vcxproj`):**
   - `/INTEGRITYCHECK` (`IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY`) is required by `PsSetCreateProcessNotifyRoutineEx`. Without it, registration fails with `STATUS_ACCESS_DENIED` (`0xC0000022`) and the driver refuses to load. `sc start` then reports error 5.
   - `$(DDK_LIB_PATH)wdmsec.lib` provides `IoCreateDeviceSecure`.
   - The build uses warning level 4. Spectre mitigation is off (`Driver_SpectreMitigation=false`) so the project builds without the Spectre-mitigated libraries installed.
2. **Test Signing:** Enable test-signed drivers on the target VM prior to loading:
   ```cmd
   bcdedit /set testsigning on
   ```
3. **Service Registration (elevated CMD/PowerShell):** The install scripts do this for you. Manually:
   ```cmd
   sc.exe create ProcSentinelDrv type= kernel binPath= %SystemRoot%\System32\drivers\procsentinel_driver.sys
   sc.exe start ProcSentinelDrv
   ```
   `driver/procsentinel_driver.inf` is kept for reference. The scripts install the driver as a legacy kernel service and do not use the INF.

---

## Requirements

- Windows 10 version 2004 (build 19041) or later, x64. The driver imports `ExAllocatePool2`, so older builds cannot load it.
- Visual Studio 2019 or 2022 with the Desktop C++ workload
- Windows Driver Kit 10.0.19041 or later, matching the installed Windows SDK
- An elevated (Administrator) prompt to install or uninstall the service and to run the client
- Test signing enabled on the machine that loads the driver (`bcdedit /set testsigning on`, reboot)

The user-mode client is C++23. The build script compiles it with `/std:c++latest`.

---

## Build

From the `scripts` directory.

### User-mode client and tests

```powershell
.\Build-UserMode.ps1
```

Compiles `procsentinel_app.exe` and `procsentinel_unified_tests.exe`.

```powershell
.\Build-UserMode.ps1 -RunTests
```

Compiles and runs the unified tests.

Example:

```
=== ProcSentinel Unified Tests ===

--- Low-Level C Ring ---
[PASS] Low-Level C Ring
--- Protocol / fromWire ---
[PASS] Protocol / fromWire
--- Client parsing helpers ---
[PASS] Client parsing helpers
--- MockDriverClient ---
[PASS] MockDriverClient

====================================
ALL TESTS PASSED
```

### Driver

```powershell
.\Build-Driver.ps1
```

Builds `driver\x64\Debug\procsentinel_driver.sys` (or Release if requested). Any MSBuild error stops the script, so it never continues with an older binary. On success it also writes `driver\debug.wds`, `driver\WinDbgWorkspace.xml`, and `scripts\KDNet-ProcSentinelDrv.ps1`.

```powershell
.\Build-Driver.ps1 -Configuration Release
.\Build-Driver.ps1 -Clean
.\Build-Driver.ps1 -KdnetPort 50008 -KdnetKey <key> -KdnetHostIp <host-ip>
```

If you omit `-KdnetPort`, `-KdnetKey`, or `-KdnetHostIp`, the script prompts for them. The defaults are `50008`, the placeholder key `1.2.3.4`, and `192.168.223.1`. Only x64 is supported.

---

## Install and run

Run the install script from an elevated prompt after the driver has been built.

```powershell
.\install_procsentinel.ps1
```

Typical successful output:

```
================================================
  ProcSentinel - Driver Installation
  Service  : ProcSentinelDrv  (WDM kernel)
================================================

[OK] Found driver binary: ...\driver\x64\Debug\procsentinel_driver.sys
Stopping / deleting old service (if any)...
Copying procsentinel_driver.sys -> C:\Windows\System32\drivers\procsentinel_driver.sys ...
[SUCCESS] Driver binary copied.
[SUCCESS] PDB copied.
Creating kernel service...
[SC] CreateService SUCCESS
[SUCCESS] Service created.
Starting driver...

SERVICE_NAME: ProcSentinelDrv
        TYPE               : 1  KERNEL_DRIVER
        STATE              : 4  RUNNING
                                (STOPPABLE, NOT_PAUSABLE, IGNORES_SHUTDOWN)
        WIN32_EXIT_CODE    : 0  (0x0)
        SERVICE_EXIT_CODE  : 0  (0x0)

[SUCCESS] Driver started.
================================================
  INSTALLATION COMPLETE
  Run procsentinel_app.exe from an elevated prompt
  (the device only admits SYSTEM and Administrators).
================================================
```

Verify from PowerShell (use `sc.exe`, not `sc` — `sc` is an alias for `Set-Content`):

```powershell
sc.exe query ProcSentinelDrv
```

```
SERVICE_NAME: ProcSentinelDrv
        TYPE               : 1  KERNEL_DRIVER
        STATE              : 4  RUNNING
```

Start the client from an elevated prompt:

```powershell
..\usermode\x64\Debug\procsentinel_app.exe
```

Example session:

```
=== ProcSentinel (C++23) ===
Built: Nov 16 2026 13:40:00
Requires ProcSentinelDrv kernel driver loaded and an elevated prompt.

=== ProcSentinel User Application ===
1. Show statistics
2. Start monitoring
3. Stop monitoring
4. Reset stats / clear event ring
5. Drain and view telemetry events (create/exit)
6. Show live process list
7. Dump process image (by name or PID)
8. Drain events + append to event log
0. Exit
Choice: 2
[OK] Monitoring STARTED

Choice: 1
=== STATISTICS ===
Creates             : 12
Exits               : 3
StateChanges        : 0  (reserved)
CurrentProcessCount : 157  (seeded on START)
Events in ring      : 15
Dropped events      : 0
Dump requests       : 0
Dump failures       : 0

Choice: 5
=== TELEMETRY EVENTS (15) ===
PID         PPID      TYPE      STATE     NAME
------------------------------------------------------------
1234        567       CREATE    Running   notepad.exe
              Path: \??\C:\Windows\System32\notepad.exe
...

Choice: 6
=== LIVE PROCESS LIST (148) ===
PID         PARENT      PROTECTION STATE     NAME
-----------------------------------------------------------------
4           0           PP         Running   System
...
[SUMMARY] Protected processes: 9 / 148  (PP: 3, PPL: 6)

Choice: 7
...
Enter PID or process short name (e.g. 1234 or notepad.exe): notepad.exe
Enter output file path: C:\dumps\notepad image.bin
[SUCCESS] Dumped 360448 bytes of PID 1234 (ImageBase 0x7FF6A1B20000, SizeOfImage 360448) to C:\dumps\notepad image.bin

Choice: 0
Goodbye.
```

Event paths are NT paths as the kernel reports them (`\??\C:\...`). Process-list paths come from `SeLocateProcessImageName` (`\Device\HarddiskVolumeN\...`).

If the driver is not loaded:

```
[ERROR] Driver not loaded. Device \\.\ProcSentinelDrv not found. Run install script as Administrator first.
```

If the prompt is not elevated:

```
[WARNING] SeDebugPrivilege is not held by this token (run elevated as Administrator). Option 7 (dump) will be refused by the driver.
[ERROR] Access denied opening \\.\ProcSentinelDrv. The device only admits SYSTEM and elevated Administrators: run this client from an elevated prompt.
```

Uninstall (elevated):

```powershell
.\uninstall_procsentinel.ps1
```

```
================================================
  ProcSentinel - Uninstall
================================================

[*] Stopping service...
[*] Deleting service...
[*] No service registry key found.
[+] Removed C:\Windows\System32\drivers\procsentinel_driver.sys
[+] Removed C:\Windows\System32\drivers\procsentinel_driver.pdb

=== Verification ===
[SC] EnumQueryServicesStatus:OpenService FAILED 1060:

The specified service does not exist as an installed service.

[+] Uninstall finished. You can reinstall with install_procsentinel.ps1
```

---

## Scripts

All scripts live in `scripts/`. The PowerShell scripts and the `.bat` files resolve paths from their own location, so they can be started from any directory, including with "Run as administrator".

### Build-Driver.ps1

Builds the WDM driver with MSBuild and, after a successful build, generates WinDbg / KDNET helpers.

**When to use:** after changing `procsentinel_driver.c`, `shared_protocol.h`, or the project files, before install.

```powershell
.\Build-Driver.ps1
.\Build-Driver.ps1 -Configuration Release
.\Build-Driver.ps1 -Clean
.\Build-Driver.ps1 -KdnetPort 50008 -KdnetKey <key> -KdnetHostIp <host-ip>
```

**Produces:** `driver\x64\<Config>\procsentinel_driver.sys`, `driver\debug.wds`, `driver\WinDbgWorkspace.xml`, `scripts\KDNet-ProcSentinelDrv.ps1`. The last two contain the KDNET key.

**Example output (success):**

```
[OK] MSBuild: C:\Program Files\Microsoft Visual Studio\2022\...\MSBuild.exe
[OK] Solution found: procsentinel_driver.sln
Building...
================================================
  BUILD SUCCESSFUL
  Driver : ...\driver\x64\Debug\procsentinel_driver.sys
================================================
[OK] debug.wds written to: ...\driver\debug.wds
[OK] WinDbgWorkspace.xml written to: ...\driver\WinDbgWorkspace.xml
Using Port=50008  Key=...  HostIp=...
[OK] KDNet script written to: ...\scripts\KDNet-ProcSentinelDrv.ps1
```

### Build-UserMode.ps1

Compiles the C++23 client and the unified mock tests with `cl`. It does not build or load the driver.

**When to use:** after changing `procsentinel_app.cpp`, `types.h`, `shared_protocol.h`, or the test runner. Use `-RunTests` before packaging.

```powershell
.\Build-UserMode.ps1
.\Build-UserMode.ps1 -RunTests
.\Build-UserMode.ps1 -SkipApp
.\Build-UserMode.ps1 -SkipTests
```

**Produces:** `usermode\x64\<Config>\procsentinel_app.exe` and `mock_tests\x64\<Config>\procsentinel_unified_tests.exe`.

**Example output:**

```
App           : procsentinel_app.cpp
Mock tests    : main_test_runner.cpp
[*] Building procsentinel_app.exe ...
[+] App: ...\usermode\x64\Debug\procsentinel_app.exe
[*] Building procsentinel_unified_tests.exe ...
[+] Tests: ...\mock_tests\x64\Debug\procsentinel_unified_tests.exe
[*] Running unified tests ...
=== ProcSentinel Unified Tests ===
...
ALL TESTS PASSED
[+] All unified tests passed.
```

### install_procsentinel.ps1 / install_procsentinel.bat

Each one stops any previous `ProcSentinelDrv` service, copies `procsentinel_driver.sys` (and the PDB) into `%SystemRoot%\System32\drivers`, creates a kernel service with `sc.exe`, and starts it. If the start fails, they list the likely causes: signing, test signing, `/INTEGRITYCHECK`, or Windows older than 10 2004.

**When to use:** after a successful driver build, on a machine with test signing enabled. Both must run elevated. The `.bat` checks this and exits if not.

```powershell
.\install_procsentinel.ps1
.\install_procsentinel.ps1 -Configuration Release
```

The `.bat` is a standalone equivalent of the PowerShell script. It doesn't call it. It looks for `..\driver\x64\Debug`, then `..\driver\x64\Release`, then `..\driver`, relative to the script folder.

### uninstall_procsentinel.ps1 / uninstall_procsentinel.bat

Stops the service, deletes it and its registry key, and removes the copied `.sys` and `.pdb`.

**When to use:** before replacing the binary, or when you are done with the lab machine.

```powershell
.\uninstall_procsentinel.ps1
```

### Increment-Version.ps1

Clones the current tree into a sibling `ProcSentinel_vN` folder so a stuck or still-loaded binary does not block the next build. Optional. See the dedicated section below for what it rewrites and what it does not isolate.

**When to use:** when you want a numbered sibling workspace for the next lab iteration.

```powershell
.\Increment-Version.ps1
```

### Typical order

```
Build-UserMode.ps1 -RunTests
Build-Driver.ps1
install_procsentinel.ps1
procsentinel_app.exe        (elevated)
uninstall_procsentinel.ps1
```

---

## Versioning & Parallel Lab Builds (`scripts/Increment-Version.ps1`)

During kernel driver development, an unloaded or stuck driver binary (`.sys`) can remain locked in memory until reboot, blocking subsequent builds or service restarts. `scripts/Increment-Version.ps1` streamlines iterative testing by cloning the workspace into a versioned sibling directory (`ProcSentinel_vN`) with isolated build and deployment identifiers.

### What It Does

1. **Workspace Cloning:** Detects the current folder's `_vN` suffix (or initializes `_v1` from an unversioned root) and copies the project tree into `../ProcSentinel_v(N+1)` with `robocopy`. The `.git` and `.vs` folders are skipped, so the clone has no git history.
2. **Artifact & Script Renaming:** Updates version suffixes across build, install, and binary targets (e.g., `procsentinel_driver_v1` → `procsentinel_driver_v2`, `Build-Driver_v1.ps1` → `Build-Driver_v2.ps1`).
3. **Configuration & Altitude Rewriting:** Replaces project/version identifiers across `.c`, `.cpp`, `.h`, `.ps1`, `.bat`, `.inf`, `.md`, `.wds`, `.vcxproj`, `.sln`, and `.filters` files. It also increments the registration altitude (`+1`) in deployment scripts, which avoids altitude conflicts if filter or callback altitudes are used.

### Usage

Run from the `scripts\` directory of your current version:

```powershell
cd ProcSentinel_v1\scripts
.\Increment-Version.ps1
```

> **Note on Device Names:** While `Increment-Version.ps1` isolates binary filenames, service names, and altitude values across versions, `\Device\ProcSentinelDrv` and `\\.\ProcSentinelDrv` remain shared unless version-suffixed—so only one driver instance should be started at a time.

### What the script isolates, and what it does not

The script is a lab helper for stuck binaries. It is not a full multi-instance isolation tool. It copies the tree, then (when the current folder already has a `_vN` suffix) renames matching files and does a literal text replace of the old version tag and the altitude number.

**Handled when moving from a versioned folder (for example `_v1` → `_v2`):**

- Binary and script names that already contain the old tag are rewritten, so the new tree does not keep writing the same `.sys` name the previous service still holds.
- The altitude literal in install scripts is bumped by one. That only matters if an install script actually registers a minifilter altitude. This driver uses `PsSetCreateProcessNotifyRoutineEx`, which has no altitude.

**Not handled:**

- First run from an unversioned folder (`ProcSentinel` → `ProcSentinel_v1`): there is no old tag, so file renames of `procsentinel_driver` / `Build-Driver.ps1` are skipped. The clone keeps the same binary name.
- Device and symlink names in the driver (`\Device\ProcSentinelDrv`, `\DosDevices\ProcSentinelDrv`) do not contain a version tag. Two loaded copies try to create the same object and the second fails with `STATUS_OBJECT_NAME_COLLISION` (`0xC0000035`). Start only one instance.
- The text replace is unanchored. A tag such as `v1` is replaced wherever it appears in the scanned files. Do not rely on the script to rewrite only identifiers.
- The altitude number is replaced wherever that exact digit string appears, not only in an altitude assignment.

---

## Wire protocol

Defined in `shared/shared_protocol.h`. Packed 1-byte alignment. Strings are NUL-terminated; paths are UTF-8.

| Struct | Size | Fields |
| :--- | :---: | :--- |
| `WireEvent` | 296 | timestamp (int64), pid, parent pid (0 on exit), event type, state, short name (16), path (260) |
| `WireProcInfo` | 288 | pid (uint32), parent pid (uint32), protection, state, short name (16), path (260) |
| `WireStats` | 44 | creates, exits, state changes (reserved, 0), current process count, event count, dropped events, dump requests, dump failures |
| `WireDumpRequest` | 24 | pid (0 = use name), reserved, short name (16) |
| `WireDumpHeader` | 32 | magic `PSDM` (0x4D445350), flags, image base, pid, SizeOfImage, data length, unreadable pages |

Event types: `CREATE` (1), `EXIT` (2), `STATE` (3, reserved).

States: `Running` (0), `Exiting` (1), `Zombie` (2, reserved), `Suspended` (3, reserved).

Protection: `NONE` (0), `PP` (1, full Protected Process), `PPL` (2, Protected Process Light).

Dump flags: `TRUNCATED` (0x1), `PARTIAL` (0x2).

IOCTL codes (`FILE_DEVICE_UNKNOWN`, functions `0xB00`–`0xB06`): `GET_STATS`, `GET_EVENTS` and `GET_PROCESS_LIST` need `FILE_READ_DATA`. `START_MONITOR`, `STOP_MONITOR` and `RESET_STATS` need `FILE_WRITE_DATA`. `DUMP_PROCESS` (`METHOD_OUT_DIRECT`) needs both.

Counters: `CurrentProcessCount` is seeded from a snapshot on `START_MONITOR` and then follows creates and exits, never going below 0. It is approximate. `DumpRequests` counts every dump request that reaches the driver, and `DumpFailures` counts the ones that fail.

Limits: 2048 events in the ring, process list capped at 1024 entries per call, dump capped at 4 MB of image data (plus the 32-byte header).

---

## Testing without the driver

The mock runner does not need Windows or a loaded driver. It checks:

1. C ring helpers (push, pop, drop-oldest when full, reset including the dropped counter).
2. Wire layout (sizes), IOCTL device/function/method/access bits, and `fromWire` conversion of event, process (PP/PPL), and stats structs, including unterminated fields.
3. The client parsers in `types.h`: `parseEvents`, `parseProcessList`, `parseDump` (valid, truncated/partial, short buffer, bad magic, oversized `DataLength`), and `makeDumpRequest`.
4. A mock client that implements the same operations the real client exposes.

The real `DriverClient` I/O path and the kernel driver are not covered. Test them on a VM.

On a machine with a C++23 compiler:

```bash
g++ -std=c++23 -I shared -I usermode -o procsentinel_unified_tests mock_tests/main_test_runner.cpp
./procsentinel_unified_tests
```

Or with CMake (also builds the client on Windows):

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

Expected result: `ALL TESTS PASSED`.

---

## Failure modes

| Symptom | Likely cause |
|---------|----------------|
| `Device \\.\ProcSentinelDrv not found` | Service not installed or not started |
| `Access denied opening \\.\ProcSentinelDrv` | Client not running elevated; the device only admits SYSTEM and Administrators |
| `sc query` shows nothing in PowerShell | `sc` is `Set-Content`; use `sc.exe query ProcSentinelDrv` |
| Driver will not load | Test signing off, or binary not signed for this machine |
| `sc start` fails with error 5 (access denied) | Driver not linked with `/INTEGRITYCHECK`, so `PsSetCreateProcessNotifyRoutineEx` was refused and `DriverEntry` failed |
| `sc start` fails on an older Windows build | Windows older than 10 2004: `ExAllocatePool2` cannot be resolved |
| `DUMP_PROCESS failed: Win32 error 1314` | `SeDebugPrivilege` not enabled (client not elevated) |
| `DUMP_PROCESS failed: Win32 error 5` | Target is a PP/PPL process; dumping it is refused |
| `DUMP_PROCESS failed: Win32 error 1168` | No live process with that PID or short name |
| `DUMP_PROCESS failed: Win32 error 50` | Target has no main image mapped (minimal processes such as Registry or Memory Compression) |
| Dump reported as truncated | `SizeOfImage` > 4 MB; only the first 4 MB were copied |
| Dump reported as partial | Some image pages were not readable and were zero-filled |
| Ring count stays 0 | Monitoring was not started, or events were already drained |
| Dropped events increase | Ring is full (2048); oldest entries were discarded |
| `STATUS_OBJECT_NAME_COLLISION` on load | Another instance already created `\Device\ProcSentinelDrv` |

---

## License and scope

This project is licensed under the [MIT License](LICENSE). Copyright (c) 2026 Damian Andres Ulanowicz.

This is a lab driver for process telemetry and bounded image dumps. It is not a production security product. Load it only on a test machine with test signing enabled.

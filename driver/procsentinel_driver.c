/*
 * procsentinel_driver.c
 * Process Activity Monitoring Kernel Driver (WDM + Process Notify Callbacks)
 *
 * Purpose: Monitor process creation and termination in real time, maintain a
 *          ring-buffer event log and statistics, expose a control device
 *          (\\.\ProcSentinelDrv) so a user-mode application can:
 *            - query live process list (PID, parent, name, protected flag, state)
 *            - retrieve telemetry events (create / exit)
 *            - request a memory image dump of a process by name
 *
 * Framework: Classic WDM (Windows Driver Model)
 *
 * Architecture:
 *   - Wire layout is defined ONCE in shared/shared_protocol.h
 *   - This driver #includes that header (no local redefinition of wire types)
 *   - User-mode C++23 app + unified mock tests consume the same header
 *
 * WHY WDM?
 *   - Matches InspectorDriver_v3 / NetworkMonitor style.
 *   - PsSetCreateProcessNotifyRoutineEx needs no KMDF scaffolding.
 *   - Full control over IRP handling and NonPagedPool.
 *
 * IMPORTANT:
 *   - Kernel code runs with highest privilege; bugs can BSOD.
 *   - Never call user-mode APIs. Memory at DISPATCH_LEVEL must be NonPaged.
 *   - Process-notify runs at PASSIVE_LEVEL (kernel APCs disabled).
 *   - All IOCTLs use METHOD_BUFFERED.
 *
 * References:
 *   - PsSetCreateProcessNotifyRoutineEx
 *   - ZwQuerySystemInformation / MmCopyVirtualMemory
 *   - Defining I/O Control Codes
 */

#include <ntifs.h>
#include <ntddk.h>
#include <ntimage.h>   // IMAGE_DOS_HEADER, IMAGE_NT_HEADERS64

// Suppress noisy warnings for educational clarity
#pragma warning(disable: 4201) // nameless struct/union
#pragma warning(disable: 4214) // bit field types other than int

// ===================== DEVICE / IOCTL DEFINITIONS =====================

#define DEVICE_NAME     L"\\Device\\ProcSentinelDrv"
#define SYMLINK_NAME    L"\\DosDevices\\ProcSentinelDrv"

#define IOCTL_PROCSENTINEL_GET_STATS         CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB00, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PROCSENTINEL_GET_EVENTS        CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB01, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PROCSENTINEL_START_MONITOR     CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB02, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PROCSENTINEL_STOP_MONITOR      CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB03, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PROCSENTINEL_RESET_STATS       CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB04, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PROCSENTINEL_GET_PROCESS_LIST  CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB05, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PROCSENTINEL_DUMP_PROCESS      CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB06, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ===================== SHARED PROTOCOL (single source of truth) =====================
// Path: ../shared/shared_protocol.h
//   WireEvent / WireStats / WireProcInfo
//   aliases: PROCSENTINEL_EVENT, PROCSENTINEL_STATS, PROCSENTINEL_PROC_INFO
//   constants: PROCSENTINEL_MAX_*, PROCSENTINEL_EVENT_*, PROCSENTINEL_STATE_*
// Same header is included by usermode/types.h and mock_tests.
#include "../shared/shared_protocol.h"

/* Kernel timestamps are LARGE_INTEGER; wire field is int64_t (same size). */
C_ASSERT(sizeof(((WireEvent*)0)->Timestamp) == sizeof(LARGE_INTEGER));
C_ASSERT(sizeof(PROCSENTINEL_EVENT) == sizeof(WireEvent));
C_ASSERT(sizeof(PROCSENTINEL_STATS) == sizeof(WireStats));
C_ASSERT(sizeof(PROCSENTINEL_PROC_INFO) == sizeof(WireProcInfo));

// ===================== REQUIRED SYSTEM DEFINITIONS =====================

#define SystemProcessInformation 5

typedef struct _SYSTEM_PROCESS_INFORMATION {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
} SYSTEM_PROCESS_INFORMATION, *PSYSTEM_PROCESS_INFORMATION;

// Undocumented but widely used
NTKERNELAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_      ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_      ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
);

NTKERNELAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
);

NTKERNELAPI
BOOLEAN
NTAPI
PsIsProtectedProcess(
    _In_ PEPROCESS Process
);

NTKERNELAPI
BOOLEAN
NTAPI
PsIsProtectedProcessLight(
    _In_ PEPROCESS Process
);

NTKERNELAPI
PCHAR
NTAPI
PsGetProcessImageFileName(
    _In_ PEPROCESS Process
);

NTKERNELAPI
PVOID
NTAPI
PsGetProcessSectionBaseAddress(
    _In_ PEPROCESS Process
);

// MmCopyVirtualMemory is exported on modern Windows
NTKERNELAPI
NTSTATUS
NTAPI
MmCopyVirtualMemory(
    _In_ PEPROCESS SourceProcess,
    _In_ PVOID SourceAddress,
    _In_ PEPROCESS TargetProcess,
    _Out_ PVOID TargetAddress,
    _In_ SIZE_T BufferSize,
    _In_ KPROCESSOR_MODE PreviousMode,
    _Out_ PSIZE_T ReturnSize
);

// ===================== RING BUFFER TYPE (must appear before globals) =====================

typedef struct _PROCSENTINEL_EVENT_RING {
    PROCSENTINEL_EVENT entries[PROCSENTINEL_MAX_EVENTS];
    volatile ULONG head;
    volatile ULONG tail;
    volatile ULONG count;
    volatile ULONG dropped;
} PROCSENTINEL_EVENT_RING, *PPROCSENTINEL_EVENT_RING;

// ===================== GLOBAL STATE =====================

static PDEVICE_OBJECT   g_DeviceObject = NULL;
static BOOLEAN          g_MonitoringEnabled = FALSE;
static KSPIN_LOCK       g_RingLock;
static PROCSENTINEL_EVENT_RING g_EventRing;
static PROCSENTINEL_STATS    g_Stats;
static BOOLEAN          g_NotifyRegistered = FALSE;

// ===================== RING HELPERS (kernel version with spinlock) =====================

static VOID EventRingInitKernel(PPROCSENTINEL_EVENT_RING r)
{
    RtlZeroMemory(r, sizeof(*r));
}

static VOID EventRingPushKernel(PPROCSENTINEL_EVENT_RING r, const PROCSENTINEL_EVENT* ev)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_RingLock, &oldIrql);
    if (r->count >= PROCSENTINEL_MAX_EVENTS) {
        r->tail = (r->tail + 1) % PROCSENTINEL_MAX_EVENTS;
        r->count--;
        r->dropped++;
        g_Stats.DroppedEvents = r->dropped;
    }
    r->entries[r->head] = *ev;
    r->head = (r->head + 1) % PROCSENTINEL_MAX_EVENTS;
    r->count++;
    g_Stats.EventCount = r->count;
    KeReleaseSpinLock(&g_RingLock, oldIrql);
}

// ===================== PROCESS NOTIFY CALLBACK =====================
/*
 * This function is called by the kernel every time a process is created or
 * is about to exit. Keep it SHORT. Do not allocate large memory, do not
 * wait, do not call complex APIs that can page-fault at elevated IRQL.
 *
 * CreateInfo is non-NULL on create, NULL on exit.
 * ParentId / ProcessId are handles (use them as ULONG after cast).
 */

static VOID
ProcessNotifyCallback(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
)
{
    UNREFERENCED_PARAMETER(Process);

    if (!g_MonitoringEnabled)
        return;

    PROCSENTINEL_EVENT ev;
    LARGE_INTEGER ts;
    RtlZeroMemory(&ev, sizeof(ev));
    KeQuerySystemTime(&ts);
    ev.Timestamp = ts.QuadPart;   /* wire field is int64_t (shared_protocol.h) */
    ev.ProcessId = HandleToULong(ProcessId);

    if (CreateInfo != NULL) {
        // ----- CREATE -----
        ev.EventType = PROCSENTINEL_EVENT_CREATE;
        ev.State = PROCSENTINEL_STATE_RUNNING;
        ev.ParentProcessId = HandleToULong(CreateInfo->ParentProcessId);

        // Short name from EPROCESS (always present, max 15 chars + null)
        // PsGetProcessImageFileName returns PCHAR
        PCHAR shortName = PsGetProcessImageFileName(Process);
        if (shortName) {
            RtlCopyMemory(ev.ImageName, shortName, PROCSENTINEL_MAX_NAME - 1);
            ev.ImageName[PROCSENTINEL_MAX_NAME - 1] = 0;
        }

        // Full path if available (FileOpenNameAvailable)
        if (CreateInfo->FileOpenNameAvailable && CreateInfo->ImageFileName) {
            // Convert UNICODE_STRING to ANSI carefully (truncate)
            ANSI_STRING ansi = { 0 };
            if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&ansi, CreateInfo->ImageFileName, TRUE))) {
                ULONG copyLen = min(ansi.Length, PROCSENTINEL_MAX_PATH - 1);
                RtlCopyMemory(ev.ImagePath, ansi.Buffer, copyLen);
                ev.ImagePath[copyLen] = 0;
                RtlFreeAnsiString(&ansi);
            }
        }

        InterlockedIncrement64((PLONG64)&g_Stats.Creates);
        InterlockedIncrement((PLONG)&g_Stats.CurrentProcessCount);
    }
    else {
        // ----- EXIT -----
        ev.EventType = PROCSENTINEL_EVENT_EXIT;
        ev.State = PROCSENTINEL_STATE_EXITING;
        // Parent unknown on exit; leave 0
        PCHAR shortName = PsGetProcessImageFileName(Process);
        if (shortName) {
            RtlCopyMemory(ev.ImageName, shortName, PROCSENTINEL_MAX_NAME - 1);
            ev.ImageName[PROCSENTINEL_MAX_NAME - 1] = 0;
        }
        InterlockedIncrement64((PLONG64)&g_Stats.Exits);
        InterlockedDecrement((PLONG)&g_Stats.CurrentProcessCount);
    }

    EventRingPushKernel(&g_EventRing, &ev);
}

// ===================== PROCESS LIST (snapshot via ZwQuerySystemInformation) =====================

static ULONG EnumerateProcesses(PPROCSENTINEL_PROC_INFO out, ULONG maxCount)
{
    ULONG count = 0;
    ULONG bufferSize = 0;
    NTSTATUS status;

    status = ZwQuerySystemInformation(SystemProcessInformation, NULL, 0, &bufferSize);
    if (status != STATUS_INFO_LENGTH_MISMATCH || bufferSize == 0)
        return 0;

    PVOID buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, bufferSize, 'nPrP');
    if (!buffer)
        return 0;

    status = ZwQuerySystemInformation(SystemProcessInformation, buffer, bufferSize, NULL);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(buffer, 'nPrP');
        return 0;
    }

    PSYSTEM_PROCESS_INFORMATION current = (PSYSTEM_PROCESS_INFORMATION)buffer;

    do {
        if (current->UniqueProcessId != NULL && count < maxCount) {
            PEPROCESS proc = NULL;
            if (NT_SUCCESS(PsLookupProcessByProcessId(current->UniqueProcessId, &proc))) {
                out[count].Pid = (ULONG_PTR)current->UniqueProcessId;
                out[count].ParentPid = (ULONG_PTR)current->InheritedFromUniqueProcessId;
                out[count].Protected = (BOOLEAN)(PsIsProtectedProcess(proc) || PsIsProtectedProcessLight(proc));
                out[count].State = PROCSENTINEL_STATE_RUNNING; // snapshot = live processes
                out[count].Reserved = 0;

                // Short name
                PCHAR shortName = PsGetProcessImageFileName(proc);
                if (shortName) {
                    RtlCopyMemory(out[count].ImageName, shortName, PROCSENTINEL_MAX_NAME - 1);
                    out[count].ImageName[PROCSENTINEL_MAX_NAME - 1] = 0;
                }
                else {
                    out[count].ImageName[0] = 0;
                }

                // Full path from UNICODE_STRING if present
                out[count].ImagePath[0] = 0;
                if (current->ImageName.Buffer && current->ImageName.Length > 0) {
                    ANSI_STRING ansi = { 0 };
                    if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&ansi, &current->ImageName, TRUE))) {
                        ULONG copyLen = min(ansi.Length, PROCSENTINEL_MAX_PATH - 1);
                        RtlCopyMemory(out[count].ImagePath, ansi.Buffer, copyLen);
                        out[count].ImagePath[copyLen] = 0;
                        RtlFreeAnsiString(&ansi);
                    }
                }

                ObDereferenceObject(proc);
                count++;
            }
        }

        if (current->NextEntryOffset == 0)
            break;
        current = (PSYSTEM_PROCESS_INFORMATION)((PUCHAR)current + current->NextEntryOffset);
    } while (TRUE);

    ExFreePoolWithTag(buffer, 'nPrP');
    return count;
}

// ===================== PROCESS MEMORY DUMP (by name) =====================
/*
 *   1. User-mode sends the process short name (e.g. "notepad.exe") in the
 *      input buffer of IOCTL_PROCSENTINEL_DUMP_PROCESS.
 *   2. We walk the live process list looking for a matching ImageName.
 *   3. Once found we obtain the PEPROCESS, read its SectionBaseAddress
 *      (ImageBase) and SizeOfImage from the PE header (or use a conservative
 *      size limit).
 *   4. We use MmCopyVirtualMemory to copy from the target process address
 *      space into our SystemBuffer (which is already mapped into the current
 *      process context by the I/O manager).
 *   5. Return the number of bytes copied.
 *
 * Safety:
 *   - Cap at PROCSENTINEL_DUMP_MAX_SIZE (4 MB).
 *   - Fail cleanly if process exits under us (STATUS_PROCESS_IS_TERMINATING).
 *   - Never probe user buffers ourselves; METHOD_BUFFERED already did it.
 */

static NTSTATUS DumpProcessByName(PCHAR name, PVOID outBuffer, ULONG outSize, PULONG_PTR bytesCopied)
{
    *bytesCopied = 0;
    if (!name || !outBuffer || outSize < 0x1000)
        return STATUS_BUFFER_TOO_SMALL;

    // First locate the PID by short name
    ULONG listSize = 64 * 1024;
    PVOID listBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED, listSize, 'dPrP');
    if (!listBuf)
        return STATUS_INSUFFICIENT_RESOURCES;

    ULONG returned = 0;
    NTSTATUS status = ZwQuerySystemInformation(SystemProcessInformation, listBuf, listSize, &returned);
    if (status == STATUS_INFO_LENGTH_MISMATCH) {
        ExFreePoolWithTag(listBuf, 'dPrP');
        listSize = returned + 0x1000;
        listBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED, listSize, 'dPrP');
        if (!listBuf)
            return STATUS_INSUFFICIENT_RESOURCES;
        status = ZwQuerySystemInformation(SystemProcessInformation, listBuf, listSize, NULL);
    }
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(listBuf, 'dPrP');
        return status;
    }

    HANDLE targetPid = NULL;
    PSYSTEM_PROCESS_INFORMATION cur = (PSYSTEM_PROCESS_INFORMATION)listBuf;
    do {
        if (cur->UniqueProcessId) {
            PEPROCESS tmp = NULL;
            if (NT_SUCCESS(PsLookupProcessByProcessId(cur->UniqueProcessId, &tmp))) {
                PCHAR img = PsGetProcessImageFileName(tmp);
                if (img && _stricmp(img, name) == 0) {
                    targetPid = cur->UniqueProcessId;
                    ObDereferenceObject(tmp);
                    break;
                }
                ObDereferenceObject(tmp);
            }
        }
        if (cur->NextEntryOffset == 0)
            break;
        cur = (PSYSTEM_PROCESS_INFORMATION)((PUCHAR)cur + cur->NextEntryOffset);
    } while (TRUE);

    ExFreePoolWithTag(listBuf, 'dPrP');

    if (!targetPid)
        return STATUS_NOT_FOUND;

    PEPROCESS targetProcess = NULL;
    status = PsLookupProcessByProcessId(targetPid, &targetProcess);
    if (!NT_SUCCESS(status))
        return status;

    // Get ImageBase. On modern Windows we can use PsGetProcessSectionBaseAddress
    PVOID imageBase = PsGetProcessSectionBaseAddress(targetProcess);
    if (!imageBase) {
        ObDereferenceObject(targetProcess);
        return STATUS_UNSUCCESSFUL;
    }

    // Read SizeOfImage from PE optional header (safe small read)
    // DOS header is at imageBase, e_lfanew points to NT headers.
    // We only need the first few bytes; use MmCopyVirtualMemory for safety.
    UCHAR peHdr[0x200];
    SIZE_T copied = 0;
    status = MmCopyVirtualMemory(
        targetProcess, imageBase,
        PsGetCurrentProcess(), peHdr,
        sizeof(peHdr), KernelMode, &copied);

    if (!NT_SUCCESS(status) || copied < 0x40) {
        ObDereferenceObject(targetProcess);
        return STATUS_ACCESS_DENIED; // or the real status
    }

    // Parse SizeOfImage
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)peHdr;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        ObDereferenceObject(targetProcess);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)(peHdr + dos->e_lfanew);
    // Be careful: e_lfanew could be outside our small buffer; clamp
    if ((ULONG)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > sizeof(peHdr)) {
        ObDereferenceObject(targetProcess);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        ObDereferenceObject(targetProcess);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    ULONG imageSize = nt->OptionalHeader.SizeOfImage;
    if (imageSize == 0 || imageSize > PROCSENTINEL_DUMP_MAX_SIZE)
        imageSize = min(outSize, PROCSENTINEL_DUMP_MAX_SIZE);

    if (outSize < imageSize) {
        ObDereferenceObject(targetProcess);
        return STATUS_BUFFER_TOO_SMALL;
    }

    // Now copy the whole image
    copied = 0;
    status = MmCopyVirtualMemory(
        targetProcess, imageBase,
        PsGetCurrentProcess(), outBuffer,
        imageSize, KernelMode, &copied);

    ObDereferenceObject(targetProcess);

    if (!NT_SUCCESS(status)) {
        InterlockedIncrement((PLONG)&g_Stats.DumpFailures);
        return status;
    }

    *bytesCopied = copied;
    InterlockedIncrement((PLONG)&g_Stats.DumpRequests);
    return STATUS_SUCCESS;
}

// ===================== DISPATCH ROUTINES =====================

NTSTATUS DispatchCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS DispatchClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS DispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    switch (code) {
    case IOCTL_PROCSENTINEL_GET_STATS:
        if (outLen < sizeof(PROCSENTINEL_STATS)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        // Snapshot under lock for consistency
        {
            KIRQL old;
            KeAcquireSpinLock(&g_RingLock, &old);
            RtlCopyMemory(buffer, &g_Stats, sizeof(PROCSENTINEL_STATS));
            ((PPROCSENTINEL_STATS)buffer)->EventCount = g_EventRing.count;
            ((PPROCSENTINEL_STATS)buffer)->DroppedEvents = g_EventRing.dropped;
            KeReleaseSpinLock(&g_RingLock, old);
        }
        info = sizeof(PROCSENTINEL_STATS);
        status = STATUS_SUCCESS;
        break;

    case IOCTL_PROCSENTINEL_GET_EVENTS:
        if (outLen < sizeof(PROCSENTINEL_EVENT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            ULONG maxEvents = outLen / sizeof(PROCSENTINEL_EVENT);
            PPROCSENTINEL_EVENT out = (PPROCSENTINEL_EVENT)buffer;
            ULONG copied = 0;
            KIRQL old;
            KeAcquireSpinLock(&g_RingLock, &old);
            while (copied < maxEvents && g_EventRing.count > 0) {
                out[copied] = g_EventRing.entries[g_EventRing.tail];
                g_EventRing.tail = (g_EventRing.tail + 1) % PROCSENTINEL_MAX_EVENTS;
                g_EventRing.count--;
                copied++;
            }
            g_Stats.EventCount = g_EventRing.count;
            KeReleaseSpinLock(&g_RingLock, old);
            info = copied * sizeof(PROCSENTINEL_EVENT);
            status = STATUS_SUCCESS;
        }
        break;

    case IOCTL_PROCSENTINEL_START_MONITOR:
        g_MonitoringEnabled = TRUE;
        status = STATUS_SUCCESS;
        break;

    case IOCTL_PROCSENTINEL_STOP_MONITOR:
        g_MonitoringEnabled = FALSE;
        status = STATUS_SUCCESS;
        break;

    case IOCTL_PROCSENTINEL_RESET_STATS:
        {
            KIRQL old;
            KeAcquireSpinLock(&g_RingLock, &old);
            RtlZeroMemory(&g_Stats, sizeof(g_Stats));
            EventRingInitKernel(&g_EventRing);
            KeReleaseSpinLock(&g_RingLock, old);
        }
        status = STATUS_SUCCESS;
        break;

    case IOCTL_PROCSENTINEL_GET_PROCESS_LIST:
        if (outLen < sizeof(PROCSENTINEL_PROC_INFO)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            ULONG maxCount = outLen / sizeof(PROCSENTINEL_PROC_INFO);
            ULONG count = EnumerateProcesses((PPROCSENTINEL_PROC_INFO)buffer, maxCount);
            info = count * sizeof(PROCSENTINEL_PROC_INFO);
            status = STATUS_SUCCESS;
        }
        break;

    case IOCTL_PROCSENTINEL_DUMP_PROCESS:
        if (inLen == 0 || !buffer) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        // Input is the null-terminated process name (ANSI)
        // Output buffer is the memory image
        {
            CHAR name[PROCSENTINEL_MAX_NAME] = { 0 };
            ULONG copy = min(inLen, PROCSENTINEL_MAX_NAME - 1);
            RtlCopyMemory(name, buffer, copy);
            name[copy] = 0;

            // Note: METHOD_BUFFERED uses the same SystemBuffer for in/out.
            // We must not overwrite the name before we finish using it,
            // so we already copied it into a local.
            status = DumpProcessByName(name, buffer, outLen, &info);
        }
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

// ===================== DRIVER ENTRY / UNLOAD =====================

DRIVER_UNLOAD DriverUnload;

VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
    if (g_NotifyRegistered) {
        PsSetCreateProcessNotifyRoutineEx(ProcessNotifyCallback, TRUE); // remove
        g_NotifyRegistered = FALSE;
    }

    UNICODE_STRING symLink;
    RtlInitUnicodeString(&symLink, SYMLINK_NAME);
    IoDeleteSymbolicLink(&symLink);

    if (DriverObject->DeviceObject)
        IoDeleteDevice(DriverObject->DeviceObject);

    g_DeviceObject = NULL;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    NTSTATUS status;
    UNICODE_STRING devName, symLink;

    RtlInitUnicodeString(&devName, DEVICE_NAME);
    RtlInitUnicodeString(&symLink, SYMLINK_NAME);

    KeInitializeSpinLock(&g_RingLock);
    EventRingInitKernel(&g_EventRing);
    RtlZeroMemory(&g_Stats, sizeof(g_Stats));
    g_MonitoringEnabled = FALSE;

    status = IoCreateDevice(
        DriverObject,
        0,
        &devName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &g_DeviceObject
    );
    if (!NT_SUCCESS(status))
        return status;

    status = IoCreateSymbolicLink(&symLink, &devName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
        return status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = DispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DispatchClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;
    DriverObject->DriverUnload = DriverUnload;

    // Register the process notify routine (Ex version)
    status = PsSetCreateProcessNotifyRoutineEx(ProcessNotifyCallback, FALSE);
    if (!NT_SUCCESS(status)) {
        // Still allow the driver to load; monitoring just won't work
        // (user can see the error via Event Viewer)
        DbgPrint("ProcSentinel: PsSetCreateProcessNotifyRoutineEx failed 0x%08X\n", status);
    }
    else {
        g_NotifyRegistered = TRUE;
    }

    DbgPrint("ProcSentinel: Driver loaded successfully. Device \\Device\\ProcSentinelDrv ready.\n");
    return STATUS_SUCCESS;
}

/*
 * procsentinel_driver.c
 * Process Activity Monitoring Kernel Driver (WDM + Process Notify Callbacks)
 *
 * Purpose: Monitor process creation and termination in real time, maintain a
 *          ring-buffer event log and statistics, expose a control device
 *          (\\.\ProcSentinelDrv) so a user-mode application can:
 *            - query live process list (PID, parent, name, PP/PPL level, state)
 *            - retrieve telemetry events (create / exit)
 *            - request a memory image dump of a non-protected process
 *
 * Framework: Classic WDM (Windows Driver Model)
 * Minimum OS: Windows 10 version 2004 (ExAllocatePool2)
 *
 * Architecture:
 *   - Wire layout and IOCTL codes are defined ONCE in shared/shared_protocol.h
 *   - This driver #includes that header (no local redefinition of wire types)
 *   - User-mode C++23 app + unified mock tests consume the same header
 *
 * WHY WDM?
 *   - PsSetCreateProcessNotifyRoutineEx needs no KMDF scaffolding.
 *   - Full control over IRP handling and pool usage.
 *
 * SECURITY MODEL:
 *   - The device is created with IoCreateDeviceSecure and
 *     SDDL_DEVOBJ_SYS_ALL_ADM_ALL: only SYSTEM and elevated Administrators
 *     can open \\.\ProcSentinelDrv.
 *   - IOCTL access bits: queries need FILE_READ_DATA, state changes need
 *     FILE_WRITE_DATA, DUMP needs both.
 *   - DUMP additionally requires the caller to hold SeDebugPrivilege
 *     (enabled), and refuses Protected (PP) and Protected Light (PPL)
 *     processes.
 *
 * IMPORTANT:
 *   - Link with /INTEGRITYCHECK or PsSetCreateProcessNotifyRoutineEx fails
 *     with STATUS_ACCESS_DENIED (DriverEntry then fails the load).
 *   - Process-notify runs at PASSIVE_LEVEL (in a critical region).
 *   - All IOCTLs use METHOD_BUFFERED except DUMP (METHOD_OUT_DIRECT).
 *
 * References:
 *   - PsSetCreateProcessNotifyRoutineEx, IoCreateDeviceSecure
 *   - ZwQuerySystemInformation / MmCopyVirtualMemory
 *   - Defining I/O Control Codes
 */

#include <ntifs.h>
#include <ntddk.h>
#include <wdmsec.h>    // IoCreateDeviceSecure, SDDL_DEVOBJ_SYS_ALL_ADM_ALL (link wdmsec.lib)
#include <ntimage.h>   // IMAGE_DOS_HEADER, IMAGE_NT_HEADERS64

// Suppress noisy warnings for educational clarity
#pragma warning(disable: 4201) // nameless struct/union
#pragma warning(disable: 4214) // bit field types other than int

// ===================== DEVICE DEFINITIONS =====================

#define DEVICE_NAME     L"\\Device\\ProcSentinelDrv"
#define SYMLINK_NAME    L"\\DosDevices\\ProcSentinelDrv"

// Pool tag shows as "PsSn" in !poolused / PoolMon
#define PROCSENTINEL_POOL_TAG       'nSsP'

// Upper bound for a SystemProcessInformation snapshot buffer
#define PROCSENTINEL_SNAPSHOT_MAX   (64u * 1024u * 1024u)

// Device class GUID used by IoCreateDeviceSecure (lets an admin override the
// SDDL via HKLM\SYSTEM\CurrentControlSet\Control\Class\{GUID}\Properties).
// {864D2BF5-F733-460E-9F71-7FE8F7201AB5}
static const GUID GUID_DEVCLASS_PROCSENTINEL =
    { 0x864d2bf5, 0xf733, 0x460e, { 0x9f, 0x71, 0x7f, 0xe8, 0xf7, 0x20, 0x1a, 0xb5 } };

// ===================== SHARED PROTOCOL (single source of truth) =====================
// Path: ../shared/shared_protocol.h
//   WireEvent / WireStats / WireProcInfo / WireDumpRequest / WireDumpHeader
//   IOCTL_PROCSENTINEL_* codes, PROCSENTINEL_* constants
#include "../shared/shared_protocol.h"

/* Kernel timestamps are LARGE_INTEGER; wire field is int64_t (same size). */
C_ASSERT(sizeof(((WireEvent*)0)->Timestamp) == sizeof(LARGE_INTEGER));

/* The portable IOCTL formula in the shared header must match CTL_CODE. */
C_ASSERT(IOCTL_PROCSENTINEL_GET_STATS        == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB00, METHOD_BUFFERED,   FILE_READ_DATA));
C_ASSERT(IOCTL_PROCSENTINEL_GET_EVENTS       == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB01, METHOD_BUFFERED,   FILE_READ_DATA));
C_ASSERT(IOCTL_PROCSENTINEL_START_MONITOR    == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB02, METHOD_BUFFERED,   FILE_WRITE_DATA));
C_ASSERT(IOCTL_PROCSENTINEL_STOP_MONITOR     == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB03, METHOD_BUFFERED,   FILE_WRITE_DATA));
C_ASSERT(IOCTL_PROCSENTINEL_RESET_STATS      == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB04, METHOD_BUFFERED,   FILE_WRITE_DATA));
C_ASSERT(IOCTL_PROCSENTINEL_GET_PROCESS_LIST == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB05, METHOD_BUFFERED,   FILE_READ_DATA));
C_ASSERT(IOCTL_PROCSENTINEL_DUMP_PROCESS     == CTL_CODE(FILE_DEVICE_UNKNOWN, 0xB06, METHOD_OUT_DIRECT, FILE_READ_DATA | FILE_WRITE_DATA));
C_ASSERT(PROCSENTINEL_DUMP_PAGE_SIZE == PAGE_SIZE);

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

// ===================== FUNCTION ROLE TYPES =====================

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;

_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLOSE)
DRIVER_DISPATCH DispatchCreateClose;

_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH DispatchDeviceControl;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#endif

// ===================== RING BUFFER TYPE (must appear before globals) =====================

typedef struct _PROCSENTINEL_EVENT_RING {
    PROCSENTINEL_EVENT entries[PROCSENTINEL_MAX_EVENTS];
    volatile ULONG head;
    volatile ULONG tail;
    volatile ULONG count;
    volatile ULONG dropped;
} PROCSENTINEL_EVENT_RING, *PPROCSENTINEL_EVENT_RING;

// ===================== GLOBAL STATE =====================

static PDEVICE_OBJECT            g_DeviceObject = NULL;
static volatile BOOLEAN          g_MonitoringEnabled = FALSE;
static KSPIN_LOCK                g_RingLock;
static PROCSENTINEL_EVENT_RING   g_EventRing;
// WireStats is pack(1); force 8-byte alignment so Interlocked* on its fields
// are naturally aligned (field offsets are asserted in shared_protocol.h).
static DECLSPEC_ALIGN(8) PROCSENTINEL_STATS g_Stats;
static BOOLEAN                   g_NotifyRegistered = FALSE;

// ===================== SMALL HELPERS =====================

// Copy the EPROCESS short name (UCHAR[15]) into a zeroed 16-byte wire field.
static VOID CopyShortName(_In_ PEPROCESS Process, _Out_writes_bytes_(PROCSENTINEL_MAX_NAME) char* Dest)
{
    PCHAR shortName = PsGetProcessImageFileName(Process);
    RtlZeroMemory(Dest, PROCSENTINEL_MAX_NAME);
    if (shortName) {
        RtlCopyMemory(Dest, shortName, PROCSENTINEL_MAX_NAME - 1);
        Dest[PROCSENTINEL_MAX_NAME - 1] = 0;
    }
}

// Convert a UNICODE_STRING to a NUL-terminated, truncated UTF-8 wire field.
// RtlUnicodeToUTF8N never splits a character when it runs out of room.
_IRQL_requires_(PASSIVE_LEVEL)
static VOID CopyUnicodeToUtf8(_In_opt_ PCUNICODE_STRING Source, _Out_writes_bytes_(DestSize) char* Dest, _In_ ULONG DestSize)
{
    ULONG written = 0;
    NTSTATUS status;

    if (DestSize == 0)
        return;
    RtlZeroMemory(Dest, DestSize);
    if (!Source || !Source->Buffer || Source->Length == 0 || DestSize < 2)
        return;

    status = RtlUnicodeToUTF8N(Dest, DestSize - 1, &written,
                               Source->Buffer, (ULONG)(Source->Length & 0xFFFEu));
    if (!NT_SUCCESS(status) && status != STATUS_BUFFER_TOO_SMALL) {
        Dest[0] = 0;
        return;
    }
    if (written > DestSize - 1)
        written = DestSize - 1;
    Dest[written] = 0;
}

// PROCSENTINEL_PROTECTION_*: PsIsProtectedProcess is TRUE for PP *and* PPL,
// PsIsProtectedProcessLight only for PPL, so test Light first.
static UINT8 GetProtectionLevel(_In_ PEPROCESS Process)
{
    if (PsIsProtectedProcessLight(Process))
        return PROCSENTINEL_PROTECTION_PPL;
    if (PsIsProtectedProcess(Process))
        return PROCSENTINEL_PROTECTION_PP;
    return PROCSENTINEL_PROTECTION_NONE;
}

// Atomically decrement, never going below zero.
static VOID InterlockedDecrementClampZero(_Inout_ volatile LONG* Value)
{
    for (;;) {
        LONG current = *Value;
        if (current <= 0)
            return;
        if (InterlockedCompareExchange(Value, current - 1, current) == current)
            return;
    }
}

// ===================== RING HELPERS (kernel version with spinlock) =====================

static VOID EventRingInitKernel(_Out_ PPROCSENTINEL_EVENT_RING r)
{
    RtlZeroMemory(r, sizeof(*r));
}

// Same semantics as EventRingReset() in shared_protocol.h: indices, count and
// dropped go to 0; entries are not wiped. Caller holds g_RingLock.
static VOID EventRingResetKernel(_Inout_ PPROCSENTINEL_EVENT_RING r)
{
    r->head = 0;
    r->tail = 0;
    r->count = 0;
    r->dropped = 0;
}

static VOID EventRingPushKernel(_Inout_ PPROCSENTINEL_EVENT_RING r, _In_ const PROCSENTINEL_EVENT* ev)
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
 * Called by the kernel every time a process is created or is about to exit,
 * at PASSIVE_LEVEL inside a critical region. Keep it short.
 *
 * CreateInfo is non-NULL on create, NULL on exit.
 */

static VOID
ProcessNotifyCallback(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
)
{
    PROCSENTINEL_EVENT ev;
    LARGE_INTEGER ts;

    if (!ReadBooleanNoFence(&g_MonitoringEnabled))
        return;

    RtlZeroMemory(&ev, sizeof(ev));
    KeQuerySystemTime(&ts);
    ev.Timestamp = ts.QuadPart;   /* wire field is int64_t (shared_protocol.h) */
    ev.ProcessId = HandleToULong(ProcessId);
    CopyShortName(Process, ev.ImageName);

    if (CreateInfo != NULL) {
        // ----- CREATE -----
        ev.EventType = PROCSENTINEL_EVENT_CREATE;
        ev.State = PROCSENTINEL_STATE_RUNNING;
        ev.ParentProcessId = HandleToULong(CreateInfo->ParentProcessId);

        if (CreateInfo->FileOpenNameAvailable && CreateInfo->ImageFileName)
            CopyUnicodeToUtf8(CreateInfo->ImageFileName, ev.ImagePath, sizeof(ev.ImagePath));

        InterlockedIncrement64((volatile LONG64*)&g_Stats.Creates);
        InterlockedIncrement((volatile LONG*)&g_Stats.CurrentProcessCount);
    }
    else {
        // ----- EXIT ----- (parent unknown on exit; left 0)
        ev.EventType = PROCSENTINEL_EVENT_EXIT;
        ev.State = PROCSENTINEL_STATE_EXITING;
        InterlockedIncrement64((volatile LONG64*)&g_Stats.Exits);
        // Processes that started before monitoring was enabled also exit here,
        // so never let the counter wrap below 0.
        InterlockedDecrementClampZero((volatile LONG*)&g_Stats.CurrentProcessCount);
    }

    EventRingPushKernel(&g_EventRing, &ev);
}

// ===================== PROCESS SNAPSHOT (ZwQuerySystemInformation) =====================

// Allocate and fill a SystemProcessInformation snapshot. Retries while the
// process list grows between the size probe and the real query.
// Caller frees *Buffer with ExFreePoolWithTag(..., PROCSENTINEL_POOL_TAG).
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS QueryProcessSnapshot(_Out_ PVOID* Buffer, _Out_ PULONG BufferSize)
{
    ULONG size = 256u * 1024u;
    ULONG attempt;

    *Buffer = NULL;
    *BufferSize = 0;

    for (attempt = 0; attempt < 8; ++attempt) {
        ULONG needed = 0;
        NTSTATUS status;
        PVOID buf = ExAllocatePool2(POOL_FLAG_PAGED, size, PROCSENTINEL_POOL_TAG);
        if (!buf)
            return STATUS_INSUFFICIENT_RESOURCES;

        status = ZwQuerySystemInformation(SystemProcessInformation, buf, size, &needed);
        if (NT_SUCCESS(status)) {
            *Buffer = buf;
            *BufferSize = size;
            return STATUS_SUCCESS;
        }

        ExFreePoolWithTag(buf, PROCSENTINEL_POOL_TAG);
        if (status != STATUS_INFO_LENGTH_MISMATCH)
            return status;

        // Grow with headroom for processes created before the next query.
        if (needed < size)
            needed = size;
        if (needed > PROCSENTINEL_SNAPSHOT_MAX - 0x10000u)
            return STATUS_INSUFFICIENT_RESOURCES;
        size = needed + 0x10000u;
    }
    return STATUS_INFO_LENGTH_MISMATCH;
}

// Advance to the next SYSTEM_PROCESS_INFORMATION entry, staying inside the buffer.
static PSYSTEM_PROCESS_INFORMATION NextProcessEntry(_In_ PVOID Base, _In_ ULONG Size, _In_ PSYSTEM_PROCESS_INFORMATION Current)
{
    ULONG_PTR offset;
    if (Current->NextEntryOffset == 0)
        return NULL;
    offset = (ULONG_PTR)((PUCHAR)Current - (PUCHAR)Base) + Current->NextEntryOffset;
    if (offset + sizeof(SYSTEM_PROCESS_INFORMATION) > Size)
        return NULL;
    return (PSYSTEM_PROCESS_INFORMATION)((PUCHAR)Base + offset);
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS CountLiveProcesses(_Out_ PULONG Count)
{
    PVOID snap = NULL;
    ULONG snapSize = 0;
    PSYSTEM_PROCESS_INFORMATION cur;
    NTSTATUS status;

    *Count = 0;
    status = QueryProcessSnapshot(&snap, &snapSize);
    if (!NT_SUCCESS(status))
        return status;

    for (cur = (PSYSTEM_PROCESS_INFORMATION)snap; cur != NULL; cur = NextProcessEntry(snap, snapSize, cur)) {
        if (cur->UniqueProcessId != NULL)   // skip the Idle "process" (PID 0)
            (*Count)++;
    }

    ExFreePoolWithTag(snap, PROCSENTINEL_POOL_TAG);
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS EnumerateProcesses(
    _Out_writes_to_(MaxCount, *Count) PPROCSENTINEL_PROC_INFO Out,
    _In_ ULONG MaxCount,
    _Out_ PULONG Count)
{
    PVOID snap = NULL;
    ULONG snapSize = 0;
    PSYSTEM_PROCESS_INFORMATION cur;
    NTSTATUS status;

    *Count = 0;
    status = QueryProcessSnapshot(&snap, &snapSize);
    if (!NT_SUCCESS(status))
        return status;

    for (cur = (PSYSTEM_PROCESS_INFORMATION)snap;
         cur != NULL && *Count < MaxCount;
         cur = NextProcessEntry(snap, snapSize, cur)) {
        PEPROCESS proc = NULL;
        PPROCSENTINEL_PROC_INFO entry;
        PUNICODE_STRING fullPath = NULL;

        if (cur->UniqueProcessId == NULL)
            continue;
        if (!NT_SUCCESS(PsLookupProcessByProcessId(cur->UniqueProcessId, &proc)))
            continue;

        entry = &Out[*Count];
        // Zero the whole entry first so no stale SystemBuffer bytes reach user mode.
        RtlZeroMemory(entry, sizeof(*entry));
        entry->Pid = HandleToULong(cur->UniqueProcessId);
        entry->ParentPid = HandleToULong(cur->InheritedFromUniqueProcessId);
        entry->Protection = GetProtectionLevel(proc);
        entry->State = PROCSENTINEL_STATE_RUNNING; // snapshot = live processes
        CopyShortName(proc, entry->ImageName);

        // Full NT image path if available, otherwise the snapshot's image name.
        if (NT_SUCCESS(SeLocateProcessImageName(proc, &fullPath)) && fullPath) {
            CopyUnicodeToUtf8(fullPath, entry->ImagePath, sizeof(entry->ImagePath));
            ExFreePool(fullPath);
        }
        else {
            CopyUnicodeToUtf8(&cur->ImageName, entry->ImagePath, sizeof(entry->ImagePath));
        }

        ObDereferenceObject(proc);
        (*Count)++;
    }

    ExFreePoolWithTag(snap, PROCSENTINEL_POOL_TAG);
    return STATUS_SUCCESS;
}

// ===================== PROCESS MEMORY DUMP =====================
/*
 *   1. User-mode sends a WireDumpRequest (PID, or short name if PID == 0).
 *   2. Name lookup walks the live process list; the first match is used and
 *      its reference is kept (no second lookup, no PID-reuse window).
 *   3. PP / PPL targets are refused.
 *   4. SizeOfImage is parsed from a private kernel copy of the PE headers
 *      (never from the user-visible output buffer).
 *   5. The image is copied page by page with MmCopyVirtualMemory into the
 *      METHOD_OUT_DIRECT MDL mapping. Unreadable pages are zero-filled and
 *      counted; images larger than the cap / buffer are truncated. Both are
 *      reported in WireDumpHeader.Flags.
 */

// Match a requested name against the kernel short name. The EPROCESS short
// name is truncated (14 chars + NUL), so a longer request also matches when
// the kernel name is a full-length prefix of it.
static BOOLEAN ShortNameMatches(_In_z_ const char* KernelName, _In_z_ const char* Requested)
{
    size_t kernelLen = strlen(KernelName);
    if (kernelLen == 0)
        return FALSE;
    if (_stricmp(KernelName, Requested) == 0)
        return TRUE;
    if (kernelLen >= PROCSENTINEL_MAX_NAME - 2 && strlen(Requested) > kernelLen)
        return (BOOLEAN)(_strnicmp(KernelName, Requested, kernelLen) == 0);
    return FALSE;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS FindProcessByName(_In_z_ const char* Name, _Outptr_ PEPROCESS* Process)
{
    PVOID snap = NULL;
    ULONG snapSize = 0;
    PSYSTEM_PROCESS_INFORMATION cur;
    NTSTATUS status;

    *Process = NULL;
    status = QueryProcessSnapshot(&snap, &snapSize);
    if (!NT_SUCCESS(status))
        return status;

    status = STATUS_NOT_FOUND;
    for (cur = (PSYSTEM_PROCESS_INFORMATION)snap; cur != NULL; cur = NextProcessEntry(snap, snapSize, cur)) {
        PEPROCESS proc = NULL;
        char shortName[PROCSENTINEL_MAX_NAME];

        if (cur->UniqueProcessId == NULL)
            continue;
        if (!NT_SUCCESS(PsLookupProcessByProcessId(cur->UniqueProcessId, &proc)))
            continue;

        CopyShortName(proc, shortName);
        if (ShortNameMatches(shortName, Name)) {
            *Process = proc;           // keep this reference; caller dereferences
            status = STATUS_SUCCESS;
            break;
        }
        ObDereferenceObject(proc);
    }

    ExFreePoolWithTag(snap, PROCSENTINEL_POOL_TAG);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS DumpProcess(
    _In_ const WireDumpRequest* Request,
    _Out_writes_bytes_(OutSize) PUCHAR Out,
    _In_ ULONG OutSize,
    _Out_ PULONG_PTR BytesWritten)
{
    PEPROCESS target = NULL;
    PVOID imageBase;
    UCHAR peHdr[0x400];
    SIZE_T copied = 0;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS64 nt;
    ULONG sizeOfImage, dataCap, toCopy, offset;
    ULONG flags = 0, unreadable = 0;
    WireDumpHeader header;
    PUCHAR data;
    NTSTATUS status;

    *BytesWritten = 0;
    if (OutSize < PROCSENTINEL_DUMP_MIN_BUFFER)
        return STATUS_BUFFER_TOO_SMALL;

    // ---- Resolve target (one reference, kept until the end) ----
    if (Request->Pid != 0) {
        status = PsLookupProcessByProcessId(ULongToHandle(Request->Pid), &target);
    }
    else {
        char name[PROCSENTINEL_MAX_NAME];
        RtlCopyMemory(name, Request->ImageName, sizeof(name));
        name[PROCSENTINEL_MAX_NAME - 1] = 0;
        if (name[0] == 0)
            return STATUS_INVALID_PARAMETER;
        status = FindProcessByName(name, &target);
    }
    if (!NT_SUCCESS(status))
        return status;

    // ---- Refuse protected targets (PsIsProtectedProcess covers PP and PPL) ----
    if (PsIsProtectedProcess(target)) {
        status = STATUS_ACCESS_DENIED;
        goto Exit;
    }

    imageBase = PsGetProcessSectionBaseAddress(target);
    if (!imageBase) {
        status = STATUS_NOT_SUPPORTED;   // e.g. System / minimal processes
        goto Exit;
    }

    // ---- Parse SizeOfImage from a private copy of the headers ----
    RtlZeroMemory(peHdr, sizeof(peHdr));
    status = MmCopyVirtualMemory(target, imageBase, PsGetCurrentProcess(), peHdr,
                                 sizeof(peHdr), KernelMode, &copied);
    if (!NT_SUCCESS(status) && status != STATUS_PARTIAL_COPY)
        goto Exit;
    if (copied > sizeof(peHdr))
        copied = sizeof(peHdr);
    if (copied < sizeof(IMAGE_DOS_HEADER)) {
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto Exit;
    }

    dos = (PIMAGE_DOS_HEADER)peHdr;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        (SIZE_T)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > copied) {
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto Exit;
    }
    nt = (PIMAGE_NT_HEADERS64)(peHdr + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto Exit;
    }
    // SizeOfImage has the same offset in IMAGE_OPTIONAL_HEADER32 and 64,
    // so this also works for WoW64 (PE32) images.
    sizeOfImage = nt->OptionalHeader.SizeOfImage;
    if (sizeOfImage == 0) {
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto Exit;
    }

    // ---- Decide how much to copy ----
    dataCap = OutSize - (ULONG)sizeof(WireDumpHeader);
    if (dataCap > PROCSENTINEL_DUMP_MAX_SIZE)
        dataCap = PROCSENTINEL_DUMP_MAX_SIZE;
    toCopy = sizeOfImage;
    if (toCopy > dataCap) {
        toCopy = dataCap;
        flags |= PROCSENTINEL_DUMP_FLAG_TRUNCATED;
    }

    // ---- Page-by-page copy; zero-fill pages that cannot be read ----
    data = Out + sizeof(WireDumpHeader);
    for (offset = 0; offset < toCopy; ) {
        ULONG chunk = toCopy - offset;
        SIZE_T got = 0;
        NTSTATUS copyStatus;

        if (chunk > PAGE_SIZE)
            chunk = PAGE_SIZE;

        copyStatus = MmCopyVirtualMemory(target, (PUCHAR)imageBase + offset,
                                         PsGetCurrentProcess(), data + offset,
                                         chunk, KernelMode, &got);
        if (!NT_SUCCESS(copyStatus)) {
            if (copyStatus == STATUS_PROCESS_IS_TERMINATING) {
                status = copyStatus;
                goto Exit;
            }
            if (got > chunk)
                got = chunk;
            RtlZeroMemory(data + offset + got, chunk - (ULONG)got);
            unreadable++;
        }
        offset += chunk;
    }
    if (unreadable != 0)
        flags |= PROCSENTINEL_DUMP_FLAG_PARTIAL;

    // ---- Header goes in front of the data ----
    RtlZeroMemory(&header, sizeof(header));
    header.Magic = PROCSENTINEL_DUMP_MAGIC;
    header.Flags = flags;
    header.ImageBase = (uint64_t)(ULONG_PTR)imageBase;
    header.Pid = HandleToULong(PsGetProcessId(target));
    header.SizeOfImage = sizeOfImage;
    header.DataLength = toCopy;
    header.UnreadablePages = unreadable;
    RtlCopyMemory(Out, &header, sizeof(header));

    *BytesWritten = (ULONG_PTR)sizeof(WireDumpHeader) + toCopy;
    status = STATUS_SUCCESS;

Exit:
    ObDereferenceObject(target);
    return status;
}

// ===================== DISPATCH ROUTINES =====================

NTSTATUS DispatchCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS DispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;   // in/out for METHOD_BUFFERED, input only for DUMP

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    UNREFERENCED_PARAMETER(DeviceObject);

    switch (code) {
    case IOCTL_PROCSENTINEL_GET_STATS:
        if (outLen < sizeof(PROCSENTINEL_STATS) || !buffer) {
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
        if (outLen < sizeof(PROCSENTINEL_EVENT) || !buffer) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            ULONG maxEvents = outLen / sizeof(PROCSENTINEL_EVENT);
            PPROCSENTINEL_EVENT out = (PPROCSENTINEL_EVENT)buffer;
            ULONG copiedEvents = 0;
            KIRQL old;
            KeAcquireSpinLock(&g_RingLock, &old);
            while (copiedEvents < maxEvents && g_EventRing.count > 0) {
                out[copiedEvents] = g_EventRing.entries[g_EventRing.tail];
                g_EventRing.tail = (g_EventRing.tail + 1) % PROCSENTINEL_MAX_EVENTS;
                g_EventRing.count--;
                copiedEvents++;
            }
            g_Stats.EventCount = g_EventRing.count;
            KeReleaseSpinLock(&g_RingLock, old);
            info = (ULONG_PTR)copiedEvents * sizeof(PROCSENTINEL_EVENT);
            status = STATUS_SUCCESS;
        }
        break;

    case IOCTL_PROCSENTINEL_START_MONITOR:
        {
            // Seed the live-process counter so exits of pre-existing processes
            // do not drive it negative. Approximate by design (races with
            // processes starting/exiting during the snapshot).
            ULONG live = 0;
            if (NT_SUCCESS(CountLiveProcesses(&live)))
                InterlockedExchange((volatile LONG*)&g_Stats.CurrentProcessCount, (LONG)live);
            WriteBooleanNoFence(&g_MonitoringEnabled, TRUE);
        }
        status = STATUS_SUCCESS;
        break;

    case IOCTL_PROCSENTINEL_STOP_MONITOR:
        WriteBooleanNoFence(&g_MonitoringEnabled, FALSE);
        status = STATUS_SUCCESS;
        break;

    case IOCTL_PROCSENTINEL_RESET_STATS:
        {
            KIRQL old;
            KeAcquireSpinLock(&g_RingLock, &old);
            RtlZeroMemory(&g_Stats, sizeof(g_Stats));
            EventRingResetKernel(&g_EventRing);   // O(1): indices only
            KeReleaseSpinLock(&g_RingLock, old);
        }
        status = STATUS_SUCCESS;
        break;

    case IOCTL_PROCSENTINEL_GET_PROCESS_LIST:
        if (outLen < sizeof(PROCSENTINEL_PROC_INFO) || !buffer) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            ULONG maxCount = outLen / sizeof(PROCSENTINEL_PROC_INFO);
            ULONG count = 0;
            if (maxCount > PROCSENTINEL_MAX_PROCESS_LIST)
                maxCount = PROCSENTINEL_MAX_PROCESS_LIST;
            status = EnumerateProcesses((PPROCSENTINEL_PROC_INFO)buffer, maxCount, &count);
            if (NT_SUCCESS(status))
                info = (ULONG_PTR)count * sizeof(PROCSENTINEL_PROC_INFO);
        }
        break;

    case IOCTL_PROCSENTINEL_DUMP_PROCESS:
        InterlockedIncrement((volatile LONG*)&g_Stats.DumpRequests);
        // SeDebugPrivilege must be held *and enabled* by the caller.
        if (Irp->RequestorMode != KernelMode &&
            !SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_DEBUG_PRIVILEGE), Irp->RequestorMode)) {
            status = STATUS_PRIVILEGE_NOT_HELD;
        }
        else if (inLen < sizeof(WireDumpRequest) || !buffer) {
            status = STATUS_INVALID_PARAMETER;
        }
        else if (!Irp->MdlAddress || outLen < PROCSENTINEL_DUMP_MIN_BUFFER) {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        else {
            // METHOD_OUT_DIRECT: input in SystemBuffer (copy it to a local),
            // output described by Irp->MdlAddress.
            WireDumpRequest request;
            PUCHAR out;
            RtlCopyMemory(&request, buffer, sizeof(request));
            out = (PUCHAR)MmGetSystemAddressForMdlSafe(Irp->MdlAddress,
                                                       NormalPagePriority | MdlMappingNoExecute);
            if (!out) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            }
            else {
                status = DumpProcess(&request, out, outLen, &info);
            }
        }
        if (!NT_SUCCESS(status)) {
            InterlockedIncrement((volatile LONG*)&g_Stats.DumpFailures);
            info = 0;
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

VOID DriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symLink;

    if (g_NotifyRegistered) {
        // Removal waits for in-flight callbacks to finish.
        PsSetCreateProcessNotifyRoutineEx(ProcessNotifyCallback, TRUE);
        g_NotifyRegistered = FALSE;
    }

    RtlInitUnicodeString(&symLink, SYMLINK_NAME);
    IoDeleteSymbolicLink(&symLink);

    if (DriverObject->DeviceObject)
        IoDeleteDevice(DriverObject->DeviceObject);

    g_DeviceObject = NULL;
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    UNICODE_STRING devName, symLink;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlInitUnicodeString(&devName, DEVICE_NAME);
    RtlInitUnicodeString(&symLink, SYMLINK_NAME);

    KeInitializeSpinLock(&g_RingLock);
    EventRingInitKernel(&g_EventRing);
    RtlZeroMemory(&g_Stats, sizeof(g_Stats));
    g_MonitoringEnabled = FALSE;

    DriverObject->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;
    DriverObject->DriverUnload = DriverUnload;

    // Only SYSTEM and (elevated) Administrators may open the device.
    status = IoCreateDeviceSecure(
        DriverObject,
        0,
        &devName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &SDDL_DEVOBJ_SYS_ALL_ADM_ALL,
        &GUID_DEVCLASS_PROCSENTINEL,
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

    // Register the process notify routine (Ex version). Without it the
    // driver has no reason to stay loaded, so fail the load visibly.
    status = PsSetCreateProcessNotifyRoutineEx(ProcessNotifyCallback, FALSE);
    if (!NT_SUCCESS(status)) {
        DbgPrint("ProcSentinel: PsSetCreateProcessNotifyRoutineEx failed 0x%08X%s\n", status,
                 status == STATUS_ACCESS_DENIED ? " (was the driver linked with /INTEGRITYCHECK?)" : "");
        IoDeleteSymbolicLink(&symLink);
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
        return status;
    }
    g_NotifyRegistered = TRUE;

    DbgPrint("ProcSentinel: Driver loaded successfully. Device \\Device\\ProcSentinelDrv ready.\n");
    return STATUS_SUCCESS;
}

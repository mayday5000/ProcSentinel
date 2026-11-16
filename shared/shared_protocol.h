/*
 * shared_protocol.h
 * ---------------------------------------------------------------------------
 * Single source of truth for the ProcSentinel wire protocol (kernel <-> user-mode).
 *
 * Include from:
 *   - Kernel WDM driver (C)
 *   - User-mode app (C++)
 *   - Mock / unit tests (C or C++, any OS)
 *
 * Layout MUST stay binary-compatible between the driver and the client.
 * Every struct below is packed to 1 byte and its size / key offsets are
 * checked at compile time (PROCSENTINEL_STATIC_ASSERT at the end of the file).
 * ---------------------------------------------------------------------------
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include <string.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------------- */
#define PROCSENTINEL_MAX_EVENTS       2048
#define PROCSENTINEL_MAX_NAME         16
#define PROCSENTINEL_MAX_PATH         260
#define PROCSENTINEL_MAX_PROCESS_LIST 1024
#define PROCSENTINEL_DUMP_MAX_SIZE    (4 * 1024 * 1024)   /* image bytes, excluding the dump header */
#define PROCSENTINEL_DUMP_PAGE_SIZE   0x1000

/* ---------------------------------------------------------------------------
 * IOCTL codes
 *
 * Computed with the same formula as CTL_CODE() so this header does not need
 * <winioctl.h> or <wdm.h> (host tests build on any OS). The driver and the
 * client both assert that these values equal CTL_CODE(...).
 *
 * Access bits: read-only queries need FILE_READ_DATA, state-changing calls
 * need FILE_WRITE_DATA, and DUMP needs both. The device object itself is
 * restricted to SYSTEM and Administrators (see the driver).
 * ------------------------------------------------------------------------- */
#define PROCSENTINEL_FILE_DEVICE_UNKNOWN  0x00000022u
#define PROCSENTINEL_METHOD_BUFFERED      0u
#define PROCSENTINEL_METHOD_OUT_DIRECT    2u
#define PROCSENTINEL_FILE_READ_DATA       0x0001u
#define PROCSENTINEL_FILE_WRITE_DATA      0x0002u

#define PROCSENTINEL_CTL_CODE(function, method, access) \
    ((PROCSENTINEL_FILE_DEVICE_UNKNOWN << 16) | ((access) << 14) | ((function) << 2) | (method))

#define IOCTL_PROCSENTINEL_GET_STATS        PROCSENTINEL_CTL_CODE(0xB00u, PROCSENTINEL_METHOD_BUFFERED,   PROCSENTINEL_FILE_READ_DATA)
#define IOCTL_PROCSENTINEL_GET_EVENTS       PROCSENTINEL_CTL_CODE(0xB01u, PROCSENTINEL_METHOD_BUFFERED,   PROCSENTINEL_FILE_READ_DATA)
#define IOCTL_PROCSENTINEL_START_MONITOR    PROCSENTINEL_CTL_CODE(0xB02u, PROCSENTINEL_METHOD_BUFFERED,   PROCSENTINEL_FILE_WRITE_DATA)
#define IOCTL_PROCSENTINEL_STOP_MONITOR     PROCSENTINEL_CTL_CODE(0xB03u, PROCSENTINEL_METHOD_BUFFERED,   PROCSENTINEL_FILE_WRITE_DATA)
#define IOCTL_PROCSENTINEL_RESET_STATS      PROCSENTINEL_CTL_CODE(0xB04u, PROCSENTINEL_METHOD_BUFFERED,   PROCSENTINEL_FILE_WRITE_DATA)
#define IOCTL_PROCSENTINEL_GET_PROCESS_LIST PROCSENTINEL_CTL_CODE(0xB05u, PROCSENTINEL_METHOD_BUFFERED,   PROCSENTINEL_FILE_READ_DATA)
#define IOCTL_PROCSENTINEL_DUMP_PROCESS     PROCSENTINEL_CTL_CODE(0xB06u, PROCSENTINEL_METHOD_OUT_DIRECT, PROCSENTINEL_FILE_READ_DATA | PROCSENTINEL_FILE_WRITE_DATA)

/* ---------------------------------------------------------------------------
 * Enumerations
 * ------------------------------------------------------------------------- */

/* Event types (STATE is reserved; the driver currently emits CREATE and EXIT only) */
#define PROCSENTINEL_EVENT_CREATE     1
#define PROCSENTINEL_EVENT_EXIT       2
#define PROCSENTINEL_EVENT_STATE      3

/* Process state (ZOMBIE / SUSPENDED are reserved; not produced by the driver yet) */
#define PROCSENTINEL_STATE_RUNNING    0
#define PROCSENTINEL_STATE_EXITING    1
#define PROCSENTINEL_STATE_ZOMBIE     2
#define PROCSENTINEL_STATE_SUSPENDED  3

/* WireProcInfo.Protection */
#define PROCSENTINEL_PROTECTION_NONE  0   /* not protected                           */
#define PROCSENTINEL_PROTECTION_PP    1   /* full Protected Process (PP)             */
#define PROCSENTINEL_PROTECTION_PPL   2   /* Protected Process Light (PPL)           */

/* WireDumpHeader.Flags */
#define PROCSENTINEL_DUMP_FLAG_TRUNCATED  0x00000001u /* SizeOfImage exceeded the cap / buffer; DataLength < SizeOfImage */
#define PROCSENTINEL_DUMP_FLAG_PARTIAL    0x00000002u /* UnreadablePages > 0; those pages are zero-filled          */

#define PROCSENTINEL_DUMP_MAGIC           0x4D445350u /* "PSDM" little-endian */

/*
 * Timestamp is 64-bit 100-ns system time (same as LARGE_INTEGER.QuadPart).
 * Strings are NUL-terminated; ImagePath is UTF-8.
 */
#pragma pack(push, 1)

typedef struct _WireEvent {
    int64_t  Timestamp;
    uint32_t ProcessId;
    uint32_t ParentProcessId;    /* 0 on EXIT */
    uint8_t  EventType;          /* PROCSENTINEL_EVENT_* */
    uint8_t  State;              /* PROCSENTINEL_STATE_* */
    uint16_t Reserved;
    char     ImageName[PROCSENTINEL_MAX_NAME];
    char     ImagePath[PROCSENTINEL_MAX_PATH];
} WireEvent;

typedef struct _WireProcInfo {
    uint32_t Pid;
    uint32_t ParentPid;
    uint8_t  Protection;         /* PROCSENTINEL_PROTECTION_* */
    uint8_t  State;              /* PROCSENTINEL_STATE_* */
    uint16_t Reserved;
    char     ImageName[PROCSENTINEL_MAX_NAME];
    char     ImagePath[PROCSENTINEL_MAX_PATH];
} WireProcInfo;

typedef struct _WireStats {
    uint64_t Creates;
    uint64_t Exits;
    uint64_t StateChanges;       /* reserved, always 0 */
    uint32_t CurrentProcessCount;/* seeded on START_MONITOR, then +create / -exit (never below 0) */
    uint32_t EventCount;
    uint32_t DroppedEvents;
    uint32_t DumpRequests;       /* every DUMP_PROCESS request that reached the driver */
    uint32_t DumpFailures;       /* requests that completed with an error status       */
} WireStats;

/* DUMP_PROCESS input. Pid != 0 selects by PID; otherwise ImageName (short name). */
typedef struct _WireDumpRequest {
    uint32_t Pid;
    uint32_t Reserved;
    char     ImageName[PROCSENTINEL_MAX_NAME];
} WireDumpRequest;

/* DUMP_PROCESS output: this header, then DataLength bytes of the image. */
typedef struct _WireDumpHeader {
    uint32_t Magic;              /* PROCSENTINEL_DUMP_MAGIC */
    uint32_t Flags;              /* PROCSENTINEL_DUMP_FLAG_* */
    uint64_t ImageBase;          /* target's image base address */
    uint32_t Pid;                /* PID that was actually dumped */
    uint32_t SizeOfImage;        /* from the PE optional header */
    uint32_t DataLength;         /* bytes that follow this header */
    uint32_t UnreadablePages;    /* pages that could not be read (zero-filled) */
} WireDumpHeader;

#pragma pack(pop)

/* Output buffer sizes for DUMP_PROCESS */
#define PROCSENTINEL_DUMP_BUFFER_SIZE  ((uint32_t)sizeof(WireDumpHeader) + (uint32_t)PROCSENTINEL_DUMP_MAX_SIZE)
#define PROCSENTINEL_DUMP_MIN_BUFFER   ((uint32_t)sizeof(WireDumpHeader) + (uint32_t)PROCSENTINEL_DUMP_PAGE_SIZE)

/* Kernel / C historical aliases */
typedef WireEvent    PROCSENTINEL_EVENT;
typedef WireEvent*   PPROCSENTINEL_EVENT;
typedef WireProcInfo PROCSENTINEL_PROC_INFO;
typedef WireProcInfo* PPROCSENTINEL_PROC_INFO;
typedef WireStats    PROCSENTINEL_STATS;
typedef WireStats*   PPROCSENTINEL_STATS;

/* ---------------------------------------------------------------------------
 * Compile-time layout checks (C and C++)
 * ------------------------------------------------------------------------- */
#if defined(__cplusplus)
#  define PROCSENTINEL_STATIC_ASSERT(expr, msg) static_assert(expr, msg)
#else
#  define PROCSENTINEL_SA_CONCAT_(a, b) a##b
#  define PROCSENTINEL_SA_CONCAT(a, b)  PROCSENTINEL_SA_CONCAT_(a, b)
#  define PROCSENTINEL_STATIC_ASSERT(expr, msg) \
       typedef char PROCSENTINEL_SA_CONCAT(procsentinel_static_assert_, __LINE__)[(expr) ? 1 : -1]
#endif

PROCSENTINEL_STATIC_ASSERT(sizeof(WireEvent) == 296, "WireEvent size");
PROCSENTINEL_STATIC_ASSERT(offsetof(WireEvent, EventType) == 16, "WireEvent.EventType offset");
PROCSENTINEL_STATIC_ASSERT(offsetof(WireEvent, ImageName) == 20, "WireEvent.ImageName offset");
PROCSENTINEL_STATIC_ASSERT(offsetof(WireEvent, ImagePath) == 36, "WireEvent.ImagePath offset");

PROCSENTINEL_STATIC_ASSERT(sizeof(WireProcInfo) == 288, "WireProcInfo size");
PROCSENTINEL_STATIC_ASSERT(offsetof(WireProcInfo, Protection) == 8, "WireProcInfo.Protection offset");
PROCSENTINEL_STATIC_ASSERT(offsetof(WireProcInfo, ImageName) == 12, "WireProcInfo.ImageName offset");
PROCSENTINEL_STATIC_ASSERT(offsetof(WireProcInfo, ImagePath) == 28, "WireProcInfo.ImagePath offset");

/* WireStats fields are updated with Interlocked*: keep them naturally aligned. */
PROCSENTINEL_STATIC_ASSERT(sizeof(WireStats) == 44, "WireStats size");
PROCSENTINEL_STATIC_ASSERT(offsetof(WireStats, Exits) == 8, "WireStats.Exits offset");
PROCSENTINEL_STATIC_ASSERT(offsetof(WireStats, CurrentProcessCount) == 24, "WireStats.CurrentProcessCount offset");
PROCSENTINEL_STATIC_ASSERT(offsetof(WireStats, DumpFailures) == 40, "WireStats.DumpFailures offset");

PROCSENTINEL_STATIC_ASSERT(sizeof(WireDumpRequest) == 24, "WireDumpRequest size");
PROCSENTINEL_STATIC_ASSERT(sizeof(WireDumpHeader) == 32, "WireDumpHeader size");
PROCSENTINEL_STATIC_ASSERT(offsetof(WireDumpHeader, ImageBase) == 8, "WireDumpHeader.ImageBase offset");
PROCSENTINEL_STATIC_ASSERT(offsetof(WireDumpHeader, UnreadablePages) == 28, "WireDumpHeader.UnreadablePages offset");

/*
 * Simple ring metadata for *user-mode / host tests*.
 * The real kernel driver uses its own NonPaged array + KSPIN_LOCK with the
 * same drop-oldest and reset semantics. Do not replace the kernel path with
 * std::atomic.
 */
typedef struct _WireEventRing {   /* single-threaded host helper: no volatile */
    WireEvent entries[PROCSENTINEL_MAX_EVENTS];
    uint32_t  head;
    uint32_t  tail;
    uint32_t  count;
    uint32_t  dropped;
} WireEventRing;

/* ---------- Ring helpers (C, for tests + shared host tools) ---------- */

static inline void EventRingInit(WireEventRing* ring)
{
    memset(ring, 0, sizeof(*ring));
}

static inline int EventRingIsFull(const WireEventRing* ring)
{
    return ring->count >= PROCSENTINEL_MAX_EVENTS;
}

static inline int EventRingIsEmpty(const WireEventRing* ring)
{
    return ring->count == 0;
}

/* Push: if full, drop oldest (same policy as the kernel EventRingPushKernel) */
static inline void EventRingPush(WireEventRing* ring, const WireEvent* ev)
{
    if (ring->count >= PROCSENTINEL_MAX_EVENTS) {
        ring->tail = (ring->tail + 1) % PROCSENTINEL_MAX_EVENTS;
        ring->count--;
        ring->dropped++;
    }
    ring->entries[ring->head] = *ev;
    ring->head = (ring->head + 1) % PROCSENTINEL_MAX_EVENTS;
    ring->count++;
}

/* Pop one event; returns 1 on success, 0 if empty */
static inline int EventRingPop(WireEventRing* ring, WireEvent* out)
{
    if (ring->count == 0)
        return 0;
    *out = ring->entries[ring->tail];
    ring->tail = (ring->tail + 1) % PROCSENTINEL_MAX_EVENTS;
    ring->count--;
    return 1;
}

static inline uint32_t EventRingCount(const WireEventRing* ring)
{
    return ring->count;
}

/* Reset = what IOCTL_PROCSENTINEL_RESET_STATS does in the kernel:
 * indices, count and the dropped counter go back to 0; entries are not wiped. */
static inline void EventRingReset(WireEventRing* ring)
{
    ring->head = 0;
    ring->tail = 0;
    ring->count = 0;
    ring->dropped = 0;
}

#ifdef __cplusplus
}
#endif

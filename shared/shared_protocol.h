/*
 * shared_protocol.h
 * ---------------------------------------------------------------------------
 * Single source of truth for the ProcSentinel wire protocol (kernel <-> user-mode).
 *
 * Include from:
 *   - Kernel WDM driver (C)
 *   - User-mode app (C++)
 *   - Mock / unit tests (C or C++)
 *
 * Layout MUST stay binary-compatible with IOCTL METHOD_BUFFERED transfers.
 * ---------------------------------------------------------------------------
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include <string.h>
#include <stddef.h>

/* Keep in sync across driver / usermode / tests. */
#define PROCSENTINEL_MAX_EVENTS       2048
#define PROCSENTINEL_MAX_NAME         16
#define PROCSENTINEL_MAX_PATH         260
#define PROCSENTINEL_MAX_PROCESS_LIST 1024
#define PROCSENTINEL_DUMP_MAX_SIZE    (4 * 1024 * 1024)

/* Event types */
#define PROCSENTINEL_EVENT_CREATE     1
#define PROCSENTINEL_EVENT_EXIT       2
#define PROCSENTINEL_EVENT_STATE      3

/* Process state */
#define PROCSENTINEL_STATE_RUNNING    0
#define PROCSENTINEL_STATE_EXITING    1
#define PROCSENTINEL_STATE_ZOMBIE     2
#define PROCSENTINEL_STATE_SUSPENDED  3

/*
 * WireEvent / WireStats / WireProcInfo are packed 1-byte aligned.
 * Timestamp is 64-bit 100-ns FILETIME (same as LARGE_INTEGER.QuadPart).
 */
#pragma pack(push, 1)

typedef struct _WireEvent {
    int64_t  Timestamp;
    uint32_t ProcessId;
    uint32_t ParentProcessId;
    uint8_t  EventType;          /* PROCSENTINEL_EVENT_* */
    uint8_t  State;              /* PROCSENTINEL_STATE_* */
    uint16_t Reserved;
    char     ImageName[PROCSENTINEL_MAX_NAME];
    char     ImagePath[PROCSENTINEL_MAX_PATH];
} WireEvent;

typedef struct _WireProcInfo {
    uint64_t Pid;                /* ULONG_PTR on wire as 64-bit */
    uint64_t ParentPid;
    uint8_t  Protected;
    uint8_t  State;
    uint16_t Reserved;
    char     ImageName[PROCSENTINEL_MAX_NAME];
    char     ImagePath[PROCSENTINEL_MAX_PATH];
} WireProcInfo;

typedef struct _WireStats {
    uint64_t Creates;
    uint64_t Exits;
    uint64_t StateChanges;
    uint32_t CurrentProcessCount;
    uint32_t EventCount;
    uint32_t DroppedEvents;
    uint32_t DumpRequests;
    uint32_t DumpFailures;
} WireStats;

#pragma pack(pop)

/* Kernel / C historical aliases */
typedef WireEvent    PROCSENTINEL_EVENT;
typedef WireEvent*   PPROCSENTINEL_EVENT;
typedef WireProcInfo PROCSENTINEL_PROC_INFO;
typedef WireProcInfo* PPROCSENTINEL_PROC_INFO;
typedef WireStats    PROCSENTINEL_STATS;
typedef WireStats*   PPROCSENTINEL_STATS;

/*
 * Simple ring metadata for *user-mode / host tests*.
 * The real kernel driver still uses a NonPaged array + KSPIN_LOCK
 * (IRQL-safe). Do not replace the kernel path with std::atomic.
 */
typedef struct _WireEventRing {
    WireEvent         entries[PROCSENTINEL_MAX_EVENTS];
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t count;
    volatile uint32_t dropped;
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

/* Push: if full, drop oldest (same policy as kernel EventRingPushKernel) */
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

static inline void EventRingReset(WireEventRing* ring)
{
    ring->head = ring->tail = ring->count = 0;
    /* keep dropped for diagnostics */
}

#ifdef __cplusplus
}
#endif

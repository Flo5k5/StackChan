/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * Wire protocol parser for the Anthropic Claude Hardware Buddy.
 *
 * Consumes newline-delimited JSON lines (from claude_nus.c, or in the future
 * from a WS bridge) and emits high-level events via a callback. Mirrors the
 * reference spec at anthropics/claude-desktop-buddy/REFERENCE.md.
 *
 * The parser is transport-agnostic: feed it any complete JSON line and it
 * will fire the right event. This lets the firmware own a single state
 * machine driven by either BLE or WS, transparently.
 *
 * Memory contract: all string fields in the event struct point into a
 * scratch buffer owned by the parser — they are ONLY valid until the next
 * call to claude_protocol_parse_line. The callback must copy anything it
 * wants to keep (typical: copy into a FreeRTOS queue).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of "entries" we surface per heartbeat. The desktop sends
 * a short list (usually 3-5), but we cap defensively. */
#define CLAUDE_MAX_ENTRIES 8
/* Max length of a single entry string (matches the "10:42 git push" format). */
#define CLAUDE_ENTRY_LEN 48

/* Derived device state, mirrors the REFERENCE.md rules. */
typedef enum {
    CLAUDE_STATE_SLEEP = 0,  /* no snapshot received in ~30s (driven by caller) */
    CLAUDE_STATE_IDLE,       /* total == 0 */
    CLAUDE_STATE_BUSY,       /* running > 0 */
    CLAUDE_STATE_ATTENTION,  /* waiting > 0 (prompt blocking) */
} claude_state_t;

/* Top-level event kinds. The parser discriminates on the JSON shape:
 *  - heartbeat (has "total"): state snapshot
 *  - turn (has "evt":"turn"): assistant message
 *  - cmd (has "cmd"): request from desktop (status, owner, name, unpair, char_*)
 *  - time (has "time"): one-shot time sync
 */
typedef enum {
    CLAUDE_EVENT_HEARTBEAT = 0,
    CLAUDE_EVENT_TURN,
    CLAUDE_EVENT_CMD,
    CLAUDE_EVENT_TIME,
    CLAUDE_EVENT_UNKNOWN,
} claude_event_kind_t;

typedef struct {
    /* Always set. */
    claude_event_kind_t kind;

    /* HEARTBEAT fields (valid when kind == CLAUDE_EVENT_HEARTBEAT). */
    struct {
        int total;            /* sessions open */
        int running;          /* sessions actively generating */
        int waiting;          /* sessions blocked on a permission prompt */
        int tokens;           /* lifetime tokens for the current session(s) */
        int tokens_today;     /* tokens since local midnight */
        const char *msg;      /* short status text (may be empty) */
        const char *entries[CLAUDE_MAX_ENTRIES];  /* recent action labels */
        int entry_count;
        /* Permission prompt (present only when waiting > 0). */
        struct {
            const char *id;    /* matches the response "permission.id" */
            const char *tool;  /* "Bash", "Edit", ... */
            const char *hint;  /* short preview of the tool input */
        } prompt;
        int has_prompt;
        /* Caller-derived state — filled in by the parser from the counters. */
        claude_state_t state;
    } heartbeat;

    /* TURN fields (valid when kind == CLAUDE_EVENT_TURN).
     * We only surface the concatenated text of content blocks for now; the
     * firmware doesn't need block-level structure. */
    struct {
        const char *role;     /* "assistant" or "user" */
        const char *text;     /* concatenated text content (may be empty) */
    } turn;

    /* CMD fields (valid when kind == CLAUDE_EVENT_CMD). */
    struct {
        const char *cmd;      /* "status", "owner", "name", "unpair", "char_begin", "file", "chunk", "file_end", "char_end" */
        const char *name;     /* for "owner" / "name" / "char_begin.name" */
    } cmd;

    /* TIME fields (valid when kind == CLAUDE_EVENT_TIME). */
    struct {
        int64_t epoch_seconds;
        int32_t tz_offset_seconds;
    } time;
} claude_event_t;

/* Callback fired for every parsed line. Runs in the parser's calling task
 * (NimBLE host task for BLE, WS worker for WS) — must be non-blocking.
 * `event` is only valid for the duration of the call. */
typedef void (*claude_event_cb_t)(const claude_event_t *event, void *user_data);

/* Register the event callback. `user_data` is passed back untouched. */
void claude_protocol_set_callback(claude_event_cb_t cb, void *user_data);

/* Parse a single newline-stripped JSON line and fire the callback if set.
 * Tolerant: malformed JSON or unknown shapes fire a CLAUDE_EVENT_UNKNOWN event
 * (kind only) rather than crashing. Returns 0 on success, -1 on parse error. */
int claude_protocol_parse_line(const char *line);

/* Convenience: derive a state from counters (used internally and exposed for
 * the WS bridge path that builds a synthetic heartbeat). */
claude_state_t claude_protocol_derive_state(int total, int running, int waiting);

#ifdef __cplusplus
}
#endif

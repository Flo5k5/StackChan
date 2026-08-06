/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "claude_protocol.h"

#include <string.h>
#include <ArduinoJson.h>

/* Scratch storage for the event being dispatched. All const char* fields in
 * the emitted event point into these buffers, valid only until the next call.
 * Sizes match REFERENCE.md's practical limits (entries ~48 chars, msg short,
 * tool/hint a few dozen chars). */
static char g_msg_buf[128];
static char g_entry_bufs[CLAUDE_MAX_ENTRIES][CLAUDE_ENTRY_LEN];
static char g_prompt_id_buf[48];
static char g_prompt_tool_buf[32];
static char g_prompt_hint_buf[96];
static char g_role_buf[16];
static char g_text_buf[512];  /* turn content, truncated from 4KB */
static char g_cmd_name_buf[48];
static char g_cmd_name_value_buf[48];

static claude_event_cb_t g_cb      = nullptr;
static void *g_user_data            = nullptr;

void claude_protocol_set_callback(claude_event_cb_t cb, void *user_data) {
    g_cb        = cb;
    g_user_data = user_data;
}

claude_state_t claude_protocol_derive_state(int total, int running, int waiting) {
    /* REFERENCE.md rules — order matters: waiting wins over running. */
    if (waiting > 0) return CLAUDE_STATE_ATTENTION;
    if (running > 0) return CLAUDE_STATE_BUSY;
    if (total == 0) return CLAUDE_STATE_IDLE;
    /* total > 0 but nothing running/waiting: treat as busy (sessions open). */
    return CLAUDE_STATE_BUSY;
}

/* Discriminate on the JSON shape — REFERENCE.md has no explicit "type" field.
 * Detection order matters because a heartbeat could theoretically carry an
 * "evt" or "cmd" key in its entries (it won't in practice, but be defensive). */
static claude_event_kind_t detect_kind(const JsonDocument &doc) {
    /* Time sync: array form {"time": [epoch, tz]}. */
    if (doc["time"].is<JsonArrayConst>()) return CLAUDE_EVENT_TIME;
    /* Turn: explicit evt field. */
    if (!doc["evt"].isNull()) {
        const char *evt = doc["evt"];
        if (evt && strcmp(evt, "turn") == 0) return CLAUDE_EVENT_TURN;
    }
    /* Command: explicit cmd field. */
    if (!doc["cmd"].isNull()) return CLAUDE_EVENT_CMD;
    /* Heartbeat: presence of "total" (integer). This is the discriminator
     * the reference buddy uses too. */
    if (!doc["total"].isNull()) return CLAUDE_EVENT_HEARTBEAT;
    return CLAUDE_EVENT_UNKNOWN;
}

static void emit_heartbeat(const JsonDocument &doc, claude_event_t *ev) {
    ev->heartbeat.total        = doc["total"]        | 0;
    ev->heartbeat.running      = doc["running"]      | 0;
    ev->heartbeat.waiting      = doc["waiting"]      | 0;
    ev->heartbeat.tokens       = doc["tokens"]       | 0;
    ev->heartbeat.tokens_today = doc["tokens_today"] | 0;

    /* msg: short status text (may be absent). */
    const char *msg = doc["msg"];
    strncpy(g_msg_buf, msg ? msg : "", sizeof(g_msg_buf) - 1);
    g_msg_buf[sizeof(g_msg_buf) - 1] = '\0';
    ev->heartbeat.msg = g_msg_buf;

    /* entries: array of short strings ("HH:MM action"). */
    ev->heartbeat.entry_count = 0;
    JsonArrayConst entries = doc["entries"].as<JsonArrayConst>();
    int i = 0;
    for (JsonVariantConst v : entries) {
        if (i >= CLAUDE_MAX_ENTRIES) break;
        const char *s = v.as<const char *>();
        strncpy(g_entry_bufs[i], s ? s : "", CLAUDE_ENTRY_LEN - 1);
        g_entry_bufs[i][CLAUDE_ENTRY_LEN - 1] = '\0';
        ev->heartbeat.entries[i] = g_entry_bufs[i];
        i++;
    }
    ev->heartbeat.entry_count = i;

    /* prompt: present only when waiting > 0 (permission needed). */
    JsonObjectConst prompt = doc["prompt"].as<JsonObjectConst>();
    if (!prompt.isNull()) {
        ev->heartbeat.has_prompt = 1;
        const char *id   = prompt["id"];
        const char *tool = prompt["tool"];
        const char *hint = prompt["hint"];
        strncpy(g_prompt_id_buf, id ? id : "", sizeof(g_prompt_id_buf) - 1);
        strncpy(g_prompt_tool_buf, tool ? tool : "", sizeof(g_prompt_tool_buf) - 1);
        strncpy(g_prompt_hint_buf, hint ? hint : "", sizeof(g_prompt_hint_buf) - 1);
        g_prompt_id_buf[sizeof(g_prompt_id_buf) - 1] = '\0';
        g_prompt_tool_buf[sizeof(g_prompt_tool_buf) - 1] = '\0';
        g_prompt_hint_buf[sizeof(g_prompt_hint_buf) - 1] = '\0';
        ev->heartbeat.prompt.id   = g_prompt_id_buf;
        ev->heartbeat.prompt.tool = g_prompt_tool_buf;
        ev->heartbeat.prompt.hint = g_prompt_hint_buf;
    } else {
        ev->heartbeat.has_prompt   = 0;
        ev->heartbeat.prompt.id    = nullptr;
        ev->heartbeat.prompt.tool  = nullptr;
        ev->heartbeat.prompt.hint  = nullptr;
    }

    ev->heartbeat.state = claude_protocol_derive_state(
        ev->heartbeat.total, ev->heartbeat.running, ev->heartbeat.waiting);
}

static void emit_turn(const JsonDocument &doc, claude_event_t *ev) {
    const char *role = doc["role"];
    strncpy(g_role_buf, role ? role : "", sizeof(g_role_buf) - 1);
    g_role_buf[sizeof(g_role_buf) - 1] = '\0';
    ev->turn.role = g_role_buf;

    /* Concatenate text blocks. content is an array of {type:"text", text:"..."}.
     * The reference sends this after every turn; we keep the firmware's view
     * short (512 chars) since the device UI only shows the latest message. */
    size_t written = 0;
    JsonArrayConst content = doc["content"].as<JsonArrayConst>();
    for (JsonVariantConst block : content) {
        const char *type = block["type"];
        if (type && strcmp(type, "text") == 0) {
            const char *text = block["text"];
            if (text) {
                size_t remaining = sizeof(g_text_buf) - 1 - written;
                size_t n = strnlen(text, remaining);
                memcpy(g_text_buf + written, text, n);
                written += n;
                if (written >= sizeof(g_text_buf) - 1) break;
            }
        }
    }
    g_text_buf[written] = '\0';
    ev->turn.text = g_text_buf;
}

static void emit_cmd(const JsonDocument &doc, claude_event_t *ev) {
    const char *cmd = doc["cmd"];
    /* Surface the command name (e.g. "status", "owner", "char_begin"). */
    strncpy(g_cmd_name_buf, cmd ? cmd : "", sizeof(g_cmd_name_buf) - 1);
    g_cmd_name_buf[sizeof(g_cmd_name_buf) - 1] = '\0';
    ev->cmd.cmd = g_cmd_name_buf;

    /* name field — shared by "owner", "name", and "char_begin.name". */
    const char *name = doc["name"];
    strncpy(g_cmd_name_value_buf, name ? name : "", sizeof(g_cmd_name_value_buf) - 1);
    g_cmd_name_value_buf[sizeof(g_cmd_name_value_buf) - 1] = '\0';
    ev->cmd.name = g_cmd_name_value_buf;
}

int claude_protocol_parse_line(const char *line) {
    if (!line || !line[0]) {
        return -1;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, line);
    if (err) {
        /* Fire an UNKNOWN so the firmware can log/ignore gracefully. */
        if (g_cb) {
            claude_event_t ev = {};
            ev.kind = CLAUDE_EVENT_UNKNOWN;
            g_cb(&ev, g_user_data);
        }
        return -1;
    }

    claude_event_t ev = {};
    ev.kind = detect_kind(doc);

    switch (ev.kind) {
        case CLAUDE_EVENT_HEARTBEAT:
            emit_heartbeat(doc, &ev);
            break;
        case CLAUDE_EVENT_TURN:
            emit_turn(doc, &ev);
            break;
        case CLAUDE_EVENT_CMD:
            emit_cmd(doc, &ev);
            break;
        case CLAUDE_EVENT_TIME: {
            JsonArrayConst time_arr = doc["time"].as<JsonArrayConst>();
            if (time_arr.size() >= 2) {
                ev.time.epoch_seconds     = time_arr[0].as<int64_t>();
                ev.time.tz_offset_seconds = time_arr[1].as<int32_t>();
            }
            break;
        }
        case CLAUDE_EVENT_UNKNOWN:
        default:
            break;
    }

    if (g_cb) {
        g_cb(&ev, g_user_data);
    }
    return 0;
}

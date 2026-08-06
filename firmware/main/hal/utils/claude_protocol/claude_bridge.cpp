/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * Integration glue between the C transport (claude_nus.c) + parser
 * (claude_protocol.c) and the C++ firmware (GetHAL().onWsClaudeEvent).
 *
 * The on-device AppClaudeCode subscribes to onWsClaudeEvent, which the WS
 * avatar worker also feeds (Phase 1). To keep the app transport-agnostic, we
 * translate claude_protocol C events into the same JSON shape the WS bridge
 * sends and re-emit via onWsClaudeEvent. The app therefore sees one uniform
 * event stream regardless of source (BLE NUS push from the Claude desktop app
 * or HTTP POST bridge from ai-agent-notify.sh).
 *
 * This file owns the bridge lifecycle: register the NUS RX -> parser pipeline
 * at boot, and register the parser -> JSON -> signal pipeline at boot.
 */
#include "claude_nus.h"
#include "claude_protocol.h"

#include <ArduinoJson.h>
#include <esp_log.h>
#include <hal/hal.h>
#include <memory>
#include <stdio.h>
#include <string>

namespace {
constexpr const char *TAG = "ClaudeBridge";

/* C-callback from claude_nus.c: a complete '\n'-stripped JSON line arrived.
 * Hand it off to the parser (which then fires the typed event callback). */
void nus_line_handler(const char *line, uint16_t /*conn_handle*/) {
    claude_protocol_parse_line(line);
}

/* Build the JSON payload that onWsClaudeEvent consumers expect, then emit.
 * The shape intentionally matches ai-agent-notify.sh::send_to_stackchan so the
 * AppClaudeCode state-derivation logic is identical for both paths:
 *   {
 *     "event": "stop"|"permission_request"|"notification",
 *     "msg":   "<short status text>",
 *     "tool_name": "<Bash|Edit|...>",  // present on permission_request
 *     "host":  "claude-desktop",       // constant marker for BLE source
 *     "cwd":   ""                       // BLE path has no cwd notion
 *   }
 *
 * We synthesize the event name from the heartbeat counters and the presence
 * of a prompt, mirroring how ai-agent-notify.sh derives its event from the
 * hook kind. */
void emit_to_app(const claude_event_t *event) {
    if (event->kind == CLAUDE_EVENT_HEARTBEAT) {
        JsonDocument doc;
        const auto &hb = event->heartbeat;
        if (hb.has_prompt) {
            doc["event"]    = "permission_request";
            doc["msg"]      = hb.prompt.tool ? hb.prompt.tool : "permission";
            doc["tool_name"] = hb.prompt.tool ? hb.prompt.tool : "";
        } else if (hb.state == CLAUDE_STATE_BUSY) {
            doc["event"] = "notification";
            doc["msg"]   = hb.msg ? hb.msg : "busy";
        } else if (hb.state == CLAUDE_STATE_ATTENTION) {
            doc["event"] = "permission_request";
            doc["msg"]   = "waiting";
        } else {
            /* IDLE or SLEEP: surface as a stop so the device UI clears. */
            doc["event"] = "stop";
            doc["msg"]   = hb.msg ? hb.msg : "idle";
        }
        doc["host"] = "claude-desktop";
        doc["cwd"]  = "";

        std::string out;
        serializeJson(doc, out);
        GetHAL().onWsClaudeEvent.emit(out);
    } else if (event->kind == CLAUDE_EVENT_TURN) {
        /* A completed assistant turn: surface the text as a notification so
         * the device shows what Claude just said. */
        JsonDocument doc;
        doc["event"] = "notification";
        doc["msg"]   = event->turn.text ? event->turn.text : "";
        doc["host"]  = "claude-desktop";
        doc["cwd"]   = "";
        std::string out;
        serializeJson(doc, out);
        GetHAL().onWsClaudeEvent.emit(out);
    } else if (event->kind == CLAUDE_EVENT_CMD) {
        /* Commands like "status" require a response. Phase 2 scope: just log,
         * full ack protocol (status response with battery/stats, owner name,
         * folder push transport) is Phase 4. */
        ESP_LOGD(TAG, "cmd received (Phase 4 scope): %s", event->cmd.cmd);
    } else if (event->kind == CLAUDE_EVENT_TIME) {
        ESP_LOGD(TAG, "time sync: epoch=%lld tz=%d",
                 (long long)event->time.epoch_seconds,
                 (int)event->time.tz_offset_seconds);
    }
}

/* C-callback from claude_protocol.c: a typed event was parsed. Forward to the
 * C++ emitter. user_data is unused for now (could carry a queue later). */
void protocol_event_handler(const claude_event_t *event, void * /*user_data*/) {
    emit_to_app(event);
}
}  // namespace

extern "C" {

/* Entry point called once during Hal::init() to wire the BLE NUS transport
 * and the wire protocol parser into the firmware's onWsClaudeEvent signal.
 * Idempotent (safe to call multiple times — the NUS service registration is
 * a no-op if already registered). */
void claude_bridge_init(void) {
    claude_nus_set_parser(nus_line_handler);
    claude_protocol_set_callback(protocol_event_handler, nullptr);
    ESP_LOGI(TAG, "Claude Code BLE bridge initialized");
}

}  // extern "C"

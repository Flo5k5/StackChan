/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_claude_code.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <ArduinoJson.h>
#include <smooth_lvgl.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

namespace {
constexpr const char* kTag = "ClaudeCode";

// Background and foreground colors tuned for the StackChan IPS panel.
// Dark theme, with one accent color per state for at-a-glance reading.
// lv_color_hex is not constexpr, so we wrap each color in an inline function
// (zero runtime cost, computed on first use).
inline lv_color_t ColorBg()     { return lv_color_hex(0x1E1E2E); }  // dark navy
inline lv_color_t ColorFg()     { return lv_color_hex(0xCDD6F4); }  // off-white
inline lv_color_t ColorAccent() { return lv_color_hex(0x89B4FA); }  // blue (idle/info)
inline lv_color_t ColorBusy()   { return lv_color_hex(0xF9E2AF); }  // yellow (busy)
inline lv_color_t ColorAttn()   { return lv_color_hex(0xF38BA8); }  // red (attention)
inline lv_color_t ColorMuted()  { return lv_color_hex(0x6C7086); }  // gray (sleep/labels)
}  // namespace

// Pimpl-backed state. All LVGL widgets and the cached payload live behind a
// single allocation so onClose can tear everything down deterministically.
struct AppClaudeCode::Impl {
    // UI widgets (created in onOpen under LvglLockGuard, destroyed in onClose).
    std::unique_ptr<Container> panel;
    std::unique_ptr<Label> title;
    std::unique_ptr<Label> state_label;
    std::unique_ptr<Label> message_label;
    std::unique_ptr<Label> meta_label;
    std::unique_ptr<Label> hint_label;

    // Connection handle for GetHAL().onWsClaudeEvent. We stash it to disconnect
    // cleanly in onClose (the Signal API clears all slots via .clear() — there
    // is no per-slot disconnect in uitk::Signal).
    bool subscribed = false;

    // Last received payload + derived display state. Guarded by data_mutex
    // because the WS callback fires from the WebSocketAvatar worker thread,
    // not the LVGL thread.
    std::mutex data_mutex;
    std::string last_event;        // raw event field ("stop", "permission_request", ...)
    std::string last_msg;          // derived message
    std::string last_tool;         // tool_name (for permission_request)
    std::string last_host;         // laptop hostname
    std::string last_cwd;          // session cwd
    bool dirty = false;            // true when a new payload arrived and UI must refresh
};

namespace {

// Map the laptop hook event to a 4-state display model mirroring the
// Anthropic claude-desktop-buddy state machine (sleep/idle/busy/attention).
// We don't have a real heartbeat yet (counters + entries will come in Phase 4
// via an enriched server payload); for now we derive the state from the event
// name forwarded by ai-agent-notify.sh.
enum class ClaudeState { Sleep, Idle, Busy, Attention };

struct StateInfo {
    ClaudeState state;
    const char* label;
    lv_color_t color;
};

StateInfo deriveState(const std::string& event) {
    if (event == "permission_request") {
        return {ClaudeState::Attention, "ATTENTION", ColorAttn()};
    }
    if (event == "stop" || event == "idle") {
        return {ClaudeState::Idle, "IDLE", ColorAccent()};
    }
    if (event == "notification") {
        return {ClaudeState::Busy, "BUSY", ColorBusy()};
    }
    // Unknown event: treat as idle but keep last state visible.
    return {ClaudeState::Idle, "IDLE", ColorAccent()};
}

}  // namespace

AppClaudeCode::AppClaudeCode() : _p(std::make_unique<Impl>()) {
    setAppInfo().name = "ClaudeCode";
    // TODO(phase4): setAppInfo().icon = (void*)&icon_claude_code;
}

AppClaudeCode::~AppClaudeCode() = default;

void AppClaudeCode::onCreate() {
    mclog::tagInfo(kTag, "on create");
}

void AppClaudeCode::onOpen() {
    mclog::tagInfo(kTag, "on open");

    // Network is shared with the rest of the firmware — the WebSocketAvatar
    // worker keeps the /stackChan/ws connection up. We only need to subscribe
    // to the ClaudeEvent signal. No need to startNetwork() ourselves as long
    // as another app (Avatar) already brought the link up; if not, the events
    // simply won't arrive until it does (graceful degradation).

    {
        LvglLockGuard lock;

        _p->panel = std::make_unique<Container>(lv_screen_active());
        _p->panel->setSize(320, 240);
        _p->panel->setAlign(LV_ALIGN_CENTER);
        _p->panel->setBgColor(ColorBg());
        _p->panel->setBgOpa(255);
        _p->panel->setPaddingAll(8);

        _p->title = std::make_unique<Label>(*_p->panel);
        _p->title->setText("Claude Code");
        _p->title->setTextFont(&lv_font_montserrat_16);
        _p->title->setTextColor(ColorAccent());
        _p->title->setAlign(LV_ALIGN_TOP_MID);

        _p->state_label = std::make_unique<Label>(*_p->panel);
        _p->state_label->setTextFont(&lv_font_montserrat_24);
        _p->state_label->setTextColor(ColorMuted());
        _p->state_label->setText("SLEEP");
        _p->state_label->setAlign(LV_ALIGN_TOP_MID);
        _p->state_label->setPos(0, 28);

        _p->message_label = std::make_unique<Label>(*_p->panel);
        _p->message_label->setTextFont(&lv_font_montserrat_14);
        _p->message_label->setTextColor(ColorFg());
        _p->message_label->setText("waiting for Claude...");
        _p->message_label->setWidth(300);
        _p->message_label->setLongMode(LV_LABEL_LONG_WRAP);
        _p->message_label->setAlign(LV_ALIGN_TOP_MID);
        _p->message_label->setPos(0, 70);

        _p->meta_label = std::make_unique<Label>(*_p->panel);
        _p->meta_label->setTextFont(&lv_font_montserrat_14);
        _p->meta_label->setTextColor(ColorMuted());
        _p->meta_label->setText("");
        _p->meta_label->setWidth(300);
        _p->meta_label->setLongMode(LV_LABEL_LONG_WRAP);
        _p->meta_label->setAlign(LV_ALIGN_TOP_MID);
        _p->meta_label->setPos(0, 150);

        _p->hint_label = std::make_unique<Label>(*_p->panel);
        _p->hint_label->setTextFont(&lv_font_montserrat_14);
        _p->hint_label->setTextColor(ColorMuted());
        _p->hint_label->setText("tap the power button to quit");
        _p->hint_label->setAlign(LV_ALIGN_BOTTOM_MID);
    }

    // Subscribe to ClaudeEvent from the WS avatar worker. Fire-and-forget:
    // we parse under data_mutex and set dirty=true; onRunning does the LVGL
    // refresh under LvglLockGuard.
    auto on_event = [this](std::string_view payload) {
        // Parse defensively: the hook script shape can drift, and we never
        // want a malformed payload to crash the device.
        JsonDocument doc;
        const auto err = deserializeJson(doc, payload.data(), payload.size());
        if (err) {
            mclog::tagWarn(kTag, "failed to parse ClaudeEvent payload: %s", err.c_str());
            return;
        }

        std::lock_guard<std::mutex> lock(_p->data_mutex);
        _p->last_event = doc["event"] | "unknown";
        _p->last_msg   = doc["msg"]   | "";
        _p->last_tool  = doc["tool_name"] | "";
        _p->last_host  = doc["host"] | "";
        _p->last_cwd   = doc["cwd"]  | "";
        _p->dirty = true;
    };
    GetHAL().onWsClaudeEvent.connect(on_event);
    _p->subscribed = true;

    mclog::tagInfo(kTag, "subscribed to onWsClaudeEvent");
}

void AppClaudeCode::onRunning() {
    // Refresh the UI only when a new payload arrived. Cheap check, no lock
    // held while doing LVGL work.
    if (!_p->dirty) {
        return;
    }

    std::string event, msg, tool, host, cwd;
    {
        std::lock_guard<std::mutex> lock(_p->data_mutex);
        if (!_p->dirty) return;
        _p->dirty = false;
        event.swap(_p->last_event);
        msg.swap(_p->last_msg);
        tool.swap(_p->last_tool);
        host.swap(_p->last_host);
        cwd.swap(_p->last_cwd);
    }

    const auto info = deriveState(event);

    LvglLockGuard lock;
    _p->state_label->setText(info.label);
    _p->state_label->setTextColor(info.color);

    // Build the message line: use msg if present, else synthesize from event.
    std::string display_msg = msg;
    if (display_msg.empty()) {
        if (event == "permission_request" && !tool.empty()) {
            display_msg = std::string("approve ") + tool + "?";
        } else if (event == "stop") {
            display_msg = "session finished";
        } else {
            display_msg = "(no message)";
        }
    }
    _p->message_label->setText(display_msg.c_str());

    // Meta line: host + cwd, kept short to fit the 320px width.
    std::string meta;
    if (!host.empty()) {
        meta += host;
    }
    if (!cwd.empty()) {
        // Keep only the last path segment to save horizontal space.
        auto pos = cwd.find_last_of('/');
        meta += "  ";
        meta += (pos == std::string::npos) ? cwd : cwd.substr(pos + 1);
    }
    _p->meta_label->setText(meta.c_str());
}

void AppClaudeCode::onClose() {
    mclog::tagInfo(kTag, "on close");

    // Disconnect first so no more callbacks fire while we tear down the UI.
    if (_p->subscribed) {
        GetHAL().onWsClaudeEvent.clear();
        _p->subscribed = false;
    }

    {
        LvglLockGuard lock;
        _p->hint_label.reset();
        _p->meta_label.reset();
        _p->message_label.reset();
        _p->state_label.reset();
        _p->title.reset();
        _p->panel.reset();
    }
}

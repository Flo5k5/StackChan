/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_claude_code.h"
#include "claude_stats.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <ArduinoJson.h>
#include <smooth_lvgl.hpp>
#include <board.h>
#include "hal/utils/secret_logic/secret_logic.h"
#include "stackchan/avatar/decorators/decorators.h"
#include "assets/claude_bufo/bufo_images.h"
#include "libs/gif/lv_gif.h"
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

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

// 4-state display model mirroring the Anthropic claude-desktop-buddy state
// machine. Declared early so the Impl struct can reference it.
enum class ClaudeState { Sleep, Idle, Busy, Attention, Celebrate };
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
    std::string last_session_id;   // Claude Code session id (used to route approve/deny back via HTTP)
    bool dirty = false;            // true when a new payload arrived and UI must refresh

    // Permission decision UI (Phase 3). Two LVGL buttons shown only when the
    // state is ATTENTION (permission_request received with a session_id).
    // Tapping a button POSTs to /stackChan/claude-permission and clears the
    // attention state.
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> btn_approve;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> btn_deny;
    bool attention_active = false;  // guards against double-send / stale buttons
    std::string pending_session_id;  // session_id captured on attention, used by the button handler

    // Phase 3.5 visual feedback: heart (approve) / angry (deny) decorator.
    // Held briefly so the LVGL object outlives its animation, then released.
    std::unique_ptr<stackchan::avatar::Decorator> decorator_holder;

    // Phase 4: animated character (bufo). The gif object is parented to the
    // panel and lives for the whole app lifetime; we swap its source when the
    // derived state changes. A celebrate burst overrides the state-driven
    // source for 3s on level-up.
    lv_obj_t* character_gif = nullptr;
    ClaudeState current_gif_state = ClaudeState::Sleep;  // tracks the last src set
    uint32_t last_tokens_today = 0;                       // for level-up detection
    int64_t celebrate_until_ms = 0;                       // 0 = no active celebrate burst

    // Phase 4 stat tracking: when ATTENTION started, so we can measure the
    // time-to-decide and feed recordApproval's velocity EMA.
    int64_t attention_started_ms = 0;
};

namespace {

// Map the laptop hook event to a 4-state display model mirroring the
// Anthropic claude-desktop-buddy state machine (sleep/idle/busy/attention).
// We don't have a real heartbeat yet (counters + entries will come in Phase 4
// via an enriched server payload); for now we derive the state from the event
// name forwarded by ai-agent-notify.sh.

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

// Map a derived state to the matching bufo GIF descriptor.
// Celebrate is handled separately as a transient overlay (see onRunning).
const lv_image_dsc_t* gifForState(ClaudeState state) {
    switch (state) {
        case ClaudeState::Sleep:     return &bufo_sleep;
        case ClaudeState::Idle:      return &bufo_idle;
        case ClaudeState::Busy:      return &bufo_busy;
        case ClaudeState::Attention: return &bufo_attention;
        default:                     return &bufo_idle;
    }
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

        // Phase 4: animated character (bufo, 96x100). Top-left corner, leaves
        // the right ~210px for the HUD. Created once; onRunning swaps the src.
        _p->character_gif = lv_gif_create(_p->panel->get());
        lv_gif_set_color_format(_p->character_gif, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(_p->character_gif, 6, 24);
        lv_gif_set_src(_p->character_gif, &bufo_sleep);
        _p->current_gif_state = ClaudeState::Sleep;

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
        _p->last_session_id = doc["session_id"] | "";
        // Phase 4: tokens_today is present only on heartbeat payloads (BLE
        // path) or enriched WS payloads. Missing -> 0 (no level-up check).
        _p->last_tokens_today = doc["tokens_today"] | 0;
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

    std::string event, msg, tool, host, cwd, session_id;
    uint32_t tokens_today = 0;
    {
        std::lock_guard<std::mutex> lock(_p->data_mutex);
        if (!_p->dirty) return;
        _p->dirty = false;
        event.swap(_p->last_event);
        msg.swap(_p->last_msg);
        tool.swap(_p->last_tool);
        host.swap(_p->last_host);
        cwd.swap(_p->last_cwd);
        session_id.swap(_p->last_session_id);
        tokens_today = _p->last_tokens_today;
    }

    // Phase 4: detect level-up before drawing so we can trigger a celebrate
    // burst even if the state itself didn't change. Non-blocking: only the
    // first crossing of each 50K boundary fires.
    if (tokens_today > 0 && app_claude_code::maybeLevelUp(tokens_today)) {
        _p->celebrate_until_ms = GetHAL().millis() + 3000;  // 3s burst
    }

    const auto info = deriveState(event);

    LvglLockGuard lock;
    _p->state_label->setText(info.label);
    _p->state_label->setTextColor(info.color);

    // Phase 4: swap the bufo GIF when the derived state changes, or when a
    // celebrate burst is active. We track the last state to avoid calling
    // lv_gif_set_src every frame (it resets the animation).
    const int64_t now_ms = GetHAL().millis();
    const bool celebrating = _p->celebrate_until_ms > 0 && now_ms < _p->celebrate_until_ms;
    if (celebrating && _p->current_gif_state != ClaudeState::Celebrate) {
        // Synthesize a pseudo-state so the tracker doesn't keep re-setting src.
        lv_gif_set_src(_p->character_gif, &bufo_celebrate);
        _p->current_gif_state = ClaudeState::Celebrate;
    } else if (!celebrating) {
        if (_p->celebrate_until_ms > 0 && now_ms >= _p->celebrate_until_ms) {
            _p->celebrate_until_ms = 0;  // burst expired
        }
        if (_p->current_gif_state != info.state) {
            lv_gif_set_src(_p->character_gif, gifForState(info.state));
            _p->current_gif_state = info.state;
        }
    }

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

    // Phase 3: show approve/deny buttons when a permission prompt is blocking
    // AND we have a session_id to route the decision back to the laptop hook.
    // Hide them in every other state. Buttons are (re)created on demand so
    // they don't intercept taps during sleep/idle/busy.
    const bool need_attention_ui =
        (info.state == ClaudeState::Attention) && !session_id.empty();
    if (need_attention_ui && !_p->attention_active) {
        _p->attention_active = true;
        _p->pending_session_id = session_id;
        _p->attention_started_ms = GetHAL().millis();  // for velocity EMA

        _p->btn_approve = std::make_unique<smooth_ui_toolkit::lvgl_cpp::Button>(*_p->panel);
        _p->btn_approve->setSize(130, 50);
        _p->btn_approve->setAlign(LV_ALIGN_BOTTOM_LEFT);
        _p->btn_approve->setPos(10, -40);
        _p->btn_approve->setBgColor(lv_color_hex(0xA6E3A1));  // green (Catppuccin)
        _p->btn_approve->label().setText("Approve");
        _p->btn_approve->label().setTextFont(&lv_font_montserrat_16);
        _p->btn_approve->label().setTextColor(lv_color_hex(0x1E1E2E));
        _p->btn_approve->onClick().connect([this]() { sendPermissionDecision("once"); });

        _p->btn_deny = std::make_unique<smooth_ui_toolkit::lvgl_cpp::Button>(*_p->panel);
        _p->btn_deny->setSize(130, 50);
        _p->btn_deny->setAlign(LV_ALIGN_BOTTOM_RIGHT);
        _p->btn_deny->setPos(-10, -40);
        _p->btn_deny->setBgColor(lv_color_hex(0xF38BA8));  // red (Catppuccin)
        _p->btn_deny->label().setText("Deny");
        _p->btn_deny->label().setTextFont(&lv_font_montserrat_16);
        _p->btn_deny->label().setTextColor(lv_color_hex(0x1E1E2E));
        _p->btn_deny->onClick().connect([this]() { sendPermissionDecision("deny"); });
    } else if (!need_attention_ui && _p->attention_active) {
        // Leaving attention state: tear down the buttons.
        _p->btn_deny.reset();
        _p->btn_approve.reset();
        _p->attention_active = false;
        _p->pending_session_id.clear();
    }
}

void AppClaudeCode::sendPermissionDecision(const std::string& decision) {
    // Snapshot the session id under the lock so the POST uses a consistent
    // value even if a new event arrives while we're still building the body.
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(_p->data_mutex);
        session_id = _p->pending_session_id;
    }
    if (session_id.empty()) {
        mclog::tagWarn(kTag, "permission decision requested but no session_id available");
        return;
    }

    // Phase 4: persist the decision into NVS-backed stats. Velocity is the
    // time elapsed between ATTENTION appearing and the user tapping Approve.
    // Denies don't track velocity. Run this before tearing down the buttons
    // so we still have attention_started_ms.
    {
        const int64_t decided_ms = GetHAL().millis();
        const uint32_t decision_time_s =
            _p->attention_started_ms > 0
                ? static_cast<uint32_t>((decided_ms - _p->attention_started_ms) / 1000)
                : 0;
        if (decision == "once") {
            app_claude_code::recordApproval(decision_time_s);
        } else {
            app_claude_code::recordDeny();
        }
    }

    // Tear down the buttons immediately so the user sees the tap registered.
    // The state will refresh on the next event from the hook (allow -> stop,
    // deny -> the laptop exits the turn so we'll see a stop or nothing).
    {
        LvglLockGuard lock;
        _p->btn_deny.reset();
        _p->btn_approve.reset();
        _p->attention_active = false;
        _p->pending_session_id.clear();

        // Phase 3.5: spawn a short-lived decorator as visual confirmation.
        // HeartDecorator (approve) or AngryDecorator (deny), self-destroy
        // after 800ms via their built-in lifetime mechanism.
        if (decision == "once") {
            _p->decorator_holder = std::make_unique<stackchan::avatar::HeartDecorator>(
                lv_screen_active(), /*destroyAfterMs=*/800, /*animationIntervalMs=*/300);
        } else {
            _p->decorator_holder = std::make_unique<stackchan::avatar::AngryDecorator>(
                lv_screen_active(), /*destroyAfterMs=*/800, /*animationIntervalMs=*/300);
        }
        // Release the holder after 1s — the decorator self-destroys its LVGL
        // object, we just need to drop our unique_ptr to avoid double-free.
        // Schedule via a detached thread (no FreeRTOS timer plumbing here).
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            LvglLockGuard lock;
            _p->decorator_holder.reset();
        }).detach();
    }

    // POST /stackChan/claude-permission {session_id, decision} in a worker
    // thread so we don't block the LVGL task. The hook script on the laptop
    // is long-polling /claude-permission/poll and will pick this up.
    std::thread([this, session_id, decision]() {
        try {
            auto& board  = Board::GetInstance();
            auto network = board.GetNetwork();
            auto http    = network->CreateHttp(0);
            if (!http) {
                mclog::tagWarn(kTag, "permission POST: CreateHttp failed");
                return;
            }
            const auto token = secret_logic::generate_auth_token();
            const auto url   = secret_logic::get_server_url() + "/stackChan/claude-permission";

            // Tiny JSON body — cJSON is already available in the firmware.
            std::string body = "{\"session_id\":\"" + session_id +
                               "\",\"decision\":\"" + decision + "\"}";
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Authorization", token.c_str());
            http->SetContent(std::move(body));
            if (!http->Open("POST", url)) {
                mclog::tagWarn(kTag, "permission POST: Open failed");
                return;
            }
            const int status = http->GetStatusCode();
            http->Close();
            mclog::tagInfo(kTag, "permission POST %s -> HTTP %d (session %s)",
                           decision.c_str(), status, session_id.c_str());
        } catch (...) {
            // Network failures are non-fatal — the laptop hook has its own
            // timeout and will fall back to the native prompt.
        }
    }).detach();
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
        // Phase 4 cleanup: GIF character (raw lv_obj_t*, manual delete). Must
        // happen before the panel is reset since the gif is parented to it.
        if (_p->character_gif) {
            lv_obj_del(_p->character_gif);
            _p->character_gif = nullptr;
        }
        // Phase 3 cleanup: permission UI + decorator (must be destroyed before
        // panel since they were parented to it / the active screen).
        _p->decorator_holder.reset();
        _p->btn_deny.reset();
        _p->btn_approve.reset();
        _p->hint_label.reset();
        _p->meta_label.reset();
        _p->message_label.reset();
        _p->state_label.reset();
        _p->title.reset();
        _p->panel.reset();
    }
}

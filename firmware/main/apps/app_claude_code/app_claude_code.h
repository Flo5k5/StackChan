/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * Claude Code companion app for StackChan.
 *
 * Renders the live state of a remote Claude Code session running on the
 * laptop, fed by the laptop hook → server bridge → WS 0x1B path
 * (see server/internal/web_socket/claude_event.go and
 * ai-agent-notify.sh::send_to_stackchan).
 *
 * Phase 1 (this file): display-only MVP. Subscribes to GetHAL().onWsClaudeEvent,
 * parses the JSON payload, derives a UI state (sleep/idle/busy/attention) and
 * shows counters + last message. No buttons interaction yet (Phase 3), no
 * character animation yet (Phase 4).
 */
#pragma once
#include <mooncake.h>

class AppClaudeCode : public mooncake::AppAbility {
public:
    AppClaudeCode();
    ~AppClaudeCode() override;  // defined in .cpp where Impl is complete

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct Impl;
    std::unique_ptr<Impl> _p;
};

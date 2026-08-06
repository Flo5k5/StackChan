/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * Persistent stats for the Claude Code app, backed by ESP-IDF NVS.
 *
 * Mirrors the Anthropic claude-desktop-buddy stats fields: lifetime
 * approvals, denies, average approval velocity (seconds), nap count, and
 * the current "level" (incremented every 50,000 tokens today). Survives
 * reboots; cheap reads (one nvs_get_u32 per field).
 *
 * Thread-safety: the NVS handle is opened per-call (no global state), so
 * the helpers are safe to call from any task. NVS internally serializes
 * writes with its own mutex.
 */
#pragma once

#include <cstdint>
#include <string>

namespace app_claude_code {

// Tokens required to gain a level. Matches the buddy reference spec
// (REFERENCE.md celebrate rule). Today-counter resets at local midnight on
// the laptop side, so the level is a session/day proxy for the device.
constexpr uint32_t kTokensPerLevel = 50000;

struct ClaudeStats {
    uint32_t approvals = 0;   // lifetime count of "once" decisions sent
    uint32_t denies = 0;      // lifetime count of "deny" decisions sent
    uint32_t velocity_s = 0;  // rolling average time-to-approve, in seconds
    uint32_t naps = 0;        // device-side counter (e.g.ATTENTION that timed out)
    uint32_t level = 0;       // current level (tokens_today / kTokensPerLevel)
};

// Load every stat from NVS. Missing keys default to 0 (first boot).
ClaudeStats loadStats();

// Persist a stats snapshot. Call after every approve/deny/level-up.
void saveStats(const ClaudeStats& stats);

// Convenience: record an approval and update velocity. `decision_time_s` is
// the time elapsed between the ATTENTION state appearing and the user
// tapping Approve (rolling average, simple EMA with alpha=0.3).
void recordApproval(uint32_t decision_time_s);

// Convenience: record a deny.
void recordDeny();

// Convenience: bump the level if tokens crossed a new kTokensPerLevel
// boundary. Returns true if the level increased (caller can trigger the
// celebrate animation). `tokens_today` comes from the heartbeat payload.
bool maybeLevelUp(uint32_t tokens_today);

}  // namespace app_claude_code

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "claude_stats.h"

#include <esp_log.h>
#include <nvs_flash.h>

namespace app_claude_code {
namespace {

constexpr const char* kTag = "ClaudeStats";
constexpr const char* kNvsNamespace = "claude_cc";  // <16 chars, NVS limit
constexpr const char* kKeyApprovals = "appr";
constexpr const char* kKeyDenies    = "deny";
constexpr const char* kKeyVelocity  = "vel";
constexpr const char* kKeyNaps      = "nap";
constexpr const char* kKeyLevel     = "lvl";

// EMA smoothing factor for the rolling velocity average. 0.3 keeps the
// average stable while still reacting to recent changes.
constexpr float kVelocityAlpha = 0.3f;

uint32_t readU32(nvs_handle_t h, const char* key) {
    uint32_t v = 0;
    if (nvs_get_u32(h, key, &v) != ESP_OK) {
        return 0;  // missing key = first boot, default to 0
    }
    return v;
}

void writeU32(nvs_handle_t h, const char* key, uint32_t v) {
    if (nvs_set_u32(h, key, v) != ESP_OK) {
        ESP_LOGW(kTag, "nvs_set_u32(%s) failed", key);
    }
}

}  // namespace

ClaudeStats loadStats() {
    ClaudeStats s;
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
        return s;  // namespace doesn't exist yet, all defaults to 0
    }
    s.approvals  = readU32(h, kKeyApprovals);
    s.denies     = readU32(h, kKeyDenies);
    s.velocity_s = readU32(h, kKeyVelocity);
    s.naps       = readU32(h, kKeyNaps);
    s.level      = readU32(h, kKeyLevel);
    nvs_close(h);
    return s;
}

void saveStats(const ClaudeStats& s) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(kTag, "nvs_open(readwrite) failed, stats not persisted");
        return;
    }
    writeU32(h, kKeyApprovals, s.approvals);
    writeU32(h, kKeyDenies,    s.denies);
    writeU32(h, kKeyVelocity,  s.velocity_s);
    writeU32(h, kKeyNaps,      s.naps);
    writeU32(h, kKeyLevel,     s.level);
    nvs_commit(h);
    nvs_close(h);
}

void recordApproval(uint32_t decision_time_s) {
    ClaudeStats s = loadStats();
    s.approvals++;
    // Exponential moving average — stable but reactive.
    if (s.velocity_s == 0) {
        s.velocity_s = decision_time_s;
    } else {
        s.velocity_s = static_cast<uint32_t>(
            kVelocityAlpha * decision_time_s + (1.0f - kVelocityAlpha) * s.velocity_s);
    }
    saveStats(s);
    ESP_LOGI(kTag, "approval recorded: total=%u vel=%us",
             static_cast<unsigned>(s.approvals),
             static_cast<unsigned>(s.velocity_s));
}

void recordDeny() {
    ClaudeStats s = loadStats();
    s.denies++;
    saveStats(s);
    ESP_LOGI(kTag, "deny recorded: total=%u", static_cast<unsigned>(s.denies));
}

bool maybeLevelUp(uint32_t tokens_today) {
    ClaudeStats s = loadStats();
    uint32_t new_level = tokens_today / kTokensPerLevel;
    if (new_level > s.level) {
        s.level = new_level;
        saveStats(s);
        ESP_LOGI(kTag, "LEVEL UP! now level %u (tokens_today=%u)",
                 static_cast<unsigned>(s.level),
                 static_cast<unsigned>(tokens_today));
        return true;
    }
    return false;
}

}  // namespace app_claude_code

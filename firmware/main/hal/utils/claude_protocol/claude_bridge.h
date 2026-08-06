/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Wire the Claude Code BLE NUS transport and the wire protocol parser into
 * the firmware's onWsClaudeEvent signal. Call once at boot after the BLE
 * peripheral is initialized (Hal::ble_init). See claude_bridge.cpp. */
void claude_bridge_init(void);

#ifdef __cplusplus
}
#endif

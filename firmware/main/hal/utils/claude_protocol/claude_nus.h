/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration to avoid pulling NimBLE headers into this public face. */
struct ble_gatt_svc_def;

/* Maximum line size for the NUS newline-delimited JSON stream. Matches the
 * claude-desktop-buddy reference cap (~4 KB per event). Lines larger than this
 * are dropped (the desktop retries with a fresh event). */
#define CLAUDE_NUS_MAX_LINE 4096

/* Parser hook called for every complete JSON line received on the RX
 * characteristic. The line is NULL-terminated and does NOT include the
 * trailing '\n'. `conn_handle` is the NimBLE connection handle of the peer
 * that sent the line (useful if the firmware needs to send a response back).
 *
 * Called from the NimBLE host task — must be non-blocking. Typical impl:
 * copy into a FreeRTOS queue and let a parser task do the heavy lifting. */
typedef void (*claude_nus_line_cb_t)(const char *line, uint16_t conn_handle);

/* Register the parser hook. Call once at boot, before ble_gatts_start(). */
void claude_nus_set_parser(claude_nus_line_cb_t cb);

/* Return the NUS service definition. Merge the result into the existing
 * gatt_svr_svcs[] table in gatt_svr.c so a single ble_gatts_count + start
 * call registers both the StackChan iOS service and this Claude service. */
const struct ble_gatt_svc_def *claude_nus_svc_def(void);

/* Send a JSON line to the connected Claude desktop via the TX characteristic
 * notification. `json_line` does NOT need to include a trailing newline — the
 * buddy desktop tolerates both, and our internal protocol uses the line as-is.
 *
 * Returns 0 on success, a BLE_ATT_ERR_* code on failure. */
int claude_nus_send(const char *json_line, size_t len);

#ifdef __cplusplus
}
#endif

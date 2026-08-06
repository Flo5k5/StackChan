/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * Claude Code Nordic UART Service (NUS) for the StackChan BLE peripheral.
 *
 * Mirrors the wire transport of github.com/anthropics/claude-desktop-buddy so
 * the official desktop app (Developer Mode -> Hardware Buddy -> Connect) can
 * pair to the StackChan and push heartbeat events without any custom bridge.
 *
 * Service: 6e400001-b5a3-f393-e0a9-e50e24dcca9e
 * RX char (desktop -> device, write):     6e400002-...
 * TX char (device -> desktop, notify):    6e400003-...
 *
 * Framing: newline-delimited JSON, one object per line. Events larger than
 * CLAUDE_NUS_MAX_LINE bytes are dropped (matches the buddy reference cap of
 * ~4 KB). The RX callback accumulates bytes until it sees a '\n', then hands
 * the line verbatim to the parser hook registered via claude_nus_set_parser.
 *
 * Multi-connection note: NimBLE's ble_gatts_chr_updated broadcasts to every
 * subscribed peer, so the existing StackChan iOS pairing service and this NUS
 * service coexist without cross-talk — each service has its own value handle,
 * and a peer only receives notifications for characteristics it subscribed to.
 * We don't need to refactor the global g_conn_handle.
 */
#include "claude_nus.h"

#include <stdio.h>
#include <string.h>
#include <esp_log.h>

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

static const char *TAG = "ClaudeNUS";

/*** Nordic UART Service UUIDs (standard Bluefruit) ***/
/* 6e400001-b5a3-f393-e0a9-e50e24dcca9e */
static const ble_uuid128_t claude_nus_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
/* 6e400002-... (RX, write) */
static const ble_uuid128_t claude_nus_chr_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
/* 6e400003-... (TX, notify) */
static const ble_uuid128_t claude_nus_chr_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

/* Value handles populated by NimBLE when the service is registered. */
static uint16_t claude_nus_rx_handle = 0;
static uint16_t claude_nus_tx_handle = 0;

/* Parser hook: set once by the firmware at boot (claude_protocol_register).
 * Receives one complete JSON line at a time, no trailing '\n'. NULL-terminated. */
static claude_nus_line_cb_t g_parser_cb = NULL;

/* Receiving line buffer. The desktop pushes bytes in MTU-sized chunks; we
 * accumulate until '\n'. Single peer assumption (Claude Code desktop), so a
 * single static buffer is enough — if multiple laptops ever connect in
 * parallel we'd need per-peer buffers, but that's not the buddy contract. */
static char g_rx_buf[CLAUDE_NUS_MAX_LINE];
static size_t g_rx_len = 0;

/* Last TX value (for the read characteristic). Stays small (permission
 * decisions, keepalives). */
static char g_tx_buf[CLAUDE_NUS_MAX_LINE];
static size_t g_tx_len = 0;

void claude_nus_set_parser(claude_nus_line_cb_t cb) {
    g_parser_cb = cb;
    ESP_LOGI(TAG, "parser hook registered");
}

static int claude_nus_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
            /* Desktop reads the TX char to discover the device's last sent
             * value (mostly diagnostic; real-time push uses notifications). */
            if (attr_handle == claude_nus_tx_handle) {
                int rc = os_mbuf_append(ctxt->om, g_tx_buf, g_tx_len);
                return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            return 0;

        case BLE_GATT_ACCESS_OP_WRITE_CHR:
            if (attr_handle == claude_nus_rx_handle) {
                /* Accumulate incoming bytes into the line buffer. The desktop
                 * frames with '\n', so we scan for it and dispatch each
                 * complete line to the parser. */
                uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
                if (len == 0) {
                    return 0;
                }
                /* Drop data that would overflow the buffer. The reference
                 * buddy caps events at ~4 KB; we mirror that contract here. */
                if (g_rx_len + len >= CLAUDE_NUS_MAX_LINE) {
                    ESP_LOGW(TAG, "RX overflow (%d+%d >= %d), dropping line",
                             (int)g_rx_len, (int)len, CLAUDE_NUS_MAX_LINE);
                    g_rx_len = 0;
                    return BLE_ATT_ERR_INSUFFICIENT_RES;
                }
                uint16_t copied = os_mbuf_copydata(ctxt->om, 0, len, g_rx_buf + g_rx_len);
                if (copied != len) {
                    ESP_LOGW(TAG, "os_mbuf_copydata partial: %d/%d", copied, len);
                    return BLE_ATT_ERR_INSUFFICIENT_RES;
                }
                g_rx_len += len;
                g_rx_buf[g_rx_len] = '\0';

                /* Dispatch every complete line ('\n'-terminated). Multiple
                 * JSON objects may arrive in a single write. */
                size_t scan = 0;
                for (size_t i = 0; i < g_rx_len; i++) {
                    if (g_rx_buf[i] == '\n') {
                        g_rx_buf[i] = '\0';
                        if (g_parser_cb && i > scan) {
                            g_parser_cb(&g_rx_buf[scan], conn_handle);
                        }
                        scan = i + 1;
                    }
                }
                /* Shift the remainder to the start of the buffer. */
                if (scan > 0 && scan < g_rx_len) {
                    memmove(g_rx_buf, g_rx_buf + scan, g_rx_len - scan);
                    g_rx_len -= scan;
                    g_rx_buf[g_rx_len] = '\0';
                } else if (scan >= g_rx_len) {
                    g_rx_len = 0;
                }
                return 0;
            }
            return 0;

        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

/* Service definition registered alongside the existing StackChan services.
 * Returned by claude_nus_svc_def() so the caller (gatt_svr) can merge it into
 * its gatt_svr_svcs[] table. */
static struct ble_gatt_svc_def claude_nus_svc[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &claude_nus_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid       = &claude_nus_chr_rx_uuid.u,
                    .access_cb  = claude_nus_access,
                    .flags      = BLE_GATT_CHR_F_WRITE,
                    .val_handle = &claude_nus_rx_handle,
                },
                {
                    .uuid       = &claude_nus_chr_tx_uuid.u,
                    .access_cb  = claude_nus_access,
                    .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &claude_nus_tx_handle,
                },
                {
                    0,
                },
            },
    },
    {
        0,
    },
};

const struct ble_gatt_svc_def *claude_nus_svc_def(void) {
    return claude_nus_svc;
}

int claude_nus_send(const char *json_line, size_t len) {
    if (!json_line || len == 0 || len >= CLAUDE_NUS_MAX_LINE) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    /* Cache for read characteristic. */
    memcpy(g_tx_buf, json_line, len);
    g_tx_buf[len] = '\0';
    g_tx_len = len;

    /* NimBLE broadcasts the notification to every subscribed peer. We don't
     * need to track which conn_handle is the Claude desktop — only subscribers
     * of the TX char receive it, which excludes the iOS app (different svc). */
    ble_gatts_chr_updated(claude_nus_tx_handle);
    ESP_LOGD(TAG, "TX notify (%d bytes): %s", (int)len, json_line);
    return 0;
}

/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package web_socket

import (
	"context"

	"github.com/gogf/gf/v2/frame/g"
	"stackChan/internal/model"
)

var claudeEventLogger = g.Log()

// BroadcastClaudeEvent pushes a Claude Code status JSON payload to every
// currently connected StackChan device, using the ClaudeEvent (0x1B) wire type.
//
// The payload format mirrors the Anthropic claude-desktop-buddy heartbeat
// (REFERENCE.md): a JSON object with counters (total/running/waiting/tokens/
// tokens_today), a list of recent entries, and an optional blocking prompt.
// The on-device app derives its UI state from these fields.
//
// payload must be the raw JSON bytes (no framing); the function prepends the
// 5-byte binary protocol header (1 byte type + 4 bytes big-endian length),
// matching how the rest of web_socket.go frames outgoing binary messages.
func BroadcastClaudeEvent(ctx context.Context, payload []byte) {
	frame := make([]byte, 1+4+len(payload))
	frame[0] = ClaudeEvent
	length := uint32(len(payload))
	frame[1] = byte(length >> 24)
	frame[2] = byte(length >> 16)
	frame[3] = byte(length >> 8)
	frame[4] = byte(length)
	copy(frame[5:], payload)

	binaryType := int(2) // websocket.BinaryMessage
	sent := 0
	stackChanClientPool.Range(func(key, value any) bool {
		client, ok := value.(*model.StackChanClient)
		if !ok {
			return true
		}
		// Non-blocking send: clients with a full send channel are skipped,
		// same contract as stackChanSendMessage (web_socket.go:784).
		stackChanSendMessage(ctx, client, &binaryType, &frame)
		sent++
		return true
	})
	claudeEventLogger.Infof(ctx, "BroadcastClaudeEvent: delivered to %d StackChan client(s), payload=%d bytes", sent, len(payload))
}

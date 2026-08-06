/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package web_socket

import (
	"github.com/gogf/gf/v2/frame/g"
	"github.com/gogf/gf/v2/net/ghttp"
)

// ClaudeEventHandler is the HTTP endpoint mounted at POST /stackChan/claude-event.
//
// It receives the Claude Code hook payload forwarded by ai-agent-notify.sh on
// the laptop and broadcasts it to every connected StackChan device via the
// ClaudeEvent (0x1B) WebSocket frame. The on-device app then renders the live
// session state (sleep/idle/busy/attention/celebrate).
//
// Auth: reuses the same TokenAuthMiddleware as the rest of the /stackChan
// routes (configured via group.Middleware in cmd.go). The hook script passes
// `Authorization: hi-stack-chan` by default.
//
// The body is forwarded as-is (no schema validation beyond JSON well-formedness):
// the device app is tolerant of extra fields and the wire format is documented
// in firmware/main/hal/utils/claude_protocol/README.md.
func ClaudeEventHandler(r *ghttp.Request) {
	ctx := r.Context()

	body := r.GetBody()
	if len(body) == 0 {
		r.Response.WriteJsonExit(g.Map{
			"code":    400,
			"message": "empty body",
		})
		return
	}

	// Forward the raw JSON payload to StackChan devices via the ClaudeEvent
	// (0x1B) WS frame. The hook script is the source of truth for the schema;
	// we don't reshape it server-side to keep the device app decoupled from
	// the server's Go types.
	BroadcastClaudeEvent(ctx, body)

	r.Response.WriteJsonExit(g.Map{
		"code":    0,
		"message": "OK",
	})
}

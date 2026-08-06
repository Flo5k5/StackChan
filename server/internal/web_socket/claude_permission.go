/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package web_socket

import (
	"context"
	"sync"
	"time"

	"github.com/gogf/gf/v2/frame/g"
	"github.com/gogf/gf/v2/net/ghttp"
)

// Permission decision bridge: lets the on-device AppClaudeCode answer a Claude
// Code PreToolUse permission prompt via two HTTP endpoints, mirroring the
// ai-agent-notify.sh flow used to push the original event.
//
// Flow:
//   1. Laptop hook (PreToolUse, synchronous) POSTs the event via
//      /stackChan/claude-event (existing Phase 0 bridge) and then blocks on
//      GET /stackChan/claude-permission/poll?session_id=...&timeout=60.
//   2. User taps Approve/Deny on the device -> device POSTs the decision via
//      /stackChan/claude-permission {session_id, decision:"once"|"deny"}.
//   3. The poll handler wakes up and returns the decision; the hook exits with
//      the matching code (allow/deny) so Claude Code resumes or aborts.
//
// The map is keyed by Claude Code session_id (the only stable identifier
// available on the hook payload — prompt.id only exists in the BLE heartbeat
// path). Entries TTL out after permissionDecisionTTL to avoid leaks when the
// device never answers.

var (
	permissionMu     sync.Mutex
	permissionChans  = make(map[string]chan string)
	permissionLogger = g.Log()
)

const (
	permissionDecisionTTL = 2 * time.Minute
	pollDefaultTimeout    = 60 // seconds
)

// ClaudePermissionPostHandler receives a decision from a StackChan device.
//
// Body: {"session_id":"...", "decision":"once"|"deny"}
// Auth: reuses the TokenAuthMiddleware mounted on the /stackChan group.
func ClaudePermissionPostHandler(r *ghttp.Request) {
	ctx := r.Context()

	sessionId := r.Get("session_id").String()
	decision := r.Get("decision").String()
	if sessionId == "" || decision == "" {
		r.Response.WriteJsonExit(g.Map{
			"code":    400,
			"message": "session_id and decision are required",
		})
		return
	}
	if decision != "once" && decision != "deny" {
		r.Response.WriteJsonExit(g.Map{
			"code":    400,
			"message": "decision must be 'once' or 'deny'",
		})
		return
	}

	permissionMu.Lock()
	ch, ok := permissionChans[sessionId]
	if !ok {
		// No one is polling: create a fresh channel and TTL it out so a late
		// device response doesn't leak forever. The laptop polls *after* the
		// event POST, so this race (device answers before poll) is rare but
		// possible; the buffered channel preserves the decision.
		ch = make(chan string, 1)
		permissionChans[sessionId] = ch
	}
	permissionMu.Unlock()

	select {
	case ch <- decision:
		permissionLogger.Infof(ctx, "ClaudePermission: decision '%s' queued for session %s", decision, sessionId)
		r.Response.WriteJsonExit(g.Map{
			"code":    0,
			"message": "OK",
		})
	default:
		// Channel already has a pending decision (duplicate device tap).
		permissionLogger.Warningf(ctx, "ClaudePermission: duplicate decision for session %s, ignoring", sessionId)
		r.Response.WriteJsonExit(g.Map{
			"code":    409,
			"message": "decision already pending for this session",
		})
	}
}

// ClaudePermissionPollHandler blocks until a decision arrives or the timeout
// expires. Called by the laptop hook script right after it POSTed the
// permission_request event.
//
// Query params: session_id (required), timeout (seconds, default 60).
// Returns: 200 {"decision":"once"|"deny"} on decision, 408 on timeout.
func ClaudePermissionPollHandler(r *ghttp.Request) {
	ctx := r.Context()

	sessionId := r.Get("session_id").String()
	if sessionId == "" {
		r.Response.WriteJsonExit(g.Map{
			"code":    400,
			"message": "session_id is required",
		})
		return
	}
	timeoutSec := r.Get("timeout").Int()
	if timeoutSec <= 0 {
		timeoutSec = pollDefaultTimeout
	}

	permissionMu.Lock()
	ch, ok := permissionChans[sessionId]
	if !ok {
		ch = make(chan string, 1)
		permissionChans[sessionId] = ch
	}
	permissionMu.Unlock()

	// Schedule cleanup so abandoned sessions don't leak.
	go func() {
		ttlCtx, cancel := context.WithTimeout(context.Background(), permissionDecisionTTL)
		defer cancel()
		<-ttlCtx.Done()
		permissionMu.Lock()
		delete(permissionChans, sessionId)
		permissionMu.Unlock()
	}()

	select {
	case decision := <-ch:
		permissionMu.Lock()
		delete(permissionChans, sessionId)
		permissionMu.Unlock()
		permissionLogger.Infof(ctx, "ClaudePermission: poll returning '%s' for session %s", decision, sessionId)
		r.Response.WriteJsonExit(g.Map{
			"code":     0,
			"decision": decision,
		})
	case <-time.After(time.Duration(timeoutSec) * time.Second):
		permissionMu.Lock()
		delete(permissionChans, sessionId)
		permissionMu.Unlock()
		permissionLogger.Infof(ctx, "ClaudePermission: poll timeout for session %s after %ds", sessionId, timeoutSec)
		r.Response.WriteJsonExit(g.Map{
			"code":    408,
			"message": "decision timeout",
		})
	}
}

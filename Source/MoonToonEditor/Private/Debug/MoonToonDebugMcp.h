// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The MoonToon debugger's MCP server: MCP "Streamable HTTP" (JSON-RPC 2.0 over HTTP POST, plain JSON
 * responses, no SSE stream) on the engine's HTTPServer module, inside the editor.
 *
 *   endpoint  http://127.0.0.1:<port>/mcp   (port: Editor Preferences > MoonToon Debugger > Mcp Port)
 *   auth      Authorization: Bearer <token>; the token is in <Project>/Saved/MoonToonDebug/mcp_token.txt
 *             (created on the first start, kept across restarts, MoonToon.Debug.Mcp.NewToken replaces it)
 *   start     manually: MoonToon.Debug.Mcp.Start (console), the debugger panel's MCP button, or
 *             unreal.MoonToonDebugLibrary.start_mcp_server(); MoonToon.Debug.Mcp.Stop / .Status
 *
 * The tools are the function library's (UMoonToonDebugLibrary), so they return the same JSON as the panel
 * and UnrealBridge. Requests run one at a time on the game thread, on the tick after they arrive (a tool may
 * draw the viewport and wait for the GPU). Only loopback peers are served; a request with a non-local
 * Origin header is refused.
 */
namespace MoonToonDebugMcp
{
	/** Starts listening (idempotent). False with a reason when the port cannot be bound. */
	bool Start(FString* OutError = nullptr);
	/** Unbinds the /mcp route. The engine's HTTP listener keeps the port until the editor exits (it answers 404). */
	void Stop();
	bool IsRunning();
	int32 GetPort();
	FString GetUrl();
	FString GetTokenPath();
	/** Writes a new random token (clients must pick it up again). */
	void RegenerateToken();
	/** {"running", "url", "token_path", "requests", "last_error", ...} */
	FString GetStatusJson();
}

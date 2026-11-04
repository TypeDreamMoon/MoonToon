# Claude Code headersHelper for the MoonToon debugger's MCP server (inside the DevTest editor).
# Prints {"Authorization":"Bearer <token>"} from <DevTest>/Saved/MoonToonDebug/mcp_token.txt,
# which the editor writes the first time the server starts (MoonToon.Debug.Mcp.Start).
# No token file yet -> prints {} (the server then answers 403 until it has been started once).
param(
    [string]$TokenFile = (Join-Path $PSScriptRoot '..\..\..\..\Saved\MoonToonDebug\mcp_token.txt')
)

$ErrorActionPreference = 'SilentlyContinue'
if (Test-Path -LiteralPath $TokenFile) {
    $token = (Get-Content -Raw -LiteralPath $TokenFile).Trim()
    if ($token) {
        @{ Authorization = "Bearer $token" } | ConvertTo-Json -Compress
        exit 0
    }
}
'{}'
exit 0

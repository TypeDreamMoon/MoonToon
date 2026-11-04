# MoonToon 调试器的 MCP 服务

服务在 DevTest 编辑器里（插件 `MoonToonEditor`，`Private/Debug/MoonToonDebugMcp.cpp`），用 MCP Streamable HTTP 协议。

- 地址：`http://127.0.0.1:47651/mcp`。只监听本机回环地址。端口在 编辑器偏好设置 > MoonToon Debugger > Mcp Port 里改，改了要同步改下面的 Claude Code 配置。
- 认证：`Authorization: Bearer <token>`，token 在 `DevTest/Saved/MoonToonDebug/mcp_token.txt`。第一次启动时生成，之后每次重启都沿用。`MoonToon.Debug.Mcp.NewToken` 会重新生成。
- 工具：和面板、UnrealBridge 函数库是同一套，返回同样的 JSON。工具有 `editor_status`、`list_debug_views`、`set_debug_view`、`get_debug_view`、`get_pixel`、`get_region_stats`、`capture`、`list_captures`、`read_capture_meta`、`diff_captures`、`list_toon_actors`、`dump_constant_table`、`dump_profiles`、`dump_ramp_atlas`、`dump_style`、`validate`。

## 启动（手动）

以下三种方式任选一种：

- 面板 "MoonToon 调试器" 工具栏上的 **MCP ○** 按钮，点一下变成 **MCP ● :47651**；
- 控制台输入 `MoonToon.Debug.Mcp.Start`（停止是 `MoonToon.Debug.Mcp.Stop`，查看状态是 `MoonToon.Debug.Mcp.Status`）；
- python：`unreal.MoonToonDebugLibrary.start_mcp_server()`。

停止只是解绑路由。引擎的 HTTP 监听没有按端口关闭的接口，所以端口要到编辑器退出才释放，在那之前访问会得到 404。

## Claude Code 配置

推荐放在 user scope，这样不管在哪个目录开会话都能用。token 用 headersHelper 从文件读，所以 token 不会写进配置：

```bash
claude mcp add-json moontoon-debug "{\"type\":\"http\",\"url\":\"http://127.0.0.1:47651/mcp\",\"headersHelper\":\"powershell -NoProfile -ExecutionPolicy Bypass -File I:/UnrealProject_Moon/DEV_58/DevTest/Plugins/MoonToon/Tools/MoonToonDebugMCP/mcp_headers.ps1\"}" --scope user
```

也可以手写 JSON 配置，user scope 写进 `~/.claude.json` 的 `mcpServers`，project scope 写进项目的 `.mcp.json`：

```json
"moontoon-debug": {
  "type": "http",
  "url": "http://127.0.0.1:47651/mcp",
  "headersHelper": "powershell -NoProfile -ExecutionPolicy Bypass -File I:/UnrealProject_Moon/DEV_58/DevTest/Plugins/MoonToon/Tools/MoonToonDebugMCP/mcp_headers.ps1"
}
```

注意：

- 必须写 `"type": "http"`。只有 `url` 没有 `type` 会被当成 stdio 服务器。
- 不要用 `"headers": {"Authorization": "Bearer ${MOONTOON_MCP_TOKEN}"}`。名字里带 TOKEN 的环境变量可能被 Claude Code 当成凭据读成空值。
- headersHelper 每次连接时运行一次。工具调用收到 403 时会重新运行它并重试一次，所以 token 换了也不用重启会话。
- project scope 的 `.mcp.json`：第一次用要在审批框里批准，而且只在这个目录被信任以后才会运行 headersHelper。
- Windows 上 headersHelper 用哪种 shell 执行，文档里没有写。上面的 `powershell -File` 写法还没有在本机实测过，第一次接入时用 `claude --debug` 看一下。

## 编辑器没开时

Claude Code 在会话开始时连接 MCP 服务器。连不上的话会重试 3 次，然后在 `/mcp` 里把它标成 failed，这个服务器的工具在本会话里就不出现。代价不大的补救办法：编辑器开好、执行 `MoonToon.Debug.Mcp.Start` 之后，在 `/mcp` 里选 moontoon-debug → **Reconnect**（或者 `/mcp reconnect all`），不用重启会话。

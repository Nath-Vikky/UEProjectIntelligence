# UE Project Intelligence 用户指南

本文面向想让 LLM 通过 MCP 读取 Unreal Engine 工程信息的用户。当前稳定版目标是只读工程理解：LLM 可以查询资产索引、蓝图事件图、动画序列、DataTable、Sequencer、材质等结构化信息；不会保存、删除、重命名、编译或修改 UE 资产。

## 当前稳定版能力

- 读取 UE 5.3.x 工程的 Asset Registry 与目标资产 L1/L2 结构化扫描。
- 通过 MCP 暴露只读工具，供 Codex 等 LLM 客户端调用。
- 读取 Blueprint 图、节点、Pin、执行流、数据流、CFG/DFG 与常见语义。
- 读取 AnimSequence，并输出整个动画中发生位移、旋转或缩放变化的骨骼摘要。
- 支持短资产名和跨 scan 资产历史回退，例如 `BP_ThirdPersonCharacter`、`MM_Run_Fwd`。
- 支持打开 UE 编辑器时通过 Live Worker 刷新最新资产扫描。
- 支持不打开 UE 时查询已经入库的 SQLite 索引。

## 组件关系

UEPI 由四部分组成：

- Unreal 插件：位于 `Plugins/UEProjectIntelligence`，负责在 UE 内或 commandlet 中生成 scan JSON。
- SQLite 索引：默认位于 `Saved/UEProjectIntelligence/index.sqlite3`，MCP 查询主要读取它。
- Daemon：`Services/uepi_daemon/uepi_daemon.py`，提供本地 HTTP API、Web UI、worker 队列和刷新任务协调。
- MCP server：`Services/uepi_daemon/uepi_mcp_server.py`，由 Codex 等 MCP 客户端以 stdio 方式启动。

只查询已有索引时，Codex 只需要启动 MCP server 并能访问 `index.sqlite3`。如果要让 Codex 触发最新扫描，需要同时运行 daemon，并在 UE 编辑器里启动 Live Worker。

## 安装与构建

1. 把插件放到工程目录：

```text
<YourProject>/Plugins/UEProjectIntelligence
```

2. 重新生成工程文件或让 Unreal 自动发现插件。

3. 关闭 UE 编辑器后构建项目，例如：

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Build\BatchFiles\Build.bat" `
  GasDemoEditor Win64 Development `
  "-Project=F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -WaitMutex -NoHotReload
```

4. 打开 UE 编辑器，确认插件已启用，并打开：

```text
Tools > UE Project Intelligence
```

## 生成第一次索引

最简单方式是在插件面板点击 `Run Metadata Scan`。扫描结果会写到：

```text
Saved/UEProjectIntelligence
```

也可以用 commandlet 生成目标资产扫描：

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\BP_ThirdPersonCharacter.json"
```

如果 scan JSON 没有自动入库，可以手动 ingest：

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 `
  ingest --scan Saved\UEProjectIntelligence\BP_ThirdPersonCharacter.json
```

## 启动 daemon

需要刷新扫描、Web UI 或 Live Worker 时，启动 daemon：

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 `
  serve --host 127.0.0.1 --port 8765
```

Web UI 地址：

```text
http://127.0.0.1:8765/v1/ui
```

在 PowerShell 里按 `Ctrl+C` 关闭 daemon 时出现 `KeyboardInterrupt` 是正常退出方式。

## 启动 UE Live Worker

当 daemon 正在运行时：

1. 打开 UE 工程。
2. 打开 `Tools > UE Project Intelligence`。
3. 点击 `Start Live Worker`。
4. 面板里应显示 worker 正在 polling，且没有 HTTP 错误。

Live Worker 的作用是让 LLM 可以通过 MCP 提交只读 `metadata_scan` 任务，然后由当前打开的 UE 编辑器执行扫描。它不会保存、删除、重命名或编译资产。

如果只查询已有索引，不需要打开 UE，也不需要 Live Worker。

## Codex MCP 配置示例

下面以 Codex 为例。把路径替换成你的工程路径和 Python 路径。

```json
{
  "mcpServers": {
    "uepi": {
      "command": "C:/Users/renne/AppData/Local/Programs/Python/Python313/python.exe",
      "args": [
        "-B",
        "F:/Epic Games/UE5project/GasDemo/Plugins/UEProjectIntelligence/Services/uepi_daemon/uepi_mcp_server.py",
        "--db",
        "F:/Epic Games/UE5project/GasDemo/Saved/UEProjectIntelligence/index.sqlite3",
        "--tool-profile",
        "codex"
      ],
      "cwd": "F:/Epic Games/UE5project/GasDemo/Plugins/UEProjectIntelligence"
    }
  }
}
```

如果使用 Codex 桌面端图形配置，可以按字段填写：

- 启动命令：`C:/Users/renne/AppData/Local/Programs/Python/Python313/python.exe`
- 参数 1：`-B`
- 参数 2：`F:/Epic Games/UE5project/GasDemo/Plugins/UEProjectIntelligence/Services/uepi_daemon/uepi_mcp_server.py`
- 参数 3：`--db`
- 参数 4：`F:/Epic Games/UE5project/GasDemo/Saved/UEProjectIntelligence/index.sqlite3`
- 参数 5：`--tool-profile`
- 参数 6：`codex`
- 工作目录：`F:/Epic Games/UE5project/GasDemo/Plugins/UEProjectIntelligence`

保存配置后重启 Codex，或新开一个 Codex 对话。MCP 工具通常在会话启动时注入；如果当前对话看不到 `uepi_project_status`，先新开对话验证。

## 在 Codex 中验证

可以直接问：

```text
请调用 uepi_project_status 看看 UEPI 当前状态。
```

正常情况下会看到：

- `ok: true`
- `project_state: indexed`
- `llm_readiness.can_query_index: true`
- 有最新 `scan_id`
- 如果 UE Live Worker 在线，会看到 `has_live_editor_worker: true`

如果 `can_query_index` 是 true，但 `can_refresh_with_worker` 是 false，说明已有索引可以查询，但当前没有可用于刷新扫描的 worker。

## 常用 MCP 工具

- `uepi_project_status`：检查索引、scan 新鲜度、worker、任务队列。
- `uepi_project_refresh`：提交只读扫描任务，通常需要 daemon + UE Live Worker。
- `uepi_read_blueprint`：读取蓝图资产、事件图节点、执行流、数据流和快照。
- `uepi_read_animation`：读取动画序列、骨骼 track、变化骨骼摘要。
- `uepi_read_asset_context`：读取通用资产上下文和关系子图。
- `uepi_search`：搜索当前最新 scan 的实体。
- `uepi_related` / `uepi_subgraph`：读取实体关系和局部图。
- `uepi_data_query` / `uepi_data_page`：读取 DataTable、CurveTable 等数据资产。
- `uepi_cinematics_key_page`：读取 LevelSequence key 时间行。

## 示例问题

蓝图问题：

```text
用 UEPI MCP 看一下 BP_ThirdPersonCharacter 的 BeginPlay 连接了哪些节点。
```

Codex 应调用 `uepi_read_blueprint` 或先用 `uepi_search` 找 `ReceiveBeginPlay`，再用 `uepi_related` / `uepi_subgraph` 读取 `exec_flows_to`。在 ThirdPerson 示例中，BeginPlay 会先连接到 `类型转换为 PlayerController`，再进入 `Is Valid` 等初始化链路。

动画问题：

```text
用 UEPI MCP 看一下 MM_Run_Fwd 里哪些骨骼在整个动画中发生变化。
```

Codex 应调用 `uepi_read_animation`。新版扫描会返回 `animation_motion_summary`，其中包括 `changing_bones`、`translation_range`、`rotation_range_degrees`、`scale_range` 等字段。

刷新问题：

```text
请刷新并读取 BP_ThirdPersonCharacter 的最新蓝图事件图。
```

这类问题需要 daemon 正在运行，UE 编辑器已打开，并且 Live Worker 在线。Codex 可以调用：

```json
{
  "asset": "BP_ThirdPersonCharacter",
  "refresh": true,
  "wait_seconds": 120,
  "include_snapshot": true,
  "graph_depth": 2,
  "graph_limit": 400
}
```

## 使用建议

- 对蓝图优先使用短名或完整对象路径，例如 `BP_ThirdPersonCharacter`。
- 对动画可以使用短名，例如 `MM_Run_Fwd`；如果同名资产较多，使用完整对象路径更准确。
- 需要实时读取最新 UE 编辑器状态时，先启动 daemon，再打开 UE 并启动 Live Worker。
- 只做离线问答时，只要 SQLite 索引存在，Codex MCP server 可以直接查询。
- 如果返回内容被截断，MCP 会生成 `uepi://mcp-artifact/...`，可继续读取 artifact。

## 故障排查

Codex 看不到 UEPI 工具：

- 确认 MCP 配置路径正确。
- 确认 `--db` 指向存在的 `index.sqlite3`。
- 保存配置后重启 Codex 或新开对话。
- 可运行 `python Plugins\UEProjectIntelligence\Tools\test_mcp_stdio.py` 做 stdio 兼容测试。

UE 面板显示 worker HTTP 502 或注册失败：

- 确认 daemon 正在 `127.0.0.1:8765` 运行。
- 确认 UE 面板连接的是 `http://127.0.0.1:8765/v1`。
- 如果开启全局代理或 TUN，确保 `127.0.0.1` / `localhost` 走直连。
- 重启 daemon 后，在 UE 面板再次点击 `Start Live Worker`。

可以查询索引，但不能刷新：

- 这是正常状态，说明 MCP server 能读 SQLite，但没有在线 worker。
- 打开 UE 工程并启动 Live Worker 后，再调用刷新工具。

重新编译 C++ 插件失败：

- 关闭 UE 编辑器和游戏进程。
- 如果 UE 提示 Live Coding active，可在编辑器里按 `Ctrl+Alt+F11` 或直接关闭编辑器后重试构建。

最新 scan 不是你想查的资产：

- `uepi_search` 默认搜索最新 scan。
- 高层工具如 `uepi_read_blueprint`、`uepi_read_animation` 会尽量通过资产历史和短名解析找到正确资产。
- 需要最稳结果时，先对目标资产刷新扫描并 ingest。

## 安全边界

当前稳定版是只读设计。MCP 工具不会：

- 执行 shell 命令。
- 写入、保存、删除、移动或重命名 UE 资产。
- 编译 Blueprint。
- 修改关卡或 Actor。
- 在 UE 内暴露任意 HTTP 控制接口。

后续如增加写操作，应作为新的能力层单独设计权限、审计和确认流程。

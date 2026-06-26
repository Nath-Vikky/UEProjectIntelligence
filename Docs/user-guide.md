# UEPI 2.0-dev 用户指南（Codex 示例）

UE Project Intelligence 现在的默认路径是：

```text
UE 插件写 Snapshot
  → Saved/UEProjectIntelligence/store/manifests/saved.json
  → Codex 按需启动 stdio MCP
  → MCP 只读 Snapshot 回答问题
```

不需要启动 daemon、HTTP 服务、Web UI、worker 或任务队列。

## 1. 生成 Snapshot

方式 A：在 Unreal Editor 中打开 `Tools > UE Project Intelligence`，点击 `Run Snapshot Scan`。

方式 B：关闭 Editor 后运行 Commandlet：

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex `
  -UEPILevel=L2 `
  -UEPIAsset=/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter `
  -unattended -nop4 -nosplash
```

生成后应存在：

```text
Saved/UEProjectIntelligence/store/manifests/saved.json
Saved/UEProjectIntelligence/store/objects/**
```

## 2. Codex MCP 配置

命令：

```text
C:/Users/renne/AppData/Local/Programs/Python/Python313/python.exe
```

参数逐条填写：

```text
-B
F:/Epic Games/UE5project/GasDemo/Plugins/UEProjectIntelligence/Services/uepi/src/uepi/mcp_server.py
--project
F:/Epic Games/UE5project/GasDemo/GasDemo.uproject
--tool-profile
codex
```

工作目录：

```text
F:/Epic Games/UE5project/GasDemo/Plugins/UEProjectIntelligence
```

## 3. 可用 MCP 工具

- `uepi_status`
- `uepi_overview`
- `uepi_search`
- `uepi_context`
- `uepi_asset`
- `uepi_blueprint`
- `uepi_blueprint_trace`
- `uepi_animation`
- `uepi_impact`
- `uepi_diff`

## 4. Editor 是否必须打开

查询已有 Snapshot 不需要打开 Editor。只有生成或刷新 Snapshot 时才需要 Editor 或 Commandlet。

## 5. 快速验证

```powershell
$env:PYTHONPATH="F:\Epic Games\UE5project\GasDemo\Plugins\UEProjectIntelligence\Services\uepi\src"
python -m uepi status --project "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject"
python Plugins\UEProjectIntelligence\Tools\test_snapshot_mcp_v2.py
```

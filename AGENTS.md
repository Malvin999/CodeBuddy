# AGENTS.md

本仓库现在是 Malvin 自用的 CodeBuddy 私人分支。后续 agent 接手时优先按这里的真实本机状态、当前产品取舍和调试方法工作；公开 README 只作为背景资料。

## 当前目标状态

- 目标硬件是 M5Stack StickS3，主要配合 Codex App / Codex CLI 使用。
- 设备展示不再以 approval 操作为核心。当前用户处于只读场景，待 approve 只展示状态，不在设备上批准/拒绝。
- StickS3 首页为横屏 `240x135`：
  - 左侧上半部分展示 PET。
  - 左侧下半部分展示 `HH:MM`、秒数、星期+日期；USB 供电时显示 `charging`。
  - 右侧最多展示 3 个 session，状态为 `work`、`approve`、`done`，用小图标区分。
  - 没有可见 session 时，右侧展示 Codex Usage。
  - 右侧面板右上角显示 StickS3 当前电量百分比，不再显示 `LIVE/IDLE`。
- B 键在首页有 session 时会隐藏当前可见的 `done` session；`running/waiting` 不隐藏。
- session 切到 `waiting` 或 `done` 时设备会短响提醒。`waiting` 会回到首页，`done` 不打断当前界面。
- 只要有可见 session，非充电状态也保持亮屏；session 过期消失、右侧回到 usage 面板后，非 USB 供电 30 秒无操作熄屏。
- Codex Usage 表示“剩余额度百分比”，不是已使用百分比。不要在缺少真实 usage 数据时伪造 100%。

## 关键路径

- 仓库根目录：`/Users/malvin/Coding/opensource/CodeBuddy`
- 固件目录：`/Users/malvin/Coding/opensource/CodeBuddy/firmware`
- 主机端源码：`/Users/malvin/Coding/opensource/CodeBuddy/src/codex_buddy`
- 本地 agent 状态：`/Users/malvin/.code-buddy/state.json`
- Codex 会话日志：`/Users/malvin/.codex/sessions`
- Codex App 侧栏标题索引：`/Users/malvin/.codex/session_index.jsonl`
- Codex sqlite 状态库：`/Users/malvin/.codex/state_5.sqlite`
- Codex auth：`/Users/malvin/.codex/auth.json`
- 用户提供的 usage 参考项目：`/Users/malvin/Coding/gitlab-ones/codex-usage`

## 主机端运行方式

这个私人分支通常不是通过重新安装 Homebrew 包来验证，而是让已安装的 Homebrew Python 加载本仓库 `src`：

```bash
PYTHONPATH=src /opt/homebrew/Cellar/code-buddy/0.1.4/libexec/bin/python -m codex_buddy --state-path /Users/malvin/.code-buddy/state.json agent
```

launchd service 已改为在仓库根目录工作，并在 plist 中设置 `PYTHONPATH=/Users/malvin/Coding/opensource/CodeBuddy/src`。修改 host 端代码后，用下面命令重启后台 agent：

```bash
launchctl kickstart -k gui/$(id -u)/com.codebuddy.agent
```

本机真实 `codex` 在 nvm 下，脚本里需要保留这个 PATH：

```bash
export PATH="$HOME/.nvm/versions/node/v22.15.0/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
```

可用的本地启动脚本：

```bash
/Users/malvin/Coding/opensource/CodeBuddy/scripts/code-buddy-local.sh doctor
/Users/malvin/Coding/opensource/CodeBuddy/scripts/code-buddy-local.sh repair
/Users/malvin/Coding/opensource/CodeBuddy/scripts/start.sh agent
```

## BLE Helper 现状

本机 iOA 会拦截原始 helper 启动。当前绕法是构建并运行本仓库下的本地 app bundle：

```bash
/Users/malvin/Coding/opensource/CodeBuddy/scripts/build-native-ble-helper.sh
```

默认本地 helper：

```text
/Users/malvin/Coding/opensource/CodeBuddy/.build/native/CodeBuddyBLEHelper.app
```

相关环境变量：

```bash
export CODEX_BUDDY_BLE_BACKEND=native
export CODEX_BUDDY_BLE_HELPER_APP="/Users/malvin/Coding/opensource/CodeBuddy/.build/native/CodeBuddyBLEHelper.app"
```

如果 helper 相关逻辑变更，优先使用本地 bundle 验证，不要假设 `/opt/homebrew/Cellar/code-buddy/.../CodeBuddyBLEHelper.app` 能直接被 iOA 放行。

## Session 标题逻辑

Codex App 侧栏显示的标题不一定等于 `state_5.sqlite.threads.title`。本项目应优先使用：

```text
~/.codex/session_index.jsonl 的 thread_name
```

再回退到：

```text
~/.codex/state_5.sqlite 的 threads.title / first_user_message
```

这能避免设备上显示首条用户问题，而 Codex App 侧栏显示生成标题的错位。

## Host Payload 约定

StickS3 接收的是紧凑 JSON。改协议时必须同步改：

- `src/codex_buddy/reducer.py`
- `src/codex_buddy/catalog.py`
- `src/codex_buddy/session_log_watcher.py`
- `firmware/src/data.h`
- `firmware/src/main.cpp`
- 对应测试

当前 `sessions` payload 形态：

```json
[
  {"id": "05ec2b75ece7", "name": "评估 StickS3 接入 Codex App", "state": "running"}
]
```

其中：

- `id` 是 session id 末尾 12 字符，用于设备本地隐藏 done session 和状态变化提醒。
- `name` 是展示标题，host 端已经按显示宽度裁剪。
- `state` 只应为 `running`、`waiting` 或 `done`。

BLE payload 要尽量维持在 `900` bytes 内，见 `src/codex_buddy/reducer.py` 的 `_BLE_PAYLOAD_MAX_BYTES`。

## 固件构建与刷机

PlatformIO project 在 `firmware/`，不要在仓库根目录直接跑 `pio run`。

构建：

```bash
cd /Users/malvin/Coding/opensource/CodeBuddy/firmware
pio run -e m5stack-sticks3
```

刷机：

```bash
cd /Users/malvin/Coding/opensource/CodeBuddy/firmware
pio run -e m5stack-sticks3 -t upload --upload-port /dev/cu.usbmodem2101
```

串口监控：

```bash
cd /Users/malvin/Coding/opensource/CodeBuddy/firmware
pio device monitor -p /dev/cu.usbmodem2101 -b 115200
```

如果 `esptool` 报 `Device not configured`，优先让用户重新插拔 StickS3、确认端口仍是 `/dev/cu.usbmodem2101`，必要时再进 bootloader。不要先怀疑固件代码。

## 常用验证命令

Python 语法/导入级检查：

```bash
cd /Users/malvin/Coding/opensource/CodeBuddy
PYTHONPATH=src python3 -m compileall -q src/codex_buddy tests/test_catalog.py tests/test_session_log_watcher.py tests/test_reducer.py tests/test_usage.py
```

空白检查：

```bash
git -C /Users/malvin/Coding/opensource/CodeBuddy diff --check
```

固件编译：

```bash
cd /Users/malvin/Coding/opensource/CodeBuddy/firmware
pio run -e m5stack-sticks3
```

完整 pytest 当前可能受本机依赖影响失败：`uv run pytest ...` 曾因为 `pyobjc-framework-libdispatch` 构建找不到 `pkg_resources` 被阻断；Homebrew 安装包的 Python 环境里也未必有 pytest。遇到这个问题时，优先说明限制，并用 `compileall`、针对性脚本、`git diff --check`、`pio run` 补足验证。

## 运行时排查

查看 agent 当前推给设备的 snapshot：

```bash
python3 - <<'PY'
import json
from pathlib import Path
state = json.loads(Path('/Users/malvin/.code-buddy/state.json').read_text())
snapshot = state.get('snapshot', {})
print(json.dumps(snapshot, ensure_ascii=False, indent=2))
print("payload bytes:", len(json.dumps(snapshot, separators=(',', ':'), ensure_ascii=False).encode()) + 1)
PY
```

直接用本地 Codex 日志生成 session payload：

```bash
cd /Users/malvin/Coding/opensource/CodeBuddy
PYTHONPATH=src /opt/homebrew/Cellar/code-buddy/0.1.4/libexec/bin/python - <<'PY'
from pathlib import Path
import json
import time
from codex_buddy.session_log_watcher import SessionLogWatcher
from codex_buddy.catalog import SessionCatalog

watcher = SessionLogWatcher(Path.home() / '.codex' / 'sessions')
catalog = SessionCatalog()
catalog.replace_readonly(watcher.poll())
payload = catalog.snapshot(now=time.time()).as_ble_payload()
print(json.dumps(payload.get('sessions'), ensure_ascii=False))
print("payload bytes:", len(json.dumps(payload, separators=(',', ':'), ensure_ascii=False).encode()) + 1)
PY
```

检查本线程标题来源：

```bash
sqlite3 /Users/malvin/.codex/state_5.sqlite "select id, title, first_user_message, preview, updated_at_ms from threads where id='<thread-id>';"
rg '<thread-id>' /Users/malvin/.codex/session_index.jsonl
```

查看 launchd 状态：

```bash
launchctl print gui/$(id -u)/com.codebuddy.agent
```

常见日志目录来自 `src/codex_buddy/agent.py` / `launchd.py` 的 `default_log_dir`，通常在：

```text
/Users/malvin/.code-buddy/logs
```

## 开发原则

- 这个分支以个人体验优先，可以直接固化本机路径、StickS3 端口和 nvm PATH，但写代码时仍尽量让默认路径可覆盖。
- UI 改动要在 StickS3 的 `240x135` 横屏约束下设计，不要回到旧的纵向 approval 面板思路。
- 固件侧状态提醒应基于状态转移，不要基于每次 payload 刷新，否则会重复响。
- host 端如果新增字段，必须考虑 BLE payload 体积和 ArduinoJson 解析成本。
- Codex Usage 要以真实 auth + backend API 结果为准；拿不到数据时显示 fallback，不要制造“看起来正常”的假 quota。
- 不要恢复 approval 按钮行为，除非用户明确要求重新启用设备端审批。
- 保留用户未提交的本地改动，不要用 destructive git 命令清理工作区。

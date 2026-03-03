#任务清单 / Task Log

本文件用于项目根目录的轻量任务记录（人力可读）。记录任务的提出、修改和完成情况，便于追踪历史和责任人。

使用说明
- 在创建任务时新增一条 `TASKS` 表中记录，并在 `CHANGELOG` 中追加一条初始条目。
- 完成任务时将 `Status`设为 `Done` 并填写 `Completed` 日期；在 `CHANGELOG` 中追加完成条目。
- 修改任务信息时更新对应字段并在 `CHANGELOG` 中追加修改条目（包含日期、修改人、简短原因）。
- 时间格式建议使用 ISO8601（例如 `2026-03-02` 或 `2026-03-02T15:04:05Z`）。

任务表（按行记录）：

| ID | Title | Status | Owner | Created | Completed | Labels | Notes |
|----|-------|--------|-------|---------|-----------|--------|-------|
| T-001 | 添加任务清单记录文件 | Done | DevOps |2026-03-02 |2026-03-02 | documentation | 在项目根目录添加 `TASKS.md`，包含模板和示例。 |
| T-002 | 公钥验证与机器码许可检查 | In Progress | developer |2026-03-02 | | licensing, gui | 在 `ApplicationExeCode/RiaMain.cpp` 中修改 `showMachineCodeAndRequirePassword()`：检查根目录/应用目录是否存在 `AAAkey.json`，若存在调 validatePublicKey() 验证；若不存在或验证失败则弹窗提示机器码并退出。验证算法待后续提供并实现。 |

CHANGELOG（按任务维护详细变更历史）：

- T-001 |2026-03-02 | DevOps | Created: 初始创建任务文件 `TASKS.md`，包含模板与示例。
- T-002 |2026-03-02 | developer | Created: 在 `RiaMain.cpp` 中新增公钥检查流程和 `validatePublicKey` 占位函数；修改 `showMachineCodeAndRequirePassword()`逻辑以符合需求。验证算法尚未实现，后续会补充。
- T-002 |2026-03-02 | developer | Updated: `computeMachineCode()` 已更新以在 Windows 上通过 WMI 查询 `Win32_NetworkAdapter.PermanentAddress`，在 Linux 上读取 `/sys/class/net/*/perm_addr`以获取物理 MAC，作为机器码来源；保留了现有的回退方案。

模板示例（新任务示例）：

| ID | Title | Status | Owner | Created | Completed | Labels | Notes |
|----|-------|--------|-------|---------|-----------|--------|-------|
| T-003 | 修复 Windows 下构建警告 | Open | alice |2026-03-02 | | build, windows | 链接器给出未使用变量警告，需要排查第三方库。 |

CHANGELOG 示例条目：
- T-002 |2026-03-02 | alice | Created: 报告构建警告。
- T-002 |2026-03-04 | bob | Updated: 修复了未使用变量，更新编译选项。
- T-002 |2026-03-05 | alice | Completed: 验证通过并关闭任务。

格式化与自动化建议
- 若需要自动化（CI 集成或 Web UI），可考虑增加 `tasks.json` 或 `tasks.yaml`作为机器可读版本，并在 CI 中解析更新 `TASKS.md`。
- 若多人协作，建议在 Pull Request 描述中引用相关 `T-xxx` ID，并在 PR 合并后更新 `TASKS.md` 的 `CHANGELOG` 部分。

备注
-该文件为手动维护的轻量方案，适合快速跟踪任务。若需要更复杂的需求（分页、查询、过滤、权限），建议使用专门的 issue tracker（GitHub Issues、Jira 等）。

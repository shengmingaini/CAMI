# CI Status（CI 状态记录）

> 原则（见 `PROJECT_REQUIREMENTS.md` §31 / `DEVELOPMENT.md` §8）：**可信验收 = 本地**。
> CI 仅做 configure + build + ctest，**不作为验收通过依据**。

## Workflow

- 文件：`.github/workflows/build.yml`
- 矩阵：ubuntu-latest + windows-latest
- 步骤：checkout → vcpkg（pinned `aae277ac`）→ configure → build → ctest
- 触发：push/PR 到 `main`

## 状态（截至 2026-08-30）

| 项 | 状态 | 说明 |
|---|---|---|
| Workflow 文件 | 已创建 | 语法有效（结构合规） |
| 实际触发 | 未触发 | 本地仓库尚未推送；GitHub 远程仓库 `shengmingaini/CAMI` 当前不存在（需重建后才能 push） |
| 结论记录 | 本地验收为准 | TASK-000 验收由 `scripts/verify/task-000.sh` 本地跑通（Debug+Release 均 RC=0），CI 结论不影响本地验收 |

## 注意

- GFW 屏蔽 `ssh.github.com`，推送须走 `git@github.com:22`（见 TASK-000 §25）。
- 远程仓库删除 / 无 token 期间，仅做本地提交；push 待仓库重建后执行。

---
name: sync-metalio
description: 将 Metalio 官方更新完整合入 Valet E-Ink4 Plus，同时维护固件、Web 原版 Tab 和 Valet Tab。用于跟进上游版本、准备同步合并或核对三者覆盖情况。
---

# 同步 Metalio 上游

在 Git 仓库根目录执行。默认完整继承官方功能、修复及底层优化，再叠加 Valet；不把全量同步变成挑选提交。例外仅限明确冲突或设备不适用，并在覆盖表写明原因和替代行为。

## 1. 检测并判断

```sh
python3 tools/upstream/sync.py check --output /tmp/valet-upstream-status.json
```

读取输出后分支处理：

- `sourceUpdate=true`：进入完整合并。
- 仅公开固件版本领先：更新发布覆盖表；可以依据更新说明补交互原型，但标记 `release-notes`，不得推进源码基线或声称底层已修复。
- 无更新：报告已核对的 commit，不制造空合并。
- 查询失败或上游不再继承基线：说明无法确认的部分，查明来源，不按“没有更新”处理。

## 2. 完整合并

当前开发工作必须先形成提交；保留无关工作，不自动 stash 或提交他人的改动。在干净的 Valet 分支运行：

```sh
python3 tools/upstream/sync.py prepare --worktree ../valet-upstream-review
```

进入输出的工作树，处理待完成的 `merge --no-ff --no-commit`。用三方差异解决冲突，特别核对 application、音频、阅读器、显示、设置、存储和构建配置；不要整文件选 ours/theirs 覆盖另一方能力。保留官方提交祖先链和 Valet 附加功能。

## 3. 同步两个 Tab 和基线

逐项维护 `tools/upstream/coverage.json`：每项都有 **firmware / classic / valet** 状态；已完成项附代码或验证依据，延期/不适用项附原因。

- 原版 Tab 跟随官方功能和交互；新版 Tab 保留 Valet 产品结构，吸收同等能力。检查设置、返回路径、侧键和屏下三键，不能只补静态画面。
- Web 不证明刷新时序、DMA、触控升级或硬件缺陷已修复。按更新说明实现的排版须等待源码复核。
- 只在实际合入目标源码后推进 `baseline.json` 的 commit/sourceVersion，同时更新 `CMakeLists.txt` 和 `sim/upstream.js` 的源码身份。公开版本及原型依据分别记录，不混用。
- 官方同步不自动推进 `integrations/valet-gateway`，不以删功能或改分区解决体积超限。

## 4. 验证后交付

运行 [维护者入口](../../../docs/upstream-sync.md) 中的检查命令。实际固件合并还须运行相关 host 回归和完整 ESP-IDF 构建，记录 app 大小及分区余量；涉及硬件的变化列出实机结果或未验收边界。纯流程或 Web 改动不重复构建未变的固件。

提交真实 merge commit 和本轮覆盖变更；PR 描述只写变更、例外、验证、剩余限制。明确报告本地提交和远端推送状态。提交本身不等同于云端定时检查已启用；工作流须进入远端默认分支。同步不包含发布、生产部署或刷机。

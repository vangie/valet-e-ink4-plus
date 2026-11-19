# 上游同步

**完整合并官方，增量叠加 Valet；固件、Web 原版 Tab、新版 Tab 同轮验收。**

智能体执行入口：[sync-metalio Skill](../.agents/skills/sync-metalio/SKILL.md)。流程规则只在该 Skill 中维护，本页提供命令入口，不保存执行日志。

## 日常检查

```sh
python3 tools/upstream/sync.py check --output /tmp/valet-upstream-status.json
```

有新源码时，从干净的 Valet 分支准备隔离合并：

```sh
python3 tools/upstream/sync.py prepare --worktree ../valet-upstream-review
```

脚本保留当前工作区；无更新不创建合并，有冲突留在新工作树解决。只有二进制发布时跟踪差异，不伪造源码升级。

## 验收入口

```sh
python3 -m unittest discover -s tools/tests -p test_upstream_sync.py
python3 tools/upstream/validate.py
# 先安装 Playwright 和 Chromium；可用 VALET_CHROME 指定浏览器：
node tools/tests/sim_upstream.cjs
```

固件实际变更还需相关 host 回归，并在 ESP-IDF v6.1 环境运行：

```sh
idf.py -B build-valet build
```

记录 app 大小、分区余量和硬件验证边界；设备刷写另行处理。

## 维护位置

| 文件 | 职责 |
| --- | --- |
| [baseline.json](../tools/upstream/baseline.json) | 已合入源码 commit/version、官方来源及已审阅发布版本 |
| [coverage.json](../tools/upstream/coverage.json) | 每项官方变化在固件、原版 Tab、新版 Tab 的状态与依据 |
| [sim/upstream.js](../sim/upstream.js) | 原型源码身份、公开版本及还原依据 |
| [upstream-watch.yml](../.github/workflows/upstream-watch.yml) | 每 12 小时检查、手动检查、两个 Tab 浏览器回归 |

工作流进入远端默认分支后才生效；新源码或未审阅发布触发失败检查，通过 Actions 摘要/产物查看差异。发布事实与验收记录放在版本覆盖表或独立版本记录，勿追加到本流程或 Skill。

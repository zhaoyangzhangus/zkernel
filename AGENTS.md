# 项目记忆规则

- Git 保存历史。
- `PROGRESS.md` 保存当前状态。
- `SCRATCH.md` 保存未解决问题的临时推理摘要。
- `docs/decisions/` 保存长期设计决策。
- 源码和测试结果是最终事实。

## 开始任务

1. 读取 `PROGRESS.md`。
2. 有未解决问题时读取 `SCRATCH.md`。
3. 只读取当前任务相关的源码和决策文档。

## PROGRESS.md

只记录：

- 当前目标
- 当前任务
- 当前问题
- 测试状态
- 下一步

这是状态快照，不是日志。更新时覆盖旧内容，避免持续增长。

## SCRATCH.md

只记录当前问题继续分析所需的：

- 已确认事实
- 已排除项
- 当前结论
- 未验证项
- 下一步

保存结论，不保存完整推理过程。

问题解决后：

- 临时内容删除
- 近期仍需的信息移入 `PROGRESS.md`
- 长期结论移入 `docs/decisions/`

然后清理 `SCRATCH.md`。

## 任务结束前

- 更新 `PROGRESS.md`
- 未解决问题则压缩更新 `SCRATCH.md`
- 已解决问题则清理 `SCRATCH.md`
- 长期新决策写入 `docs/decisions/`
- 删除过时、重复或可从 Git/源码直接获得的信息

## 上下文丢失时

按以下顺序恢复：

`PROGRESS.md` → `SCRATCH.md` → 相关决策文档 → 源码 / `git diff`

只保留下一轮工作真正需要的信息。
# CLAUDE.md

## 默认模式：提案模式（proposal-only）

FlashKV 是一个数据库内核。**每一行代码都不能轻易更改。**

AI 在此项目中的默认行为是**提案模式**：分析问题、设计方案、输出到控制台——但**不直接修改代码**。

详细规则见 skill：`flashkv-proposal-mode`（通过 Skill 工具调用，或 AI 自动触发）。

### 提案模式核心规则
- **禁止 Write、Edit、NotebookEdit**，禁止任何写文件或破坏性 Bash 操作
- **允许** Read、Grep、Glob、只读 Bash、编译测试、运行验证
- **改动方案必须以代码块输出**到控制台，标清楚文件路径和行号，让用户审查

### 提议输出格式
```
### 建议改动：路径/文件名:行号
**说明**：为什么改、怎么改
```lang
新代码
```
```

### 许可短语（仅当轮有效）

用户必须在**同一条消息中**包含以下任一短语，AI 才能直接修改代码：

- "动手吧" / "开始改代码" / "可以改了" / "apply" / "implement it" / "go ahead"
- "退出提案模式" / "退出 talk-only"

许可是**一次性**的——只对当前消息有效，下条消息自动回到提案模式。

## talk-only 模式（兼容旧习惯）

以下关键词效果同上，进入提案模式：
- "talk-only" / "只看不改" / "只说不改" / "不要改代码" / "只分析" / "只讨论"

说"退出 talk-only"、"开始改代码"、"动手吧"时授予单次实施许可。

## 语义驱动开发工作流

项目采用五层语义驱动开发，**在写代码之前先写语义规约**。

### 五层目录结构

```
notes/      推导思路            人写           自由格式，想到哪写到哪，AI 不读
spec/       结构化规约          人写，AI 读     模块级语义文档，AI 理解"是什么"
contract/   形式化声明          人写，AI 读     YAML 格式，严格声明不变量/契约/状态机
src/        代码实现            AI 写，人审     C 源码，从 spec + contract 生成
memory/     AI 工作记忆          AI 写          跨会话持久事实（~/.claude/...）
```

各层职责：
- `notes/`：开发者的思考过程、源码阅读笔记、方案推演。给人类看，AI 不理。
- `spec/`：模块语义规约。自然语言描述"这个模块做什么、为什么这么设计"。AI 生成代码前必须先读。
- `contract/`：形式化声明。YAML 格式，锁定不变量、函数前置/后置、状态机、约束。AI 生成代码时必须遵守。
- `src/`：C 实现。从上述两层生成。代码必须满足 contract 中声明的所有约束。
- `memory/`：AI 跨会话记忆。设计模式、用户偏好、跨模块关联。

### 开发流程

1. 开发者在 `notes/` 写推导思路（自由格式）
2. 开发者在 `contract/<模块>.yaml` 写形式化声明（不变量、契约、状态机）
3. 开发者在 `spec/<模块>.md` 写结构化规约（面向 AI 的语义文档）
4. AI 读 `spec/` + `contract/`，生成/修改 `src/` 代码
5. AI 读 `spec/` 的测试场景，生成 `tests/` 测试

### contract/ YAML 格式规范

每个模块一个 YAML 文件，路径 `contract/<模块名>.yaml`。必须包含以下区块：

```yaml
module: <模块名>
depends: [<依赖模块>]
description: <一句话>

invariants:           # 不变量列表 — 违反即 bug
  <name>:
    describe: <人类可读的描述>
    condition: <精确的条件语句，可用伪代码>
    check: review | runtime | test   # 检查方式
    violation: <违反时的后果>

contracts:            # 函数契约
  <函数名>:
    file: <源文件:行号>
    access: public | private
    summary: <一句话>
    requires:         # 前置条件列表
      - "<条件1>"
    guarantees:       # 后置条件列表
      - "<保证1>"
    side_effects:     # 副作用
      - when: "<条件>"
        effect: ["<副作用1>"]
    constraints:      # 硬约束
      - name: <约束名>
        describe: <描述>
        value: "<具体值或表达式>"

state_machine:        # 状态机（可选）
  initial: <初始状态>
  states:
    <状态名>:
      desc: <描述>
      transitions:
        - trigger: "<事件>"
          to: <目标状态>

config:               # 配置参数
  <参数名>:
    value: <默认值>
    range: [<min>, <max>]  # 可选
    describe: <描述>

test_scenarios:       # 测试场景（可选）
  - name: "<场景名>"
    given: "<前置状态>"
    when: "<操作>"
    then: "<预期结果>"
```

### AI 行为约束

- 生成 `src/` 代码前，必须阅读对应的 `spec/` 和 `contract/`，在代码注释中标注覆盖了哪些 invariant 和 constraint
- 修改代码后，检查是否违反了 `contract/` 中声明的任何 invariant 或 constraint
- 如果实现过程中发现了 `contract/` 未覆盖的约束，回写 `contract/`

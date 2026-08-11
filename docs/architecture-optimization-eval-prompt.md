# xdec 项目架构优化方向 — 评估提示词

将以下内容完整复制给其他模型，用于对 xdec 反编译器项目的架构优化方向进行评估、排序和补充。

---

## 角色与任务

你是一位有 **编译器/反编译器/静态分析工具** 经验的资深软件架构师。请对下面描述的 **xdec** 项目及其提出的架构优化方向进行系统性评估。

**你的输出应包含：**

1. **总体判断**：当前架构成熟度（1–10）及一句话结论
2. **方向评估表**：对每个优化方向打分（收益 / 成本 / 风险 / 优先级），并给出 1–2 句理由
3. **优先级排序**：P0–P4 是否合理，是否需要调整
4. **遗漏项**：是否还有应补充的架构优化方向
5. **实施建议**：若只能做 1–2 项，选什么；若做 3 个月 roadmap，如何分阶段
6. **风险提示**：哪些改动可能破坏现有 483 单元测试 + L0 96/96 + L1 4/4 回归

---

## 项目背景

**xdec** 是一个自包含的 **AArch64 多级 IL 反编译器**（C++20，CMake + Ninja），目标平台为 Android NDK / AArch64 Linux ELF `.so`。

### 当前能力

- 机器码 → 多级 IL（Lifted → Local → CFG → SSA → Resolved → Optimized → Vars → Structured → Typed）
- OLLVM 控制流扁平化识别、MBA 代数化简、间接分支/跳转表解析
- Syscall 恢复、C 头文件类型导入、PLT/GOT 导入解析、尾调用识别
- 结构化 C 输出（if/while/switch，尽量减少 goto）
- 插件 ABI（`--plugin`）

### 当前回归状态

| 层级 | 内容 | 状态 |
|------|------|------|
| 单元测试 | Catch2，483 个测试 | 全过 |
| L0 eval | 96 个 NDK 编译的 ground-truth 函数 | baseline 96/96，typed 36/36 |
| L1 samples | 4 个真实混淆 `.so` 形状指标 | 4/4 |

### 目录结构

```
xdec/
├── specs/              ARM64 架构 spec（.xspec DSL）
├── include/xdec/       公共头文件
├── src/
│   ├── spec/           提升引擎
│   ├── il/             中间表示（hash-cons，文本 round-trip）
│   ├── passes/         优化与去混淆 pass（~20 个）
│   ├── analysis/       CFG/支配树/形状匹配/变量恢复/emit 预扫描（27 个文件）
│   ├── decompile/      多轮发现 driver
│   ├── emit/           structurizer + C printer
│   ├── binary/         ELF 镜像、target profile
│   ├── types/          类型数据库、syscall 表、NDK preset
│   └── tools/          CLI（xdec.exe）
├── eval/               L0 回归
├── samples/            L1 回归
├── tests/              单元测试
└── docs/               设计文档 01–16
```

### CMake 静态库依赖

```
xdec_il → xdec_analysis → xdec_passes → xdec_decompile
                        → xdec_emit → xdec_tools (CLI)
xdec_pass (框架) ← xdec_passes
xdec_spec, xdec_types, xdec_support (横切)
```

---

## 核心架构设计（已落地）

### 1. IL 设计（docs/01-il-spec.md）

- **Expr hash-cons**：结构相同 → 同一 `ExprId`，CSE 免费
- **Load/Read 是 Op 不是 Expr**：避免错误去重
- **惰性 flagdef**：不透明谓词可折叠
- **文本 round-trip**：`print(parse(print(f))) == print(f)`
- **溯源**：每个 Op 带 `va`（机器地址）和 `origin`（pass 名）

### 2. Maturity 契约

9 级成熟度，verifier 强制不变量，pass 声明 `level` / `produces`：

```
Lifted → Local → Cfg → Ssa → Resolved → Optimized → Vars → Structured → Typed
```

### 3. Pass 框架（pass/registry.h, pass/manager.h, pass/pass.h）

- **Registry**：Kahn 拓扑 + requirements + 注册顺序 tie-break
- **Manager**：before/run/after + verifier + fixpoint（上限 64）
- **Context**：function + image + memoryFacts + types + syscalls + names + discovery + seal
- **PassInfo::invalidates**：已声明（如 `"dominators"`, `"scc"`, `"cfg"`），但 **P7 analysis cache 未实现**
- **Plugin ABI**：存在，builtin pass 仍硬编码在 `passes/builtin.cpp`

### 4. Driver 不动点循环（decompile/driver.cpp）

```
每轮: re-lift(entry + discoveries) → pipeline 到 target → probe resolve-indirect
      → 上报 Discovery（完整候选集 + missing）
      → 下一轮
收敛或 round cap → 最终完整 pipeline run
```

- 每轮 **全量 re-lift**（不用 stitch，避免 maturity 混合）
- 携带 **Resolutions**（已证边）和 **Discovery 候选集**（24 轮→8 轮的关键优化）
- `maxTotalEntries=512` 硬上限防假 entry 爆炸

### 5. 内置 Pass 流水线（passes/builtin.cpp，注册顺序即 tie-break）

```
local-simplify → cfg-finalize → trampoline-fold → ssa-construct
→ const-fold-memory → ssa-optimize → stack-prop
→ recover-tailcall → resolve-call → recover-syscall → apply-types
→ resolve-indirect → fold-resolved-branch → vars
```

Driver 默认 target = **Vars**（不是 Structured/Typed）。

---

## 已知架构缺口（待评估的优化方向）

### 方向 A：抽出 `decompileToC()` API（P0）

**现状**：Vars → C 的完整流程全在 `src/tools/cli/cmd_pipeline.cpp`（~460 行），不在框架内：

```cpp
// cmd_pipeline.cpp 415-439 行（简化）
Dominators dominators = Dominators::compute(function);
PostDominators postDominators = PostDominators::compute(function);
vector<NaturalLoop> loops = naturalLoops(function, dominators);
StructuredFunction structured = structureFunction(function, dominators, postDominators, loops);
string text = printFunction(function, variables, frame, structured, cOptions);
```

同时 `Maturity::Structured` 和 `Maturity::Typed` 在 enum 中存在，但函数对象从未到达这两个级别。

**提议**：

```cpp
struct EmitOptions { /* 合并 COptions + 部分 DriverOptions */ };
struct DecompileResult {
  unique_ptr<il::Function> function;
  DriverReport driverReport;
  analysis::VariableTable variables;
  emit::StructuredFunction structured;
  string cSource;
};
Result<DecompileResult> decompileToC(engine, image, entry, options);
```

**预期收益**：CLI / MCP / finetuning / Python 绑定共用入口；测试不必 subprocess CLI；`observe` 可 dump 到 Structured。

---

### 方向 B：`SessionContext` 统一配置（P1）

**现状**：三处重复 wiring：

- `pass::Manager` setter（manager.h）
- `decompile::configure()`（driver.cpp）
- `cmd_pipeline.cpp` 手工设置 types/syscalls/names/memory/profile

**提议**：单一 `SessionContext` / `PipelineEnvironment`，Manager / Driver / Emit 共用。

---

### 方向 C：AnalysisCache 落地（P2）

**现状**：`PassInfo::invalidates` 已声明，`cfg_finalize`、`ssa_construct`、`ssa_optimize`、`trampoline_fold` 都在写 invalidates，但 **没有任何 cache 消费它**。每次 decompile 在 CLI 里重算 dominators、post-dominators、loops、StackFrame。

**提议**：按 function revision / pass generation 的 lazy cache；pass 改 IL 后自动 invalidate。

---

### 方向 D：`analysis/` 子域划分（P3）

**现状**：`xdec_analysis` 一个库 27 个文件，混合职责：

| 子域 | 文件示例 | 消费者 |
|------|----------|--------|
| CFG | dominators, scc, loops, reachability | structurizer |
| Shape | guard_cascade, dispatcher_shape, jump_table, profile | structurizer / passes |
| Vars | variables, stack_frame, typed_variables | vars pass + emit |
| Emit prep | stack_load_fold, load_inline, emit_redundancy, value_uses | CContext |
| Resolve | image_eval, index_bound, call_target | passes |

**提议**：逻辑分区（namespace 或子目录），不必立刻拆 CMake target。

---

### 方向 E：Structurizer 模式注册表（P3）

**现状**：`emit/structure.cpp` 随 pattern 增多持续膨胀；`tryDiamond` → `tryGuardCascade` → `tryDispatchTree` → `tryOneSided` → `gotoChain` 是硬编码优先级链。

**提议**：类似 pass registry 的可插拔 pattern 列表；`StructuredFunction`（Stmt AST）考虑独立 maturity + round-trip 测试。

---

### 方向 F：ERE 统一入口（P3）

**现状**：Emit Redundancy Elimination 分散在：

- `CContext` 构造时的预扫描（deadOps, inlinedStackLoads, inlinedMemoryLoads, deadLocalStackDeltas）
- `ExprPrinter::materializeAs`（H2，打印时 CSE，无法在预扫描决定）

docs/14 已文档化，但 `c_context.cpp` 逐个 include 分析头。

**提议**：`analyzeEmitRedundancy()` 一次调度 F–J 形状；H2 保持 emit 层（设计约束）。

---

### 方向 G：Structured / Typed 纳入 Pass Pipeline（P4）

**现状**：structure + typed variables 在 CLI 手工调用，不在 pass 框架内；maturity 模型不完整。

**提议**：至少 `structure` 注册为 pass（`produces = Structured`）；typed 可挂在 emit 前或作为 analysis pass。

---

### 方向 H：测试 PipelineFixture（P4）

**现状**：大量 pass 测试各自调用 `registerBuiltinPasses(registry)`，无统一 fixture；无「IL 文本 → 期望 C 片段」的高层测试。

**提议**：

```cpp
class PipelineFixture {
  Registry registry_;
public:
  PipelineFixture() { registerBuiltinPasses(registry_); }
  Result<Function> runTo(Function f, Maturity target);
  Result<string> decompileToC(/* minimal inputs */);
};
```

CMake 拆分 `xdec_tests_emit` / `xdec_tests_passes` 加速增量。

---

### 方向 I：公共 API 边界（P4）

**现状**：所有头文件在 `include/xdec/`，稳定 API 面不清晰；`decompile()` 只返回 Vars 成熟度 IL；无「binary + addr → C string」单一 API。

**提议**：`include/xdec/api.h` 作为唯一对外入口；内部细节保持 `src/` 私有头。

---

## 明确不建议的方向

| 方向 | 原因 |
|------|------|
| 重写 IL | hash-cons + round-trip 是核心资产 |
| 增量 lift（不全量 re-lift） | driver 设计论证了全量 re-lift 的正确性 |
| 过早拆 repo | monorepo + 静态库已够用 |
| 把 ERE H2 强行变成 pass | CSE scope 必须在打印时决定 |
| 把 emit 分析全部 pass 化 | 会加剧 maturity 混乱 |

---

## 关联项目上下文

- **finetuning/**：MCP/decomp 工具链，用同一批 `.so` 产训练数据
- **new_funtiue/redecomp v2**：状态块图 = 事实源、C = 一次性渲染；与 xdec 互补
- **decomp_mcp skill**：CFG 分析 MCP，可接 xdec 下游

---

## 评估维度（请逐项回应）

1. **方向 A–I 各自**：收益（1–5）、成本（1–5）、风险（1–5）、是否 P0–P4 合理
2. **P0–P4 排序**：是否应调整？有无应提前或延后的项？
3. **遗漏**：还缺哪些架构优化（如性能 profiling、错误处理、并发、配置系统、文档架构等）？
4. **trade-off**：维护 483 测试 + 96 eval + 4 samples 的前提下，最大 ROI 的 1–2 项是什么？
5. **3 个月 roadmap**：若团队 1–2 人，如何分阶段？每阶段的验收标准？
6. **与 redecomp v2 的关系**：xdec 应偏「全自动 pipeline」还是「可嵌入的 IL+emit 库」？API 设计如何兼顾 MCP？
7. **技术债预警**：哪些改动容易触发大规模回归？应用什么迁移策略（feature flag、parallel API、golden file 等）？

---

## 约束条件

- **语言**：C++20，CMake ≥ 3.24，Catch2
- **平台**：仅 AArch64 ELF（Android NDK / Linux）
- **不能破坏**：483 单元测试、L0 96/96 baseline、L1 4/4 samples
- **团队规模**：假设 1–2 人维护
- **优先级原则**：架构清晰度 > 性能 > 新功能；最小正确 diff

---

## 输出格式要求

请用 **中文** 回答，结构如下：

```markdown
## 总体判断
（成熟度评分 + 一句话）

## 方向评估表
| 方向 | 收益 | 成本 | 风险 | 建议优先级 | 理由 |
|------|------|------|------|------------|------|

## 优先级调整建议
（是否调整 P0–P4）

## 遗漏项
（补充方向）

## 实施路线图
（1–2 项 quick win + 3 个月分阶段）

## 风险与迁移策略

## 最终建议
（给维护者的 3 条 actionable 建议）
```

---

# xdec IL 规格

本文档是 IL 的规范定义。`src/il/printer.cpp` 与 `src/il/parser.cpp` 必须与之一致，
`tests/il/test_roundtrip.cpp` 强制 `print(parse(print(f))) == print(f)`。

## 1. 三条不可协商的约束

**惰性标志位。** `flagdef` 只记录「哪种运算、多宽、哪些操作数」，不展开成 N/Z/C/V 四个位表达式。
`flagcond` / `flagbit` 消费它。常量折叠 `flagcond(flagdef(...))` 就能整类消掉不透明谓词。

**溯源是硬不变量。** 每个 Op 携带 `va`（来源机器指令地址）与 `origin`（产生它的 pass）。
verifier 在 `lifted` 成熟度强制 `va` 存在。它同时服务于调试、C 输出的 `/* 0xADDR */` 锚点和边覆盖率门禁。

**文本可 round-trip。** 打印再解析必须完全一致。测试用例就写成文本，模型也直接读它。

## 2. 表达式与指令的分界

这是整套 IR 最核心的设计决定。

**Expr 是操作数的纯函数**，因此结构相同的两个表达式必然表示同一个值，于是表达式池做
**哈希共享（hash-consing）**：结构相同 → 同一个 `ExprId`。收益是三点：公共子表达式消除免费得到、
结构相等退化为一次整数比较、MBA 重写规则有了规范形式可以匹配。

**正因如此，内存读取和寄存器读取不能是表达式。** 同一地址在不同时刻的两次 load 可能得到不同值，
去重会静默改变语义。两者都是「定义一个值」的 Op，表达式通过 `val:T(%N)` 引用那个值。
VEX 的 GET/PUT 与纯 IRExpr 是同样的划分，出于同样的理由。

哈希共享还附带一条便宜的不变量：操作数总是先于使用者创建，所以 `operand.index() < expr.index()`
恒成立，verifier 用它一次线性扫描就证明值图无环。

## 3. 类型

只描述机器值。指针、结构体、有无符号属于 typed HIR 层，这一层刻意没有——`add` 不关心，
过早假装知道等于凭空发明信息。

| 拼写 | 含义 |
|---|---|
| `void` | 无值 |
| `i<N>` | N 位整数。`i1` 是比较产生的布尔。N 不必是 2 的幂（位域提取会产生 `i13`） |
| `f32` `f64` | 浮点 |
| `flags` | 不透明标志位包。没有宽度，无法被当成整数使用 |
| `i<N>x<M>` | M 个 N 位整数通道，`M >= 2` |
| `f<N>x<M>` | M 个 N 位浮点通道，`M >= 2` |

`i32x1` 被拒绝而不是当成 `i32`：一个类型只有一种拼写，文本才是规范的。

## 4. 文本语法

```
function @<entryVa> name="<name>" arch=<arch> maturity=<level> {
  block b<N> @<va>..<endVa> [entry] [preds=[b<N>, ...]] {
    @<va> | @none
    [%<N> = ]<opcode>[:<type>] [<operands>] [!from(<pass>)]
    ...
  }
  ...
}
```

- `@<va>` 行设定后续 Op 的来源地址，直到下一个标记。每个 block 的第一个 Op 必有标记，
  没有来源地址的写 `@none`。
- `preds=[...]` 是派生信息，解析时由 `rebuildEdges()` 重算，只为人读。
- `;` 起注释到行尾。dump 可以把反汇编挂在注释里而解析器不必理解它。
- block 标签 `bN` 与 `BlockId` 数值一致；`%N` 与 `ValueId` 数值一致。二者都在解析时按标签重建，
  因此手写 IL 可以用任意顺序的标号，但打印出来一定是规范编号。

### 指令

| 语法 | 说明 |
|---|---|
| `%0 = read <reg>` | 把寄存器当前内容快照成一个值 |
| `write <reg>, <expr>` | 写寄存器。verifier 检查宽度与寄存器一致 |
| `%0 = load:<type> <expr>` | `type` 是访问宽度 |
| `store:<type> <addr>, <value>` | |
| `br b<N>` | |
| `brc <expr>, b<taken>, b<notTaken>` | 条件必须是 `i1` |
| `brind <expr> -> unresolved` | 计算跳转，目标未解析。这是真实状态，不是缺字段 |
| `brind <expr> -> [b1, b2]` | 已解析的目标集 |
| `call <expr>` | **不是**终结符：控制流通常会回来。已证明不返回的调用后面跟 `unreachable` |
| `ret` | |
| `nop` | |
| `unreachable` | |
| `unimplemented "<mnemonic>"` | 无法解码或提升的指令。是终结符，因为它之后同块内的一切都不可信 |
| `intrinsic[:<type>] "<name>"(<args>)` | 效果已识别但未建模。SIMD、系统寄存器、屏障走这里 |
| `%0 = phi:<type>(<args>)` | 每个前驱一个操作数，顺序与前驱列表对齐 |

`intrinsic` 是唯一「是否定义值」取决于类型而非操作码的指令：非 `void` 定义值，`void` 不定义。

### 表达式

统一形如 `<op>[:<modifier>](<args>)`。冒号分隔 op 名与修饰符，因为 op 名本身含点
（`cmp.eq`、`shr.u`）。多数 op 的修饰符是结果类型。

```
const:i64(0x1000)          const:i64(-0x60)      ; 小负数按有符号打印，栈偏移更易读
val:i64(%3)                undef:i64()
add:i64(a, b)              sub:i64(a, b)         mul:i64(a, b)
and:i64  or:i64  xor:i64  not:i64  neg:i64
shl:i64  shr.u:i64  shr.s:i64  rotr:i64  rotl:i64
cmp.eq:i1(a, b)            cmp.ltu:i1  cmp.lts:i1  cmp.leu:i1  cmp.les:i1  cmp.ne:i1
zext:i64(a)  sext:i64(a)  trunc:i32(a)  bitcast:f64(a)
extract:i8(a, 24)          ; 从第 24 位起取 8 位
concat:i64(hi, lo)
clz  ctz  popcount  bswap  brev
select:i64(cond, ifTrue, ifFalse)
fadd:f64  fsub  fmul  fdiv  fneg  fabs  fsqrt
fcmp.eq:i1  fcmp.lt:i1  fcmp.le:i1  fcmp.uno:i1
inttofp.s:f64(a)  fptoint.s:i64(a)  fpconvert:f32(a)
```

标志位三件套的修饰符不是类型：

```
flagdef:<op>.<width>(...)   ; op ∈ add sub adc sbc logic
flagcond:<cc>(<flags>)      ; cc ∈ eq ne cs cc mi pl vs vc hi ls ge lt gt le al nv
flagbit:<bit>(<flags>)      ; bit ∈ n z c v
```

`flagdef` 的操作数个数由 `op` 决定：`logic` 取 1 个（结果），`add`/`sub` 取 2 个，
`adc`/`sbc` 取 3 个（含进位输入）。

## 5. 成熟度

每级是一份契约，不是标签：该级有一组确定的不变量，verifier 强制它们，pass 声明自己工作在哪级。
这是长流水线可调试的前提——输出错了靠逐级 dump 二分，而不是逐个 pass 读代码。

| 级别 | 契约 |
|---|---|
| `lifted` | 与机器指令一一对应。值是块内局部的，每个 Op 有 `va`，没有任何分析跑过。语义差分测试对照的就是这一级 |
| `local` | 块内折叠与死值消除完成 |
| `cfg` | 直接边完整：每块有显式终结符，缓存边与终结符一致，未解析的间接跳转被明确标记 |
| `ssa` | 寄存器与内存上的静态单赋值，跨块数据流显式化，phi 存在 |
| `resolved` | 间接跳转与调用已解析，或带证据地记录为不可解析。平坦化在此级被拆掉（若画像判定被平坦化） |
| `optimized` | 常量/复制传播、DCE、代数与 MBA 化简到达不动点 |
| `vars` | 栈槽与寄存器提升为变量，调用约定与序言惯用法已识别 |
| `structured` | 控制流结构化为 HIR AST |
| `typed` | 类型已推断 |

## 6. verifier 检查什么

pass 之后就跑，不是偏执：反编译 pass 的失败方式不是崩溃而是**看起来合理的错误输出**，
等错误在发射出的 C 里显现时，责任 pass 已在二十级之前。逐级检查把一次调试会话变成一条
指名 pass 与地址的错误消息。它报告**全部**问题而不是遇到第一个就停，因为一个坏假设通常在多处显现。

- 溯源：`lifted` 级 `va` 必须存在；`origin` 必须是已注册的 pass
- 表达式：元数为声明范围内；结果类型符合该 op 的 `ResultRule`；`const` 装得进声明宽度；
  `extract` 不越界；`concat` 位数相加等于结果；`zext`/`sext` 不变窄、`trunc` 不变宽；
  `flagdef` 操作数个数与 `op` 匹配；`flagcond`/`flagbit` 的操作数是 `flags`；
  操作数下标严格小于使用者下标（无环）
- 块：非空；终结符只出现在末尾；`cfg` 级起必须有终结符；phi 是块首前缀；地址不重复
- 边：缓存的 successors 必须等于终结符推出的结果（陈旧缓存是极难看见的一类 bug）；
  predecessors 与 successors 互为镜像
- 值：单赋值；`ValueInfo` 记录的定义 Op、所在块与类型与实际一致；
  `ssa` 以下值必须块内局部且先定义后使用；`ssa` 起 phi 操作数个数等于前驱个数
- `resolved` 级仍未解析的 `brind` 报错
- 从入口不可达的块是**警告**：pass 中途合法地孤立一个块、由清理 pass 稍后移除

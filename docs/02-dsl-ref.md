# 指令语义 DSL 参考

一个 `.xspec` 文件用一种语言同时声明三件事：**位模式**、**反汇编文本**、**语义**。
三者写在一起是关键——解码器与语义脱节是静默错误最常见的来源。

编译流程：`parse` → `check` → 规格 blob（P3）→ SpecEngine 运行时。
`xdec spec <file.xspec>` 跑到 `check` 为止并报告解码树质量。

## 1. 检查器保证什么

**能通过类型检查的规格构造不出畸形 IL。** 内建函数的签名就是 IL 各 op 的签名。

**宽度是被证明的，不是被假定的。** 一条规则用 `bits(32 << sf)` 同时覆盖 32/64 位两种形态，
检查器用符号整数（哈希共享 + 常量折叠）证明两个操作数同宽，而不需要知道 `sf` 是几。
编译期 `if sf == 1` 会**细化环境**：分支内 `sf` 代入 1，`32 << sf` 折叠成 64，于是返回
具体的 `bits(64)` 与声明的 `bits(32 << sf)` 相符。

**无法证明的一律报错。** 一个宽度证不出相等，恰恰就是静默截断会藏身的地方。

## 2. 结构

```
arch <name> { ... }        // 恰好一个，必须在最前
fn <name>(...) -> T { }    // 若干
insn <name> { ... }        // 若干
```

注释 `//` 与 `/* */`。整数支持 `42`、`0x1f`、`0b1011`，`_` 可作分隔符。

## 3. arch 块

```
arch arm64 {
  endian little              // little | big
  insnwidth 32               // 定长指令宽度（位），必须是整字节。变长编码 v1 明确不支持
  pointer 64                 // 可省略，默认取架构的指针宽度

  regfile gpr : bits(64) [32] {
    prefix "x"                                       // x0..x31
    zero 31 as "xzr"                                 // 31 号读作 0、写被丢弃
    view w : bits(32) = low 0 zeroext prefix "w" zero "wzr"
  }

  reg sp   : bits(64) role stack
  reg nzcv : flags     role flags
}
```

`zeroext` 声明「写这个视图会把父寄存器高位清零」，正是 AArch64 w 寄存器的行为。
它必须声明而不能推断：搞错的话，一次 32 位写之后的 64 位读会拿到陈旧的高半部分。

`role` ∈ `general` `float` `vector` `flags` `stack` `pc` `special`。

寄存器文件的元素在 IL 寄存器表里连续排列，所以 `gpr[n]` 是索引而不是按名查找。

## 4. 类型

| 拼写 | 含义 |
|---|---|
| `int` | 编译期整数：解码字段、移位量、寄存器号。**不会**出现在 IL 里 |
| `int(lo..hi)` | 带取值范围的编译期整数 |
| `bits(<int 表达式>)` | IL 整数表达式，宽度可以是符号式的 |
| `float(<int 表达式>)` | IL 浮点表达式 |
| `flags` | IL 惰性标志位包 |
| `void` | 无返回值 |

**范围为什么重要。** `if sf == 1` 的 else 分支能推出 `sf == 0`，前提是 `sf` 恰好只有两个取值。
解码字段自动带上由位宽决定的范围；函数参数需要显式写 `int(0..1)`。
范围同时用于证明 `gpr[Rn]` 不会越界——5 位字段索引 32 个寄存器是可证安全的。

## 5. 表达式

优先级由低到高：`?:` → `||` → `&&` → `|` → `^` → `&` → `== !=` →
`< <= > >= <u <=u >u >=u` → `<< >> >>>` → `+ -` → `* / /s % %s` → 一元 `- ~ !` →
后缀 `f(x)` `a[i]` `a.b`。

有符号/无符号在**运算符上**区分而不是在类型上：`>>` 逻辑右移、`>>>` 算术右移，
`<` 有符号、`<u` 无符号，`/` 无符号、`/s` 有符号。这一层没有「有符号整数」这种类型，
因为机器里没有。

`a ? b : c` 的条件必须是编译期的；它在提升时选择，不产生 IL 节点。
运行时二选一用 `select`，运行时分支用 `cbranch`。同理 `&&` `||` 只能作用于 `int`，
运行时的合取是两个 `bits(1)` 相 `&`。

两个 `bits` 运算必须同宽，移位量也不例外——需要先 `imm(n, width)` 或 `zext`。

### 语义里可用的名字

- 该指令的解码字段
- `insn_pc`：本条指令的地址；`insn_len`：字节长度；`opcode`：原始指令字
- 具名寄存器：`sp`、`nzcv`
- 寄存器文件元素：`gpr[n]`，视图 `gpr[n].w`

## 6. 内建函数

内建函数就是 IL 的 op。列表可用 `xdec::spec::builtinNames()` 取得。

**取值 / 常量**

```
imm(value, width)          // 常量，value 装不下 width 会报错
undef(width)               // 未定义值
load(addr, width)          // addr 必须是指针宽度
```

**位运算与转换**

```
zext(e, w)  sext(e, w)  trunc(e, w)       // 方向反了会报错
bitcast_int(e, w)  bitcast_float(e, w)
extract(e, lo, w)                          // 越界会报错
concat(hi, lo)                             // 宽度相加
select(c, a, b)                            // c 必须是 bits(1)，两臂同宽
clz ctz popcount bswap brev (e)
rotr(e, n)  rotl(e, n)  mulhi_u(a,b)  mulhi_s(a,b)
```

**惰性标志位**

```
flagdef_add(a, b)   flagdef_sub(a, b)      // 同宽
flagdef_adc(a, b, c)  flagdef_sbc(a, b, c) // c 是 bits(1) 进位输入
flagdef_logic(result)
cond(flags, cc)                            // cc 必须由解码字段算得
flagbit(flags, bit)                        // bit ∈ 0..3 对应 n z c v
```

四个标志位在这里**不会**被展开成表达式。常量折叠 `cond(flagdef_sub(...))` 就能整类
消掉不透明谓词，这是去混淆能便宜地做成的前提。

**浮点**

```
fadd fsub fmul fdiv (a, b)     fneg fabs fsqrt (a)
fcmp_eq fcmp_lt fcmp_le fcmp_uno (a, b) -> bits(1)
inttofp_s(e, w)  inttofp_u(e, w)  fptoint_s(e, w)  fptoint_u(e, w)  fpconvert(e, w)
```

浮点算术只有具名内建，没有运算符拼写：舍入行为绝不该由一个 `+` 隐含。

**编译期整数辅助**

```
sextint(v, fromBits)                       // 按位宽符号扩展一个编译期整数
ones_int(n)
ror_int(v, amount, width)
replicate_int(pattern, patternBits, totalBits)
```

后两个是为了不引入循环也能写出 AArch64 的逻辑立即数解码。DSL 没有循环，
也因此**禁止递归**——递归在提升期无法终止，检查器会直接报调用环。

**效果（语句位置）**

```
store(addr, value)                         // 宽度取自 value
branch(targetAddr)                         // targetAddr 是编译期整数
cbranch(cond, takenAddr, notTakenAddr)
brind(target)   callind(target)            // 运行时目标
call(targetAddr)   ret()   nop()   unreachable()
intrinsic("name", args...)                 // 效果已知但未建模
intrinsic_value("name", width, args...)
unimplemented("mnemonic")
```

分支目标写成**地址**而不是块号：提升时块还不存在，地址到块的映射由 CFG 构建阶段完成。

屏障、系统寄存器、暂未建模的 SIMD 走 `intrinsic` 而不是 `nop`。
假装它什么都不做，会让后面的 pass 越过它重排。

## 7. 语句

```
let x = <expr>;              // 可遮蔽同名绑定，但不能重绑解码字段
<寄存器> = <expr>;           // sp = ... / gpr[n] = ... / gpr[n].w = ... / nzcv = ...
<调用>;                      // 求值只为其效果；丢弃返回值会给警告
if <编译期条件> { } else { }
return <expr>;
```

有返回值的 `fn` 必须在所有路径上 `return`。

## 8. insn 块

```
insn subs_shifted_reg {
  encoding sf:1 "1101011" shift:2 "0" Rm:5 imm6:6 Rn:5 Rd:5
  asm "subs {Rd:reg(sf)}, {Rn:reg(sf)}, {Rm:reg(sf)}[, {shift:shift} #{imm6:dec}]"
  require Rd != 31;
  priority 10
  semantics {
    let a = read_gpr(Rn, sf);
    let b = shift_reg(sf, read_gpr(Rm, sf), shift, imm6);
    write_gpr(Rd, sf, a - b);
    nzcv = flagdef_sub(a, b);
  }
}
```

### encoding

字段**从高位到低位**书写，与所有架构手册画图的方式一致，总宽必须等于 `insnwidth`。
条目形如 `名字:位数`、`_:位数`（不绑定）、或 `"0101"`（字面位串）。
拆开的立即数（如 `immhi`/`immlo`）声明成两个字段，在语义里用整数算术拼回去。

`encoding` 到下一个 insn 属性为止。属性名后面不会跟冒号，字段名后面一定跟冒号，
一个 token 的前瞻就足以区分，不需要终结符。

### asm

`{表达式}`、`{表达式:样式}`、`{表达式:样式(参数)}`。样式 ∈
`reg` `hex` `dec` `cond` `label` `shift`。
`[...]` 是可选组，内容为默认值时整组不打印——ARM 语法里大量的可选操作数就是这么来的。
字面的 `{` `}` `[` `]` 用反斜杠转义。

### require 与 priority

位模式分不开的编码用 `require`（如别名要求 `Rn == 31`）。`priority` 数值大的先匹配。

### 编码重叠检测

两条编码能匹配同一个指令字，是规格里最常见的 bug，而生成出的解码器会静默地
选中先测到的那条。检查器会报错并给出**一个两者都匹配的具体指令字**作为证据。

真实的重叠是存在的——`cmp` 就是 `Rd == 31` 的 `subs`——所以以下三种情况会被接受：

1. 一条严格约束了另一条的位的超集（别名对基础指令的典型关系）
2. 两条声明了不同的 `priority`
3. 至少一条带 `require`（证明守卫互斥需要求解器；此时相信声明）

### 解码决策树

在候选集上逐层挑选**区分度最好的连续位段**（最宽 8 位）建树，不约束该位段的模式会进入
每个子节点。叶子按「优先级降序 → 约束位数降序 → 声明顺序」排列，解码器在叶子上仍要
复核 mask/value 与 `require`。树只做收窄，不做确认。

`xdec spec` 会报告节点数、深度和最大叶子；叶子很大说明编码重叠得比应有的多。

## 9. 与 SLEIGH 的取舍

同样是声明式指令语义，但三处刻意不同：

- **惰性标志位是一等公民。** SLEIGH 把标志位写成四个独立赋值，之后要靠优化才能消掉。
  这里 `flagdef` 是一个不透明节点，不透明谓词的折叠因此变成一条重写规则。
- **宽度多态被静态检查。** 一条规则覆盖 32/64 位两态，且这种覆盖是被证明的。
- **溯源是硬性的。** 提升产生的每个 Op 都带来源地址，这是 C 输出锚点和覆盖率门禁的基础。

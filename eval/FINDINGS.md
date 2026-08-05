# xdec 评测语料（NDK ground-truth）

用 Android NDK 把已知 C 源码编译成 `arm64-v8a` 动态库，再对 `eval_*` 符号逐个反编译，与 manifest 期望对比。

**当前：baseline 36/36、typed 36/36 通过**（2026-08-05）。首次跑分 0/20，中途 7/20，
基础语料修完 20/20，之后加入 syscall（6）、types（8）、tailcall（2）三类共 36 个函数。  
基线分模式归档：`baseline.json` / `typed_baseline.json`，`run.ps1 [-Typed] -UpdateBaseline` 更新，
`run.ps1 -Baseline <file>` 与指定基线对比。

## 快速运行

```powershell
cd d:\funtune\xdec\eval
.\build.ps1                  # NDK 编译 libxdec_eval.so（source*.c 全部参与）
.\run.ps1                    # baseline 模式：不给任何外部信息
.\run.ps1 -Typed             # typed 模式：--types + --syscall-table
.\run.ps1 -Typed -UpdateBaseline
```

**两种模式是同一份 manifest 的两次评分**，这是这套语料现在的核心结构：
`expect` 是两模式共同要求，`expect_baseline` 是「光靠推断能证到什么」，
`expect_typed` 是「给了头文件之后多出什么」。同一个函数两边都登记期望，
才能说明外部类型确实带来了增量，而不是把断言写松了。
`decompile_args`（按 case）与 `modes`、`enabled` 一起决定每个 case 怎么跑。

依赖：
- NDK：`%LOCALAPPDATA%\Android\Sdk\ndk\27.2.12479018`（可在 `build.ps1 -NdkRoot` 改）
- xdec：`xdec\build\dev\bin\xdec.exe`（先 `cmake --build build/dev`）

输出（`<mode>` = `baseline` / `typed`）：
| 路径 | 内容 |
|------|------|
| `build/out_<mode>/eval_*.c` | 每个函数单独反编译结果 |
| `build/all_<mode>.c` | 合并输出，评分即在此文件上按函数分段进行 |
| `build/report_<mode>.json` | 机器可读评分（含 mode 字段） |
| `build/run_<mode>.log` | 耗时与 exit code |
| `baseline.json` / `typed_baseline.json` | 归档基线 |

## 语料分类

| 类别 | 函数 | 测什么 |
|------|------|--------|
| basic | add_three, bitwise_mix, rotr32 | 算术、位运算 |
| conditional | abs, max, clamp | if/else |
| loop | sum_array, factorial, count_bits | while/do/for |
| switch | switch_arith, switch_sparse | switch 恢复 |
| memory | vec3_dot, array_max | 指针/结构体/数组 |
| call | call_chain, indirect_binop | 调用与间接调用 |
| signed | sign_extend_chain, cmp_signed | 有符号比较 |
| nested | nested | 嵌套控制流 |
| control | early_return, state_machine | 多 return / 状态机 |
| syscall | svc_write, svc_gettimeofday, svc_getpid, svc_unknown, svc_nr_from_arg, svc_errno | `svc` 语义恢复：号码常量折叠、按真实 arity 裁参、未知号码降级、`-errno` 判定 |
| types | types_struct_arg, types_struct_field, types_enum_switch, types_typedef_chain, types_fn_ptr, types_return_struct, types_extern_global, types_void_ptr | 外部头文件类型导入：签名、字段名、枚举、typedef 链、函数指针 arity |
| tailcall | tailcall_table, tailcall_import | 间接尾调用：经调用方指针数组、经 PLT/GOT 到别的模块（见 `docs/08-tailcall.md`） |

源码：`corpus/source.c`、`corpus/source_syscall.c`（内联汇编 `svc`）、`corpus/source_types.c`、
`corpus/source_tailcall.c`  
类型头：`corpus/types/eval_types.hdecl` —— 既是编译语料用的 C 头，也是反编译时 `--types` 导入的头，
所以「真值」和「被测输入」不可能各自漂移。  
期望：`manifest.json`

---

## 已修问题（语料曾暴露、现已通过）

| 曾经的表现 | 根因 | 修在哪 |
|------------|------|--------|
| 全部签名 `uint64_t eval_*(uint64_t, ...)` | vars 直接把寄存器宽度当类型；AArch64 里 32 位参数走 `w0`，寄存器仍是 64 位 | `analysis/variables.cpp`：按「被读的宽度」而非「寄存器宽度」定型，返回类型剥掉 ABI 的 Z/SExt，指针经 phi 传播；`emit/c_printer.cpp` 用恢复出的返回类型 |
| eval_early_return：return 块排在 entry 之前、3 处 goto | 结构化按「谁先被认领」出顺序，不是按 RPO；单臂 if 只在第二遍才试 | `emit/structure.cpp`：顶层按 RPO 排序；死路分支第一遍就允许单臂 if；补上被误删的分支边 |
| eval_switch_arith：`case N: goto L_xxx;` | switch 只记录 case 目标块，body 靠 label 兜 | `emit/structure.h` + `c_stmt.cpp`：case body 内联（仅当 handler 只有 dispatcher 一个前驱）；range guard 并入同一个 switch |
| `return op(a,b)` 这类间接尾调用整个函数反编译失败（`indirect branch is still unresolved`）；尾调用到 import 的函数则把 PLT 桩当成自己的块，产出带重复 label、编译不过的 C | `br xN` 既是 switch 分派也是尾调用，lifter 分不出来；resolve-indirect 只会尝试把它解成本函数内的块 | 新增 `passes/recover_tailcall.cpp`（Ssa→Ssa，见 `docs/08-tailcall.md`）：按「目标地址链的基址来自调用方指针 / loader 绑定的 import 槽，且不含本镜像地址」判定尾调用，改写成 call + return；参数取 ssa-construct 在未解析间接分支上记录的 ABI 快照 |
| 平坦化 dispatcher 里，解析到唯一目标的间接分支被印成 `t = load(...); if (t == 0xaddr) goto L; L: ...`——一个永远为真的运行时判断，外加一对从不需要的 goto/label | resolve-indirect 只负责证明目标集合，不负责判断集合大小是 1；结构化对 `IndirectBranch` 一律走 `switchFor`，目标数 <3 时退化成「诚实的比较链」 | 新增 `passes/fold_resolved_branch.cpp`（Resolved→Resolved，在 resolve-indirect 之后、vars 之前注册）：目标数恰为 1 的 `IndirectBranch` 改写成无条件 `Branch`，结构化随即按普通顺序流处理，不再需要比较/goto/label。特意没有并进 resolve-indirect 本身——driver 用它做跨轮探测（`decompile/driver.cpp` 的 `Probe`），探测阶段仍需要看到 `IndirectBranch` 才能像之前一样逐轮重新推导，这个化简只出现在最终那趟完整流水线里 |
| 一个 320 block 的巨型分派函数里有 300 个 brind 静态不可解，Resolved 门禁逐条报错，整次反编译 exit 1、零输出，320 个已经看懂的 block 一起丢掉 | 门禁本身是对的（未解析 brind 就是 CFG 上的洞），但「要么全懂要么什么都不给」与 driver 对轮数上限的既有论证自相矛盾 | `pass::Context::setSealUnresolvedBranches` + CLI `--allow-unresolved`：`resolve-indirect` 把答不出来的可达 brind 改写成 `Unimplemented`，名字带分支地址和算不出来的表达式。verifier 契约一个字没改——`Unimplemented` 在 Resolved 本来就合法，下游全都已经把它当作不透明终结符处理。默认仍然失败；`unresolved_branches` 进评分指标，只许降不许升 |
| 跳转表里的空槽（表项为 0）被当成合法目标，CFG 上长出一条指向地址 0 的边（`case 307: goto L_0x0;`） | `isCode()` 查段权限，而共享库的第一个段可以从 vaddr 0 开始且映射为可执行，于是链接器留下的 0 被放行 | `passes/resolve_indirect.cpp`：指针表的空槽既不算目标（0 不是代码）、也不算表的结束标记（没填的槽说明不了表在哪结束——第一版当成结束，`sample_jni_onload` 立刻回归：无界扫描提前停下，把空槽前面一段错误前缀当成了目标集） |
| eval_sum_array：循环条件成了 `a1 != 1` | ssa-construct 没把「寄存器入口值」算作 def-site，循环归纳变量因此没有 phi，条件读到的是初值 | `passes/ssa_construct.cpp`：entry block 对每个跟踪寄存器都算 def-site |
| eval_state_machine：没有 loop/switch，只有一堆 goto | 条件跳转读的是裸 flag（`flagcond:ne(val:flags(%9))`），跨块没折成 `Cmp`，结构化认不出 | `passes/ssa_optimize.cpp`：开头显式跑 `foldFlagConditions`；`emit/structure_dispatch.cpp`：识别 `while(true){switch{...continue;}}` |
| eval_abs/max/clamp/cmp_signed：全是嵌套三元 | 顶层 `Select` 直接印成 `?:` | `emit/c_stmt.cpp`：`printSelectReturn` 把 return 里的 Select（含穿透 Z/SExt/Trunc 的嵌套）展成 if 链 |

### 仍属设计选择（非缺陷）

| 函数 | 说明 |
|------|------|
| eval_call_chain | NDK `-O1` 已内联 `helper_double/triple`，反编译得到 `x*6` 算术，**语义正确**；manifest 已按内联后期望 |
| eval_rotr32 | 用 `__xdec_rotr32` intrinsic 而非 `>>`/`<<` 组合，emitter 设计选择 |
| eval_early_return | 期望 1 处 goto：编译器把两条 return 合并成共享尾块，那处 goto 是真实控制流 |
| eval_cmp_signed | `cset` 只产生一个比较，源码的三分支 if 在 `-O1` 下已被折掉，期望放宽到 1 个条件 |
| eval_types_return_struct | 12 字节结构体按值返回走 x0+x1；发射器只建模一个返回寄存器，第三个分量根本没恢复。头文件也不采用为返回类型（否则产出的 C 编译不过），只在注释里说明 `/* header says EvalVec3 */` |
| eval_types_extern_global | GOT 里的全局：符号名两模式都能给（`ptr_g_eval_stats`），但类型没接上——见 `docs/06-type-import.md` 的 Known gaps |
| eval_types_fn_ptr | 现在就是 `return op(a,b)`，即真正的间接尾调用 `br x0`；baseline 返回类型是机器字（尾调用把 x0 原样交回，没有任何东西把它收窄），typed 模式按头文件收成 `int32_t (EvalBinOp, int32_t, int32_t)` |

---

## 与 obfuscated 样本对比

| 维度 | NDK eval（本语料） | afRDLog（生产样本） |
|------|-------------------|---------------------|
| 规模 | 36 函数，<100 行/函数 | 2882 block，84k 行 C |
| 已知真值 | 有 C 源码 | 无 |
| 主要失败 | 签名、if/三元、多 exit、状态机（已修） | goto 洪流、MBA、dispatcher |
| 用途 | 回归每个 pass 的**基础能力** | 压力测试 **VMP/平坦化** |

**结论**：NDK 语料适合作为 CI 基准；afRDLog 仍用于大规模 obfuscation。两者互补。  
小语料修完不等于大样本可用——afRDLog 上两个只有大样本才暴露的问题见下。

---

## L1 自有样本（`samples/`，2026-08-05 建立）

在动手改 pass 之前先把「感觉变好了」变成可重复的数字：`samples/` 是 NDK eval（L0，有源码真值）
和 afRDLog 级 mega 样本（L2，暂缓）之间的中间层——真实的、自己持有的混淆 `.so`，没有源码，
只按结构形状（goto/switch/三元计数、能否在给定 rounds 内跑通）评分。机制、加样本的方法见
`samples/README.md`；这里只记录当前基线和它说明了什么。

目前两个 case，都来自 `libtarget.so`（`finetuning/tests/fixtures/`）：`sample_jni_onload`
（`JNI_OnLoad @ 0xe4c8c`，与 afRDLog 分析里提到的同一个函数）和 `sample_mega_dispatcher`
（`0x199214`，见下方 A3 一节）。前者现在有了可以 diff 的基线而不只是一次性观察：

| 指标 | S0 首跑 | Phase A1 后（`fold-resolved-branch`） | Phase A2 后 | Phase A4 后 | 说明 |
|------|--------|--------------------------------------|------------|------------|------|
| rounds | 24（默认 8 会失败） | 24（不变） | **8** | 8 | 见下方 A2 一节；8 轮里 7 轮在证边，第 8 轮确认不动点 |
| gotos | 14 | **7** | 7 | 7 | 解析到唯一目标的间接分支不再印成 if+goto+label（见下方已修问题表） |
| ifs | 12 | **3** | 3 | 12 | A1 砍掉的是恒真判断；A4 加回来的是真判断——原本被压成三元的那些 |
| switches | 1 | 1 | 1 | 1 | 三路 case 的那一个分派点仍是唯一被识别出的 |
| ternaries | 17 | 17 | 17 | **13** | 赋值位置的 Select 展开成 if/else；剩下 13 处见下方 A4 一节 |

Phase A1 的第一步（`fold-resolved-branch`，见「已修问题」表）已经把 gotos/ifs 砍掉一半：
凡是间接分支只解析出一个目标的地方，本就没有第二条路可走，之前却因为分支本身没变成
`Branch` 而被印成一段「比较-goto-标签」的仪式性代码。剩下的 7 处 goto 是真正的多路合并
（3-way switch、循环回边）和一个混淆器留下的死循环哨兵块，需要 Phase A1 剩余部分（dispatcher
识别成 `while(true) switch`）才能进一步收敛。`samples/baseline.json`
记录了这次的基线，`run.ps1 -UpdateBaseline` 在每次刻意的形状改动后更新它。

### Phase A2：24 轮 → 8 轮，以及一个不该成立的表

轮数高不是「跳转表枚举不给力」，而是**每一跳要花两轮**。把 `XDEC_LOG=driver=info,resolve=debug`
的轮次日志排开看，奇数轮只发现地址、偶数轮只证边，严格交替：

- 第 N 轮：某个 `br xN` 的目标算出来了，但那个地址还没有 block，于是 `resolve-indirect`
  按「全中或不中」的规矩不解析，只上报发现；driver 把地址加进 entry 集合。
- 第 N+1 轮：地址被 lift 成 block 了，这轮才把入边证出来——可是这轮的 SSA 是在**入边装回来之前**
  建的，所以这个新块此刻在 CFG 里没有前驱，块内寄存器读不到任何流入值，它自己结尾的
  `br xN` 必然算不出来。
- 第 N+2 轮：入边终于在建 SSA 之前装回去了，新块有了前驱，它自己的分支这才解得开——然后
  又开始下一跳的两轮。

真正缺的东西很小：第 N 轮其实**已经算出了完整候选集**，只是因为块不存在而丢掉了。现在
`pass::Discovery` 把「分支地址 + 完整候选集 + 其中尚未 lift 的部分」一起上报，driver 连同
候选集一起累积；于是第 N+1 轮在建 SSA 之前就能把入边装回去，lift 这一层和打开这一层
变成同一轮。13 个 block、7 层链，24 轮降到 8 轮，输出与 `--rounds 24` 逐字节相同。

顺带发现一个更该修的东西：`.data.rel.ro` 里的**裸基址被当成了表**。`load(g_1e14e0)` 这种
单个全局函数指针没有 index，`matchJumpTable` 仍然匹配成 base=0x1e14e0、stride=8 的表，
`resolve-indirect` 于是从这个槽往后枚举最多 512 个 entry。这个样本里 512 个全都「看着像代码」
（`.data.rel.ro` 后面本来就是成片的重定位函数指针），所以它跑满上限、什么都不声称——纯浪费。
但换一个布局，只要第 4 个槽恰好不像代码，它就会把前 3 个槽当成「一个只有一个真目标的分支」的
三个目标，凭空造出两条 CFG 边。现在没有 index 的匹配直接不走整表枚举：裸基址是指针不是表，
值集合读那一个槽就停，本来就是这个形状该走的路。

还剩的两条 `nothing bounds its index` 是**真表**（`g_1e14d0 + state*8`，有 index），
但它的 index 形如 `(int64)0xf < (int64)x ? 6 : x`——带符号比较，负数会原样穿过去，
所以这不是一个 sound 的上界，`boundOnIndex` 拒绝声称是对的；而值集合本来就把它精确解成了
3 个目标并印成了 `switch`。这里不需要放宽表枚举。

driver 的轮数预算也跟着改成「自适应」：预算用完那一轮如果**还在证新边**，就再给一轮，
直到 `kRoundCeiling = 64` 兜底——预算本来就只是对函数有多深的猜测，猜差一层不该换来一个
半解析的函数。显式传 `--rounds N` 仍然是硬墙（调用方要的是有界耗时），`samples/run.ps1`
正是这么跑的，所以样本用例是在 8 轮**硬上限**下通过的。

### Phase A3：320 个 block 不该因为 300 个洞被整个丢掉

第二个 L1 case `sample_mega_dispatcher` = 同一个 `libtarget.so` 里 `0x199214` 的那个函数，
同一套混淆放大到 320 个 block。它的分派形状高度规律：

```
cmp  x8, #0x132
csel x8, x9, x8, gt          ; 索引 clamp
ldr  x8, [x26, x8, lsl #3]   ; x26 是寄存器基址表
br   x8
```

`x26` 由函数中段一条 `adr x26, #0x1f6020` 唯一赋值，但在这些分派块里它是一个跨几百个前驱的
phi，值集合退化成 top，于是表形状匹配不上、候选集也算不出来。**这不是轮数问题**：driver 4 轮
就到了真不动点（第 4 轮零新增），320 个 block 全部探到，300 个 brind 确实静态不可解。

问题在于后果：Resolved 门禁对每个未解析 brind 报错 → 整次反编译 exit 1 → **16 秒后零输出**，
320 个已经看懂的 block 一起陪葬。这与 driver 对轮数上限的既有论证（「已经学到的是函数的大部分，
丢掉它去报个失败对谁都没好处」）自相矛盾。

修法刻意**不动 verifier 契约**：`--allow-unresolved` 打开后，`resolve-indirect` 把每个答不出来的
可达 brind 改写成 `Unimplemented` 不透明终结符，名字里带上分支地址和它算不出来的那个表达式。
这在 Resolved 是合法 IL，下游（DCE / stack-prop / copy-prop / structurizer / emit）本来就把
`Unimplemented` 当作「控制流到此为止、效果不可知」处理，一行都不用改。输出里长这样：

```c
__xdec_unimplemented("unresolved indirect branch at 0x1a0ddc to %13805 = load:i64 add:i64(
  val:i64(%58387), shl:i64(select:i64(cmp.lts:i1(const:i64(0x132), ...)), const:i64(0x3)))");
```

结果：exit 1 零字节 → exit 0 的 3.1 MB C，84391 行、5 个 switch、301 处标记出来的洞。
默认行为不变（仍然失败），没有任何东西会悄悄降级。`unresolved_branches` 进了评分指标，
manifest 里锁在 301——它只许降不许升，这就是「数据依赖 brind 解析」这条线的进度尺。

顺带修掉一个会造出**错误 CFG 边**的问题：表项为 0 时，`isCode()` 会放行——共享库的第一个
段可以从 vaddr 0 开始且映射为可执行，于是链接器留下的空槽被当成合法目标，CFG 上凭空长出
一条指向 ELF 头的边（输出里就是 `case 307: goto L_0x0;`）。现在空槽既不算目标、也不算表的
结束标记：不算目标是因为 0 不是代码；不算结束是因为「链接器没填的槽」说明不了表在哪结束，
把它当结束正是无界枚举在别处明确拒绝做的那种猜测（第一版改成「当结束」，`sample_jni_onload`
当场回归——无界扫描提前停在空槽上，把前面一段错误前缀当成了目标集）。修完 block 数 323 → 320。

### Phase A4：Select 展开到赋值，以及它**不该**展开的地方

`printSelectReturn` 只认 return 位置的 Select，赋值位置的一律印成三元。补上之后：

| | jni_onload | mega_dispatcher |
|---|---|---|
| ternaries | 17 → **13** | 6690 → **3623** |
| ifs | 3 → 12 | → 3835 |

改动本身很小，真正需要想清楚的是**边界在哪**。把 select 链拆解并提前命名的那一步
（`flattenSelect`）两个形态共用——这一步的顺序是有讲究的：表达式打印器会在共享子表达式
**第一次被用到**的地方把它提到临时量，如果第一次使用发生在某个 guard 里面，不走那条臂的路径
就会读到一个没人赋值过的临时量。所以命名必须全部发生在第一个 `if` 之前，这个约束放在一个
函数里，而不是让每个调用点各自记得。两个形态的**输出**不共用：return 可以用
`if (c) { return a; } return b;`（return 会离开语句），赋值必须显式 `else`。

接进了三个赋值点：`Store`、`WriteReg`、以及 phi 的边拷贝。剩下的三元刻意没动：

- **嵌在更大表达式里**（jni 6 处，mega 691 处），典型是 `t = *(uint64_t*)((clamp << 3) + base)`
  里的 clamp，和 `x ^ (c ? y : 0)` 这种掩码选择。它们不是语句，要展开就得先把 select 提成
  一个临时量——那是在无中生有一条代码里没有的语句。而且 `x ^ (c ? y : 0)` 本来就是源码的样子。
- **被提成共享临时量的**（jni 6 处，mega 2030 处），典型是 `_cse2941 = (c ? 0x7f : 0x73);`。
  这类「二选一取常量」写成三元本来就比五行控制流好读，展开是退步不是进步。
- **switch 判别式里的**（5 处）：展开等于把整个 switch 复制一遍。这里的 clamp 该走的是
  「识别成有界索引」那条路（跳转表方向），不是 A4。

也就是说 13 和 3623 这两个数不是「还没做完」的余量，是这条路线的下界。顺带发现两个可以单独
处理的问题：同一个表达式在不同 scope 里会被重复提成两个临时量（`_cse2941`/`_cse2942` 内容
完全相同）；以及 `t49 = *(uint64_t*)(((0xf < 0x5 ? 6 : 5) << 3) + 0x1e14d0)` 这种条件两侧
都是常量却没被折叠的——algebra 的常量折叠只看表达式树，而这里的常量是发射器内联 `val(%N)`
定义时才显现的，两者都不在 A4 范围内。

---

## afRDLog 回归（`Java_com_appsflyer_internal_AFb1nSDK_afRDLog` @ `0x841ac`）

```powershell
$env:XDEC_LOG="driver=info,emit=debug"
.\build\dev\bin\xdec.exe decompile <libsdk_bc_lib.so> 0x841ac --rounds 8 -o build\out-afRDLog.c
```

### 本次基线（2026-08-04）

| 指标 | 值 |
|------|-----|
| 总耗时 | **47.7s**（此前：>46 分钟且不出结果） |
| 收敛 | 第 6 轮，`0 new address / 0 new edge` |
| 规模 | 1 + 1518 entry，2882 block，84101 行，2.0 MB |
| 阶段耗时 | rounds ≈14s/轮，structure 1290ms，print 253ms，vars 37ms |
| 结构化 | 4 switch、199 while、676 if、1010 三元 |
| 未结构化 | **3642 goto、1882 labeled block**（占 2882 块的 65%） |

### 只有大样本才暴露的两个问题（本轮已修）

**1. `vars` 在平坦化函数上指数爆炸** —— `readWidth` 只有深度上限、没有 visited 集合。
dispatcher 一解析开，合并点就出现「每个活寄存器一个 phi、每个 case 一个 operand」的宽 phi，
同一个值会出现在同一个 phi 的几百个 operand 上，于是 `merges` 里同一个 phi 被记几百次，
重走代价是「扇出 ^ 合并链长度」。改成按 merge 去重 + visited 集合后，`vars` 从「跑不完」变成 37ms。
回归测试：`tests/analysis/test_variables.cpp` → *a merge chain wide enough to be a dispatcher's still resolves*。

**2. driver 收敛判定会永远认为「有进展」** —— 原判据是「本轮 resolutions 集合 != 上轮」。
间接分支的 target 列表在轮间会重排或增减，于是判据恒真，8 轮预算烧光后**报错并丢弃全部结果**。
改成按「边」单调累积：每轮把证到的 `(branch, target)` 并入已知集合，只有真出现新边才算进展；
同时轮数用尽不再报错，而是照常发射并置 `DriverReport::converged = false`，CLI 打印
`(round cap reached; coverage may be partial)`。
回归测试：`tests/decompile/test_driver.cpp` → *a run the round cap cuts short still yields what it proved*。

### 顺带修掉的一个求值器 bug（2026-08-05）

写「高半比较」化简规则（`matchShiftedCompare`，见 `docs/07-syscall.md`）时发现：
`il/ceval.cpp` 里六个整数比较用**结果宽度**给操作数掩码，而比较的结果是 1 bit，
于是 `4 == 6` 会折成 true。影响面是所有走 `tryEvalConst` 的常量比较折叠（含分支判定）。
已改为按操作数宽度掩码，回归测试：`tests/passes/test_algebra.cpp` →
*the constant evaluator compares whole operands, not result widths*。
这个 bug 之所以一直没暴露，是因为之前没有任何测试对纯常量比较做过折叠断言。

### 做得好的地方

- 间接调用带解释性注释：`/* indirect call: target = load(v + i*0x5d0 + j*0x8) ^ 0x66908 (encrypted dispatch table) */`
- 13743 处 `_cse` 引用：巨型 MBA 表达式已被拆成命名中间量，不再是单行几千字符

---

## 下一步（按优先级）

1. **平坦化结构化**（最大缺口）：3642 goto / 1882 label 说明 structurizer 对这个形状基本没生效。
   1494 路 dispatcher 现在散成 goto，应当收成一个 switch。方向：把 dispatcher 的 state 变量
   识别出来，按 state 值重建 `while(true) switch(state)`，而不是依赖 CFG 形状匹配。
2. **1518 个 entry 是否合理**：2882 block / 1519 entry ≈ 1.9 块每 entry，说明多半是极小的桩块。
   需要判断这些 target 属于本函数的跳转表还是尾调用到别的函数——后者应当作为 call 而非 entry。
   `recover-tailcall` 已经吃掉其中「目标是调用方指针或 import 槽」的那部分；剩下的是
   「基址是本镜像常量、但表解不开」的分派，仍归 1（平坦化结构化）与跳转表枚举。
   仓库里的混淆样本 `finetuning/tests/fixtures/libtarget.so`（`JNI_OnLoad` @ `0xe4c8c`）
   是这条判定的反向体检：13 处间接分支全是 `ldr x9,[x21,x9,lsl#3]; br x9` 的寄存器基址分派，
   一处都没被误判成尾调用（`XDEC_LOG=tailcall=debug` 可逐条看原因）。该函数原先要 `--rounds 24`
   才收敛，Phase A2 之后 8 轮即可（见上方 L1 一节）。
3. ~~**三元仍有 1010 处**：`printSelectReturn` 只处理 return 里的 Select，赋值语句里的还没展开。~~
   Phase A4 已推广到赋值（见下方 A4 一节）；剩下的三元都是嵌在更大表达式里的，不是语句。
4. **structure 1290ms**：目前不是瓶颈，但已是最慢的单个阶段，等 1 修完后重新测。
5. **稀疏 switch 语义注释**：低优先级。

---

## 扩展语料

在 `corpus/source*.c` 增加 `eval_*` 函数（`build.ps1` 按 `source*.c` 通配编译），
在 `manifest.json` 登记期望，重新 `build.ps1 && run.ps1`。
需要外部信息的 case 记得同时写 `expect_baseline` 与 `expect_typed`，并在 `decompile_args` 里
给出该 case 独有的命令行参数。

待补：afRDLog（生产样本）上的 syscall / 类型导入指标。样本不在仓库里，现有 47.7s 基线是纯
baseline 模式的数字；`svc` 计数与 `--types` 对签名的影响需要拿到样本后单独测一轮。

建议下一批：
- NEON 向量（phase-g）
- 64 位乘法/highpart
- 递归（fib）
- setjmp/longjmp 边界

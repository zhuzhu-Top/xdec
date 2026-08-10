# xdec 评测语料（NDK ground-truth）

用 Android NDK 把已知 C 源码编译成 `arm64-v8a` 动态库，再对 `eval_*` 符号逐个反编译，与 manifest 期望对比。

**当前：baseline 96/96、typed 36/36 通过**（2026-08-06）。首次跑分 0/20，中途 7/20，
基础语料修完 20/20，之后加入 syscall（6）、types（8）、tailcall（2）三类共 36 个函数——这 36 个
两种模式都跑，是 typed 模式至今唯一的语料。2026-08-06 按「环境采集优先、零 CLI、一函数一 case」
的方向扩到 96：新增的 60 个全部只标 `modes: ["baseline"]`，`--types`/`--syscall-table` 一律不加，
靠 [`inferTargetProfile`](../src/binary/target_profile.cpp) 见到 AArch64 ELF 就自动挂
`android-ndk` 类型 + `aarch64-linux` 系统调用表；typed 模式的原 36 个因此保持字节级不变。  
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
| syscall | svc_write, svc_gettimeofday, svc_getpid, svc_unknown, svc_nr_from_arg, svc_errno（原 6 个，两模式共测）+ svc_openat, svc_read, svc_close, svc_mmap, svc_clock_gettime, svc_ptrace, svc_getuid, svc_nanosleep, svc_gettimeofday_errno, svc_gettimeofday_libc（新 10 个，baseline only） | `svc` 语义恢复：号码常量折叠、按真实 arity 裁参、未知号码降级、`-errno` 判定；新增的十个把号码表覆盖到 openat/read/close/mmap/clock_gettime/ptrace/getuid/nanosleep，外加 `gettimeofday_errno`——`sub_199214` 那种 errno-store 语法糖，源头这次换成裸 `svc` 而不是 libc 调用，`gettimeofday_libc` 是同一操作走 libc 的对照组 |
| env | property_get_model/sdk/serial, property_find, open_cmdline, read_maps/status/cpuinfo, access_root/data, readlink_exe, stat_libc, gettimeofday, clock_gettime, time, usleep, nanosleep, getpid, gettid, getuid, pthread_self, prctl, log_print/write, dlopen, dlsym, ptrace, syscall_getpid, mmap, getenv（30 个，baseline only） | 逆向里最常见的环境采集单函数调用：一个 case 只调一个 libc/API，逐个验证 PLT 桩解析、参数类型套用（`struct timeval*`、`struct stat*` 等）、`errno` 折叠不误伤 |
| import | errno_call/fold/dispatch, stack_chk, abort, write_direct, got_indirect, snprintf, strlen, memcpy, strcmp, fopen（12 个，baseline only） | import 解析本身的机制：`errno` 折叠（含穿过一个真 `goto`+合并块的多分支形态）、`__stack_chk_fail`、直接 PLT 调用（非尾调用）、GOT 间接调用（非尾调用）、variadic 签名的天然限度 |
| jni | find_class, get_object_class, get_method_id, call_object_method, call_int_method, new_string_utf, register_natives, exception_check（8 个，baseline only） | `JNIEnv` 虚表间接调用：`android-ndk.hdecl` 按真实 `jni.h`（ABI 自 1.2 冻结）登记了这 8 个成员的偏移，验证「基址解引用 + 固定偏移取函数指针 + `blr`」这条链路解得对，且不会被误判成尾调用（各 case 都在返回值上做了一次编译器无法证伪的哨兵比较，防止 `-O1` 把 `blr` 合并成 `br`） |
| types | types_struct_arg, types_struct_field, types_enum_switch, types_typedef_chain, types_fn_ptr, types_return_struct, types_extern_global, types_void_ptr | 外部头文件类型导入：签名、字段名、枚举、typedef 链、函数指针 arity |
| tailcall | tailcall_table, tailcall_import | 间接尾调用：经调用方指针数组、经 PLT/GOT 到别的模块（见 `docs/08-tailcall.md`） |

源码：`corpus/source.c`、`corpus/source_syscall.c`（原 6 个 `svc` case，两模式共测）、
`corpus/source_syscall_env.c`（新增 10 个 `svc` case，baseline only）、`corpus/source_env.c`、
`corpus/source_import.c`、`corpus/source_jni.c`、`corpus/source_types.c`、`corpus/source_tailcall.c`  
类型头：`corpus/types/eval_types.hdecl` —— 既是编译语料用的 C 头，也是反编译时 `--types` 导入的头，
所以「真值」和「被测输入」不可能各自漂移；`types/presets/android-ndk.hdecl` 同理，扩了
`__system_property_get/find`、`getenv`、`readlink`、`stat`、`fopen` 系、以及真实布局的
`struct JNINativeInterface`（8 个具名成员，其余按 `jni.h` 表序用等宽 `void*` 占位）。  
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

## 2026-08-06：环境采集优先扩容（36 → 96）

按「启动参数尽可能少、case 尽可能多、优先环境采集」的方向分五个阶段把语料从 36 扩到 96，
全部新 case 只标 `modes: ["baseline"]`，不新增任何 `decompile_args`，也没有往 typed 模式的
`$TypedArgs` 里叠 `android-ndk`——原有 36 个 typed case 因此逐字节未变。

- **Phase 0**：`android-ndk.hdecl` 补上 `__system_property_get/find`、`getenv`、`readlink`、
  `stat`、`fopen` 系；`score.py` 加 `plt_sub_calls`/`max_plt_sub_calls` 和
  `import_comments`/`min_import_comments` 两组指标，专门盯「PLT 桩解出来了没」。
- **Phase 1**（`source_env.c`，30 个）：逆向里最常见的环境探针，一 case 一 API——系统属性、
  `/proc` 与文件路径、时间/sleep、身份/线程、日志与 `dlopen`/`dlsym`、`ptrace`/裸
  `syscall`/`mmap`。每个 case 都在调用结果上做一次真实运算（比较、算术）而非直接
  `return call(...)`，否则 `-O1` 会把它折成尾调用，测的就变成 `recover-tailcall` 而不是
  PLT 解析本身。
- **Phase 2**（`source_import.c`，12 个）：import 解析机制本身——`errno` 折叠（含穿过一个真
  `goto` 和共享合并块的多分支版本，对照 `docs/10-import-resolution.md` 里 `sub_199214` 的
  形状）、`__stack_chk_fail`（为此在 `build.ps1` 全局开了 `-fstack-protector-strong`，验证过
  不影响其余 case）、`abort`、非尾调用的直接 PLT 调用与 GOT 间接调用、`snprintf` 这类
  variadic 签名的天然限度。
- **Phase 3**（`source_syscall_env.c`，10 个）：裸 `svc` 覆盖到 `source_syscall.c` 原六个
  之外的号码——`openat`（AArch64 没有裸 `open`，凡 `open` 落到内核都是这个号）、`read`、
  `close`、`mmap`（六参数，验证 wrapper 链没有在 x0..x5 上漏参）、`clock_gettime`、`ptrace`、
  `getuid`、`nanosleep`，以及旗舰 case `eval_svc_gettimeofday_errno`——`sub_199214` 的
  errno-store 语法糖，这次源头换成裸 `svc` 而非 libc 调用，证明这条折叠对「值从系统调用直接来」
  同样生效；`eval_svc_gettimeofday_libc` 是同一操作走 libc 的对照组，两者在 manifest 里相邻。
- **Phase 4**（`source_jni.c`，8 个，优先级最低）：`JNIEnv` 虚表间接调用。没有真实 `jni.h`
  可用（standalone NDK 工具链不带），所以 `source_jni.c` 自带一份本地类型定义，布局和
  `android-ndk.hdecl` 新增的 `struct JNINativeInterface` 逐字节对齐——按真实 `jni.h` 表序
  （ABI 自 1.2 冻结）用等宽 `void*` 占位，只给八个被测成员（`FindClass`=6/`0x30`、
  `GetObjectClass`=31/`0xf8`、`GetMethodID`=33/`0x108`、`CallObjectMethod`=34/`0x110`、
  `CallIntMethod`=49/`0x188`、`NewStringUTF`=167/`0x538`、`RegisterNatives`=215/`0x6b8`、
  `ExceptionCheck`=228/`0x720`）具名。指针类返回值的 case 一开始都写成
  `return x ? x : 0;`——语义上是恒等映射，`-O1` 认出来直接把 `blr` 折成 `br`，反而测成了
  `recover-tailcall`；改成跟一个编译器无法证伪的哨兵比较（`(uintptr_t)x == (uintptr_t)-1`）
  才稳定产出真正的间接调用。这批 case 不要求印出成员名——那需要 case 函数自己的签名被
  某个 `--types` 头登记，而 zero-CLI 的 baseline 模式无从提供；这里锁的是偏移算术和间接调用
  识别本身，`eval_jni.hdecl` 风格的具名调用留给以后接上 typed 模式再做。
- **Phase 5**：`baseline.json` 用 `run.ps1 -UpdateBaseline` 重新生成（96/96），`typed_baseline.json`
  未变（`run.ps1 -Typed` 仍 36/36 且 `fixed`/`regressed` 均为空），`xdec_tests.exe` 全量
  455 个测试用例、121817 个断言在改动后仍然全过。

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

## 2026-08-07：bc_lib OLLVM 反编译质量优化方案，Phase 0-5

目标不是去混淆还原原始逻辑，是让 `afRDLog` 这类 OLLVM 平坦化函数的输出**完整、可审查**——
state 变量、hub 循环、handler 分派看得出来。方案的 7 条主线（详见对话记录，未落盘为文件）
分 Phase 0-5 实施，`samples/manifest.json` 的 `sample_afRDLog` 是每个 phase 都要跑一遍的 L2 门禁。

### 结果总表（`sample_afRDLog` @ `0x841ac`，L2 baseline 见 `samples/baseline.json`）

| 指标 | 方案起点（2026-08，见 `samples/manifest.json` 旧 `expect`） | Phase 0-5 后 | 说明 |
|------|------|------|------|
| 行数 | 10,770 | **8,207** | 主要来自声明分组（下） |
| goto | 319 | 319 | 见下方「没动的地方」 |
| switch | 2 | 2 | 同上 |
| `/*undef*/` | 290（历史峰值；本次起点已是 16，Phase 1b 早前已落地） | 16 | 未再退化 |
| encrypted dispatch call | 218（历史峰值；起点 80） | 80 | 运行时 base，见下 |
| `state` 命名 | 正确 | 正确 | Phase 1a 已落地，本轮未改动 |
| L0（96 eval）/ L1（libtarget 2 case） | 全过 | 全过 | 每个改动后都跑过一次 |

### Phase 4：声明压缩 + 不透明谓词折叠 + switch 判别式注释

**声明压缩**（`src/emit/c_printer.cpp`）：同类型的连续 `temp`/`tempNames`/`cseTemps` 声明
合并到一行（每行最多 12 个名字），例如 `uint64_t t1, t2, t3;` 取代三行。这是本轮唯一移动了
「行数」指标的改动：10,770 → 8,207（**-24%**）。

**不透明谓词折叠**（新增 `algebra_idioms.cpp` 的 `matchCancelledSubtrahend`，接入
`algebra.cpp::rewriteCompare`）：`(a + (k1 - x)) == (k2 - x)` 折成 `a == (k2 - k1)`，对 `!=`
同理——减法里被减的 `x` 在等式两边严格相消，跟 `x` 是什么无关（对有序比较不成立：见
`algebra_idioms.h` 里该函数自己的说明）。bc_lib 里这曾是 do-while 循环的判据本体：

```c
// 折叠前
if (!(((t9 + (0x898048e0df683786 - t766)) == (0x898048e0df6837a1 - t766)))) { ... }
// 折叠后
if (!((t9 == 0x1b))) { ... }
```

行数指标没动（if 语句还是一行），但可读性是真实的——巨大的不相关常量消失了，循环判据变成
一个能一眼看出边界的整数比较。71 处 `0x898048e0df683786` 常量的引用降到 29 处（还留着的是
真正参与自增计算的，不是判据里的裸值）。回归测试：`tests/passes/test_algebra.cpp` →
*algebra tier: a shared subtrahend cancels out of an equality*。

**switch 判别式注释**（`src/emit/c_stmt.cpp::printSwitch`）：table-mode switch 的 case 标签
本来只是表序数（`case 0:`），跟反汇编或图形视图对不上号，尤其是 `claimCaseBody` 把 handler
内联进 case body 之后，序数对应哪个地址就无处可查。现在每个 table-mode case 都带上目标块地址：

```c
case 0: /* handler @0x1992ec */
```

### 没动的地方，以及为什么

**goto/switch 没变**：afRDLog 的控制流是「多个分派点交织」的形态，不是单一 hub-and-spoke——
多数 labeled block 有真实的多个前驱（`claimCaseBody` 正确拒绝内联），现有的 2 个 switch 已经是
局部 compare-chain 的全部，不是平坦化的主干。Phase 3 的 hub 检测/`wrapAsLoop` 扩展对
`sample_jni_onload`（单一 hub）有效（goto 6→5），对 afRDLog 无效——这不是本轮要修的缺口，
是「多 hub 交织」需要更通用的分派检测，留给后续。

**encrypted dispatch call 没变**：80 处里的 base 地址来自运行时解析的全局（写内存/GOT），
`resolve_call.cpp` 的 `ImageEval` 只在整条链（base、index、编码目标、key）都能从**只读**内存
证明时才静态求值——这是正确性要求，不是实现缺口，见 `tests/passes/test_resolve_call.cpp` 新增的
*an encrypted dispatch table with a knowable base still resolves*（同形状但 base 可读时确实会解）。

### Phase 5：driver discovery 硬上限

`sub_627ac`（本方案最初发现的反面教材，见「afRDLog 回归」一节之前的分析）不是真函数入口，
但没有 fence 时驱动会无差别地把发现的每个地址都当成新 entry 去 lift——第一轮就发现 1349 个，
最终 44,786 行。Phase 0 已经给它加了「>512 个新地址且无 fence」的告警，但只是告警，仍会
全部 lift。本轮加了 `DriverOptions::maxTotalEntries`（默认 512，与告警阈值同一个量级）：
一旦本轮新地址会把累计 entry 数推过这个数，超出部分被跳过并降级告警（"hit the N-entry hard
cap"），run 仍然完成——被跳过的分支永久留在未解析状态，最终 verify 阶段会因为大量 block
不可达而诚实失败（`il/verify.cpp` 已有的 `unreachable from the entry` 检查，不是新逻辑）。

`0x627ac` 实测：38,722 行降到 **586 行**（外加 exit 1——这是期望的：一个不是真函数入口的地址，
现在快速诚实失败，而不是安静吐出四万行垂圾）。真实样本（afRDLog 只用 7 个 entry、
`sample_jni_onload` 用 9 个）离 512 的上限有两个数量级的余量，不受影响——L0/L1/L2 全过。
回归测试：`tests/decompile/test_driver.cpp` → *a run capped mid-round on total entries still
finishes*（两个独立、都无需 discovery 就能触达的间接分支，同一轮各报一个新地址，验证上限
准确只放行其中一个）。

### 回归门禁

`samples/manifest.json` 的 `sample_afRDLog` 阈值已按本轮实测收紧（`max_gotos` 340→330、
`max_undef` 25→20、`max_unnamed` 5→4、`max_encrypted_dispatch_calls` 90→85），
`samples/baseline.json` 已用 `-UpdateBaseline` 刷新。`eval/` 96 个 L0 case 与 `samples/` 的
`sample_jni_onload`/`sample_mega_dispatcher`（L1）在每一步改动后都跑过，全程无回归；
`xdec_tests.exe` 全量 459 个测试用例、131,054 个断言通过。

## 2026-08-10：mega-block local-simplify hang（`bc_lib` `0x2a2428`）排查手册

`0x2a2428` 是同一个 bc_lib 里另一段代码（用户经反偏移确认为核心算法的一部分），Binary Ninja
能反编译，`xdec decompile`/`observe --to local`（及更高 maturity）却挂起数分钟到数十分钟，
不产出任何 `.c`。这不是 discovery 爆炸类问题（对比 Phase 5 的 `sub_627ac`），排查方式也不同，
记在这里备查。

### 先确认不是 discovery/lift 问题

```powershell
xdec observe libsdk_bc_lib.so 0x2a2428 --rounds 1 --to lifted
```

若这一步在 1 秒内完成、`observe-2a2428/00-lifted.il` 里只有个位数 block（本例是 4 个），
说明 lift 本身没问题——`0x2a2428..0x2a443c` 反汇编后 2055 条指令里只有 1 条分支
（`b 0x2a52a4`），中间全是直线 MBA 运算，lift 把它们合成一个 ~5394-op 的巨大 basic block
是**正确**的 CFG，不是切块失败。真正的问题在下一步。

### 定位卡在哪个 pass、哪个子步骤

```powershell
$env:XDEC_LOG="pass=debug,local=debug,algebra=debug"
xdec observe libsdk_bc_lib.so 0x2a2428 --rounds 1 --to local
```

三个类别配合看：

| 类别 | 打印时机 | 用途 |
|------|----------|------|
| `pass=debug` | fixpoint pass 每轮迭代**完成后立即**打印（`manager.cpp`），单轮 ≥30s 额外 `WARN` | 判断卡在哪个 pass、第几轮迭代；`WARN` 直接给出 op 总数与最大单 block op 数 |
| `local=debug` | `local-simplify` 每个子步骤（algebra/fold/flags/copy/loads/dce）**跑完立即**打印，≥256-op 的大 block 逐块打印 copy/loads/dce 耗时 | `local-simplify` 内部具体是哪一步（`local_simplify.cpp`） |
| `algebra=debug` | `simplifyAlgebra` 每次调用打印 touched op 数、walk 次数、memo 命中、intern 次数、expr 池前后大小 | 判断是否是代数化简本身在膨胀表达式池（本例排除） |

由于日志是子步骤**完成后**才打，hang 在某一步时，**最后一条日志就是上一个完成的步骤**，
下一步没打印就是卡点。本例实测：

```
[debug pass]     local-simplify start 4 block(s) 5394 op(s) 6837 expr(s)
[debug algebra] 1249 op(s) touched, 8895 simplify walk(s) (2059 memo hit(s)), 1242 intern(s), 6837 -> 8283 expr(s)
[debug local]   algebra 3ms changed
[debug local]   fold 1ms
[debug local]   flags 1ms
（10 分钟无更多输出 —— 卡在 b0 的 copyPropagateBlock）
```

`algebra`/`fold`/`flags` 全部 ≤4ms，说明代数化简与常量折叠不是瓶颈；下一步该打的
`b0 @0x2a2428 ... op(s): copy ...ms` 始终没出现，说明卡在 `copyPropagateBlock`。

### 用小规模入口做对照，确认复杂度曲线

不用等大 block 跑完，从函数中段挑几个不同大小的入口分别计时，能看出复杂度是否超线性：

```powershell
foreach ($addr in @('0x2a4200','0x2a4100','0x2a4080')) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  xdec observe libsdk_bc_lib.so $addr --rounds 1 --to local | Out-Null
  Write-Output "$addr -> $($sw.Elapsed.TotalSeconds)s"
}
```

实测（同一份 bc_lib）：

| 入口 op 数（约） | `local-simplify` 总耗时 |
|------|------|
| ~128 | 0.27s |
| ~470 | 50s |
| ~544 | 133s |
| ~5394（完整 `0x2a2428`） | >10min（未收敛） |

470→544 op（+16%）耗时从 50s 到 133s（+166%），远超线性，指向 `copyPropagateBlock`
（[`copyprop.cpp`](../src/passes/copyprop.cpp)）对每个 op 的每个 operand 调用
`ValueSubst::apply()` 做全树展开——直线代码里 subst 链随 op 数线性增长，每次展开
都要重新走一遍累积的树，整体退化到 O(n²) 以上。`dceBlock`
（[`dce.cpp`](../src/passes/dce.cpp)）对每个 op 做 `collectValueUses` 全块树遍历，
同一类问题但量级较小（604 op 时 dce 758ms，copy 未到千级前不明显）。

### 排查结论一览（判定树）

1. `observe --to lifted` 秒级完成、block 数正常 → 排除 discovery/lift
2. `pass=debug` 显示卡在哪个 pass 的第几轮迭代（若单轮 ≥30s 会自动 `WARN` 报最大 block
   op 数，不用等 hang 完再猜）
3. 若卡在 `local-simplify`：看 `local=debug` 最后一条子步骤日志，通常是 `copy` 或 `dce`
4. 若卡在 `ssa-optimize`：看 `optimize=debug`（`ssa_optimize.cpp` 已有 flags/sccp/
   algebra/phis/dce 五段计时，同样的判定法）
5. 排除代数膨胀：`algebra=debug` 的 expr 池前后大小若没有数量级增长，说明不是规则
   互相触发的化简爆炸，而是遍历算法本身的复杂度问题

### 结论

超大直线 MBA block（无内部分支，5000+ IL op）触发了 `local-simplify` 里 `copyprop`/`dce`
两个 block-local 变换的超线性复杂度，这是**性能 bug**，不是这类输入本身反编译不出来——
Binary Ninja 能处理同一段代码，说明它的等价变换没有这个复杂度陷阱。

### 修复：快修 + 根治（2026-08-10 落地）

**快修**（[`transform.h`](../src/passes/transform.h) 的 `kMegaBlockOpThreshold`，
[`local_simplify.cpp`](../src/passes/local_simplify.cpp)）：block op 数超过阈值时，
`local-simplify` 跳过该 block 的 `copyPropagateBlock`/`forwardRedundantLoads`，只保留
`dceBlock`。这一项独立就把 `0x2a2428` 的 `local-simplify` 从 >10 分钟压到 20ms，是最先
落地、验证问题定位是否准确的一步。

**根治，两处独立的复杂度源**：

1. **`copyprop.cpp` 的 `BlockProp::write`**：`copyPropagateBlock` 会把每次写寄存器时
   完全替换过的表达式树整个记进 `contents`，下一次这个值被其它写用到时，又把这整棵树
   嵌进新树里重新 `intern`——直线代码里这棵树跟着 op 数线性变大，是 O(n²) 的来源。修法是
   `boundedExprNodeCount` 在 `write()` 时给树size封顶（`kMaxTrackedExprNodes = 64`），
   超过阈值就放弃精确跟踪（等价于现有"不确定的部分写"分支，直接 `contents.erase`）——
   丢的只是"跨越一个已经很大的中间值"的传播机会，正确性不受影响（未被内联的 Value 仍然
   是 DAG 里合法的叶子，指向仍然存活的 ReadReg）。
2. **`dce.cpp` 的 `collectValueUses` 与 `ssa_optimize.cpp` 的 `collectUsed`**：两处都是
   对表达式 DAG 的递归遍历，只在碰到 `Value` 叶子时去重，同一个被多处引用的复合子表达式
   （代数化简后极常见）会被从每个引用处重新完整遍历一次——DAG 越"钻石"，这个重复就越接近
   组合爆炸而不是单纯冗余。修法是加一个按 `ExprId` 记的 `visitedExprs` 集合，进入非叶子
   节点前先查重；因为表达式是无环的，先标记再递归是安全的（不会漏掉任何一次首次访问）。

**效果**（`bc_lib` `0x2a2428`，参数一致，仅改这次修复涉及的四个文件）：

| 阶段 | 之前 | 之后 |
|------|------|------|
| `local-simplify`（大 block skip 生效） | >10 分钟未收敛 | 20ms |
| 完整 `xdec decompile ... 0x2a2428`（发现到 163 个额外 block，共 169 个） | 从未产出 | 4.8s，退出码 0，5595 行 C |
| `copyPropagateBlock` 在原始 5394-op block 上单独跑（结构修后，不跳过） | （原来会是主因） | ~3-17ms/轮，且反而把 block 从 5394 op 精简到 993 op |

结构修（size-capped copyprop + 记忆化 dce/collectUsed）落地后，`local-simplify` 已经能在
不跳过 `copyPropagateBlock` 的情况下正常处理这个 block（见上表第三行），于是
`kMegaBlockOpThreshold` 从"主要防线"降级为兜底 fuse，数值从最初的 512 上调到 16384——
留给未跑过的更极端输入一层保险，而不再是日常路径依赖它。

**回归**：`xdec_tests.exe` 462 个用例（新增 3 个覆盖直线累加链 + 跨 mega-block 阈值 +
共享子表达式 DCE 的用例）、131,069 个断言全过；`eval/` baseline 96/96、typed 36/36 无回归；
`samples/` 的 `sample_jni_onload`/`sample_mega_dispatcher`/`sample_afRDLog` 无回归，新增
`sample_core_mba`（`0x2a2428`，L2，`straight-mba` 类别）4/4 通过，基线已用 `-UpdateBaseline`
刷新。

## 2026-08-10：dispatcher handler 内联之后的 `tN = tM` 拷贝噪声（Live Register Frame，F0-F4）

162 个 case 内联进 `switch`、共享 tail 收敛成一份 epilogue 之后，`0x2a2428` 仍有
`t8 = t0; ...; t15 = t7;`（case 末尾）与 `t0 = t8; ...; t7 = t15;`（epilogue 内）两种
八行一组的拷贝，约占输出总行数的 23%。最初按寄存器编号猜测这是 OLLVM 常见的
"x0-x7 活跃寄存器 / x8-x15 影子备份" 两组寄存器банк，但对 [`ssa_construct.cpp`](../src/passes/ssa_construct.cpp)
的 phi 加 `reg:xN` 注解后实测发现：hub 和 merge 两处的 phi 标注的是**同一批寄存器**
（都是 x0-x7），不存在真正的影子寄存器组——这只是同一个寄存器在 hub、merge 两个
phi 汇合点上的"两级接力"：每个 handler 把值写到 merge 的 phi（对应输出里的
`t8..t15`），merge 再把它接回 hub 的 phi（`t0..t7`），`printEdge`（[`c_stmt.cpp`](../src/emit/c_stmt.cpp)）
把这两级 phi 的每条边都如实打印成一行赋值，于是每个 handler 都反复重复同一份"保存/
恢复"协议。

**F0**（[`live_register_frame.h/.cpp`](../src/analysis/live_register_frame.cpp)）：
`matchLiveRegisterFrame` 扫描 hub 的每个带 `reg:xN` 注解的 phi，在 merge 里找同一寄存器
的第二个 phi，配对进 `LiveRegisterFrame::slots`；`classifyHandlerExit` 判定某个 handler
在 merge 侧的 phi 操作数是否等于 hub 侧 phi 的结果值本身（`Passthrough`/`Partial`/
`Return` 三态）。

**F1**（`c_stmt.cpp` 的 `printEdge`/`printFrameSeed`）：进入带 frame 的 switch 前，先把
`shadow[i] = live[i]` 打印一次（`printFrameSeed`），建立"没被哪个 handler 改动的 slot，
shadow 值仍等于 live 值"这一不变量；随后每个 handler 落到 merge 的边拷贝，凡是
`classifyHandlerExit` 判定为 unchanged 的 slot 一律跳过。`0x2a2428` 从 5303 行降到 4107 行。

**F2**（[`variables.cpp`](../src/analysis/variables.cpp)）：给 dispatcher 函数里配对出的
hub phi/merge phi 分别起 `xN_live`/`xN_exit` 语义名，取代无意义的 `tN` 序号，可读性收益，
不改变行数。

**F3**：原计划的"IL 层 shadow phi 消除 pass"建立在错误的双寄存器组假设上，重新定性后
改为一个更小但同样成立的加固——`unanimousPassthroughSlots`：如果某个寄存器 slot 在
*所有*落到 merge 的 handler 上都是 unchanged（不只是某一个 handler），那么 merge 侧的
phi 值处处等于 hub 侧 phi 值本身，seed 拷贝和 epilogue 里对应的 restore 拷贝都可以整体
省略，不只是省略某个 handler 的一行。`0x2a2428` 的 8 个寄存器没有一个满足"全体一致"
（case 1 就单独打破了每个 slot 的一致性），所以这一步在这个样本上是零行变化——用
`fc /b` 逐字节比对 F2/F3 两版输出完全一致验证过；这是分析给出的正确判断，不是遗漏，
留给别的、handler 更少或某些寄存器确实从未被任何 handler 触碰的 dispatcher 函数。

**F4**（[`structure.cpp`](../src/emit/structure.cpp) 的 `tryDispatcherLoop`）：guard 越界出口
（`b3`）自己也直接汇入 `merge`，而不是真正离开循环——这正是它会落进 `naturalLoop` 的
`loop.blocks` 里的原因（反向从 latch=`merge` 沿 predecessors 走到 `b3` 时不需要经过
`header`），也正是原先 `!loop.blocks.contains(headerExit)` 判定失败、整个函数拿不到
`while (true)` 包装的原因。修法是把 `b3` 当成一个私有 handler，复用
`claimDispatcherCaseBody`（新增 `appendBreak` 参数，这里传 `false`：guard 分支不在
switch 里，`break;` 会错误地跳出外层 `while`，而不是像 case 里那样落到 epilogue）内联
进 guard 的 `if` 分支，dispatch/switch 挪进 `else` 分支（越界状态不能再跑一遍基于垃圾
下标的 switch），共享 tail 从 switch 自己的 epilogue 挪到 `if`/`else` 之后统一打印一次。
`0x2a2428` 最终 0 goto、0 label，整个函数是一个 `while (true) { if (guard) {...} else
{...switch...} ; tail }`，4106 行。

**回归**：`test_live_register_frame.cpp`（新增 unanimous slot 用例）、`test_structure.cpp`
（新增两个 `tryDispatcherLoop` 用例：普通守卫退出、三向汇合守卫退出）纳入
`xdec_tests.exe`，476 个用例、131,173 个断言全过；`eval/` 96/96、`samples/` 4/4 无回归。
`samples/manifest.json` 的 `sample_core_mba` 阈值按本轮实测收紧：新增 `max_lines: 4200`、
`min_loops_while_true: 1`，`max_gotos` 10→2。

## 2026-08-10：语义 helper 头文件化与短名（`xdec_helpers.h`）

`0x2a2428` 的 F4 输出里，`__xdec_rotr32` 出现约 586 次、`__builtin_bswap32` 约 64 次，
每次调用都比等价的算术表达式长一截，而且每个反编译出的 `.c` 文件的 preamble 都要
重复一份完全相同的 `static inline uint32_t __xdec_rotr32(...) { ... }` 定义——同一个
`xdec` 进程、同一份逻辑，被打印了 N 次。

按「能在 C 里无歧义、可移植地定义的 → 短名 + 头文件里给真身；语义依赖目标/embedder
的 → 保留 `xdec_` 前缀、头文件里只给声明」拆成两类，新增
[`include/xdec/xdec_helpers.h`](../include/xdec/xdec_helpers.h)：

- **真身**（`static inline`，与目标无关，任何输入下都对）：`rotr8/16/32/64`、
  `rotl8/16/32/64`、`bswap16/32/64`、`popcount64`、`cc_{ge,lt,gt,le,vs,vc}{8,16,32,64}`
  （溢出精确条件码，逻辑照搬 `c_helpers.cpp` 原来的 `conditionHelper`/`rotateHelper`，
  现在只有头文件这一份 source of truth）。
- **仅声明**（原来只在用到时打一行注释，现在是头文件里的真实原型）：
  `xdec_clz/ctz/brev{8,16,32,64}`、`xdec_mulhi{u,s}{8,16,32,64}`、`xdec_flagbit`、
  `xdec_flagcond_stub`、`xdec_f{add,sub,mul,div}{32,64}`、`xdec_fneg{32,64}`。
- **不变**（不进头文件，原样保留双下划线）：`__xdec_intrin_*`（每条指令一个不同的名字，
  没有固定原型可声明）、`__xdec_syscall`（这行声明本来就在 `c_helpers.cpp` 里按需打印，
  早于这次改动）、`__xdec_unimplemented`（从来没在任何输出里被声明过）。

顺手修了一个潜伏的小 bug：浮点 stub 的 helper key 原来拼成 `"f" + "fadd32"` =
`"ffadd32"`，但调用点打印的是 `__xdec_fadd32`——key 和实际调用名对不上，只是从未
影响过输出（`helperDeclarations` 里两处判断都只看前缀 `"f"`，凑巧都能命中）。这次连
带 key 一起改成与调用名一致的 `"fadd32"`。

`c_flags.cpp` 的 `ccHelper` 顺带补了一层防御：条件码只有 `ge/lt/gt/le/vs/vc` 六种在
头文件里有定义，如果 `Always`/`Never` 真的以某种方式流到这里（正常不会——它们不依赖
标志位，理论上会在更早的阶段被折叠掉），现在会退回复用已有的 `xdec_flagcond_stub`
埋点而不是拼出一个头文件里根本不存在的函数名。

`COptions` 新增 `helpersHeader`（默认 `"xdec_helpers.h"`，空串表示不 include）；
`xdec decompile` 新增 `--helpers-header <path|none>`；`src/tools/CMakeLists.txt` 给
`xdec` target 挂了个 `POST_BUILD` 步骤，把头文件拷到 `bin/xdec.exe` 旁边，反编译出的
`.c` 不用额外配置 include path 就能直接编译。

**回归**：`test_c_expr.cpp`/`test_c_printer.cpp` 里断言旧名字/内联定义的用例改成断言
新短名 + `#include`；新增 `tests/emit/test_c_helpers_header.cpp`（7 个用例：无 helper
不 include、rotate/bswap/embedder stub 各自触发 include、单独的 syscall 不触发、
`helpersHeader` 可改路径、可清空禁用）。`xdec_tests.exe` 483 个用例、131,190 个断言
全过；`eval/` baseline 96/96、typed 36/36、`samples/` 4/4 全部用 `-UpdateBaseline`
刷新（純粹是 helper 拼写/preamble 形状变化，非语义回归）。`eval/score.py` 的
`intrinsics` 计数从数 `__xdec_` 改成数 `xdec_`（排除 `#include "xdec_helpers.h"` 这行
本身），这样 rotr/bswap/cc_* 不再被算进"还依赖 embedder"的计数——它们现在是完整定义，
真正还依赖 embedder 的只剩 `xdec_` 前缀的 stub 加 `syscall`/`intrin`。
`samples/score.py` 的 `__xdec_` 前缀过滤（防止 helper 定义被误认成目标函数）不用改：
helper 现在只有声明和 `#include`，不再有带函数体的内联定义，这条防线本来就用不上了，
留着无害。

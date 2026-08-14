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

## 2026-08-10：单次栈 Load 物化噪声（stack-load-fold）

`0x2a2428` 的 MBA 直线代码里，一次性读回的栈槽本该直接以变量名出现在算式里，却因为
`nameResultTemps` 无条件给每个 `Load` 分配临时量，印成了 `t293 = var_984; ...;
(*(uint32_t*)(t294)) = (_cse561 + t293);` 这种「先拷贝、再用」的两步——`--reuse-report`
现有的两种可判定形状（`docs/09` 的 A/B）都看不到这个问题：既没有第二个 `ExprId`，也没有
第二次 `Load`，只是**同一次读被印了两遍**。详见 `docs/09` 新增的形状 F 与
`docs/12-stack-load-fold.md`（算法、安全规则、与 `stack_prop.cpp` 的边界）。

### 结果（同一份 `0x2a2428`，参数一致）

| 指标 | 之前 | 之后 |
|------|-----:|-----:|
| 总行数 | 4103 | **3715** |
| `tN = var_XXX;` | 385 | **95** |
| `(*(T*)tN) = ...` | 42 | **1** |
| 提升为指针类型的栈局部变量 | 0 | **10** |

剩下的 95 处 `tN = var_XXX` 是分析故意保守的地方：读者在另一个 block（跨 block 转发不是
本方案目标），或读之前那个 slot 在同一 block 内又被写过一次（新鲜度检查正确拒绝）。

### 实现落点（三层，均为 emit/analysis 层，未新增 IL pass）

- `analysis::findFoldableStackLoads`（新增 `analysis/stack_load_fold.h`/`.cpp`）：判定
  一个 `Load` 是否「地址是栈槽、每个活跃读者都在同一 block 内且在 load 之后、两者之间
  没有可能的写入/调用能改变槽内容」，满足则可折叠；额外标注 `usedAsAddress`（是否所有
  读者都只把结果当地址用，而非取值参与运算）。
- `emit::CContext` 构造函数把折叠结果记进 `inlinedStackLoads`，对应 `Load` 的 `OpId` 并入
  `deadOps`；`ExprPrinter::value`（`c_expr.cpp`）在查普通临时量之前先查这张表，命中就直接
  返回槽位自己的打印文本——`memoryLvalue` 原有的 `(*(T*)...)` 兜底路径不需要单独改动，
  替换进去的名字本身就够。
- `analysis::VariableTable::recover`（`variables.cpp`）复用同一份分析（`deadOps` 传空，
  因为变量恢复跑在 `emit` 阶段填满 `deadOps` 之前）：`usedAsAddress` 为真的栈槽被提升为
  指针类型，`c_stmt.cpp` 的 `Store` 分支相应地在写入槽位本身时补上必要的 `(T*)` 显式转换。

### 回归

新增 `tests/analysis/test_stack_load_fold.cpp`（9 用例：单读折叠、store/call/跨 block 各自
挡住折叠、global 地址不处理、两个活跃读者都能折叠、已死读者不计入、`usedAsAddress` 的
正反两个例子）与 `tests/emit/test_c_stack_load_inline.cpp`（3 用例：标量单读直接印变量名、
仅作地址用的读提升指针类型、两个活跃读者都不需要临时量）。`xdec_tests.exe` 495 个用例、
131,215 个断言全过；`eval/` baseline 96/96、typed 36/36 无回归；`samples/` 4/4（含
`sample_core_mba`，即 `0x2a2428`）无回归。

## 2026-08-10：Emit Redundancy Elimination（ERE）框架 + 写端死 spill 消除（H1）

用户指出 `_cse8 = bswap32(t32); var_aa8 = _cse8;`（`var_aa8` 全文件仅此一次赋值、
再无读者）是 stack-load-fold 没覆盖的**写端**冗余，并要求系统性排查而非逐个模式打补丁。
按「中间变量系统性消除方案」（`中间变量系统性消除_dbc46949.plan.md`）分阶段实施：

**Phase 0（度量基础设施）：** 新增 `analysis::EmitRedundancyReport`
（`include/xdec/analysis/emit_redundancy.h`、`src/analysis/emit_redundancy.cpp`），
IL 层统计栈 Load/Store 折叠比例与 write-only 局部变量数，接入
`xdec decompile --emit-report`；新增 `tools/emit_metrics.ps1` 做文本层互补统计
（`_cseN = ...`、`var_X = _cseN;`、`tN = var_XXX;` 等行数）；`docs/09` 补入形状
G–J（非栈单读 Load、CSE 写端 spill、refCount 虚高、dispatcher relay），新增
`docs/14-emit-redundancy.md` 作为框架总览。

**Phase 1（写端死 spill 消除，H1）：** 新增 `analysis::findDeadStackStores`
（`include/xdec/analysis/stack_store_fold.h`、`src/analysis/stack_store_fold.cpp`）：
一个写栈槽的 `Store`，若全函数没有任何 `Load` 读同一 delta、地址从未逃逸（未被当作
`Call`/`Intrinsic` 参数或另存为某处的值）、不是别名字段、也不是 `VariableTable::recover`
专门提升的 `state` 槽（该槽故意只写不读，见 `variables.cpp` 自己的注释——真实
dispatcher 的状态值常常活在寄存器/phi 里，这个槽存在只是为了让读者看出"这是状态槽"，
折叠掉它的写入会抹掉这个提升唯一的用处），则判定为死，其 `OpId` 并入 `CContext::deadOps`
（`c_context.cpp`）。因为该分析证明的是"整个槽死"而非"某一次写死"，死槽的 delta 额外记入
新增的 `deadLocalStackDeltas`，`c_printer.cpp` 的 `declarations()` 据此跳过声明——
一个再没有赋值也没有读者的局部变量，不该再有声明。

`StmtPrinter::printBlock` 收集 CSE scope roots 时已经先跳过 `deadOps`
再调用 `addExprRoots`（`c_stmt.cpp`），所以 shape I1（死 spill 抬高共享表达式的
refCount）**不需要额外机制**，只需验证：`_cse8` 这类节点在真实语料里全部还有别的
活引用（`0x2a2428` 里 `bswap32(t32)` 除死 store 外还被 3 处 MBA 算式引用），所以去掉
死 store 自己的贡献从未把一个真正共享的节点降格成内联表达式。

### 结果（同一份 `0x2a2428`，参数一致）

| 指标 | Phase 0 后 | Phase 1 后 |
|------|-----:|-----:|
| 总行数 | 3715 | **3446** |
| `var_X = _cseN;`（H1/H2） | 277 | **185** |
| `_cseN = ...`（I3，含 I1 级联） | 1179 | **1138** |
| write-only 局部变量（仅声明、从无赋值或读者） | 105 | **0** |
| IL report：栈 store 判死 / 总数 | 0/665 | **105/665** |

### 回归

新增 `tests/analysis/test_stack_store_fold.cpp`（7 用例：无读者即死、后有 Load 则非死、
地址传给 intrinsic 则非死、地址被存成另一处的值则非死、同槽两次写一起判死、`state` 槽
即使无读者也保留、global 地址不处理）与 `tests/analysis/test_emit_redundancy.cpp`
（IL report 字段的一个综合用例）。三个既有测试
（`test_c_printer.cpp` 的 diamond/assigned-select/nested-ternary 三个用例、
`test_c_expr_reuse.cpp` 的 dispatcher-state-store 用例）原本的栈槽写入在各自 fixture
里从未被读回，恰好落进新折叠的形状——补一次读回（或，对 dispatcher 用例，补一次代表
"下一轮循环读回状态"的 Load）后各自验证的原有断言不变。`xdec_tests.exe` 503 个用例、
131,231 个断言全过；`eval/` baseline 96/96、typed 36/36 无回归；`samples/` 4/4（含
一次因 `state` 槽被误判为死而回归、加上述例外后修复的 `sample_afRDLog`）无回归。

详见 `docs/13-stack-store-fold.md`（算法、安全规则、与 `state` 槽的边界）与
`docs/14-emit-redundancy.md`（ERE 框架总览、分形状进度表）。

## 2026-08-10：读端非栈单读 Load 内联（G）

`docs/09` 形状 G：`t32 = (*(uint32_t*)(a1+0x18)); bswap32(t32)` 这类非栈地址的
单读 Load，与已修的形状 F（栈槽单读）对称，`nameResultTemps` 同样无条件给它
分配临时量。新增 `analysis::findFoldableMemoryLoads`
（`include/xdec/analysis/load_inline.h`、`src/analysis/load_inline.cpp`）：
与 `findFoldableStackLoads` 完全相同的安全规则（活跃读者都在同一 block、都在
load 之后、两者之间无可能的写入/调用），只是把地址类从"必须是 StackSlot"放宽
到"只要不是 StackSlot"——`frame.mayAlias` 本就对 `Global`/`Other` 地址一样保守
地判断别名，不需要为这两类地址单独收紧规则。

栈槽的替换文本（局部变量名）在分析阶段就能定下来；`Global`/`Other` 地址没有
这样的名字，所以 `CContext` 只记下地址 `ExprId` 与宽度（`inlinedMemoryLoads`），
`ExprPrinter::value` 在替换点按 `StmtPrinter::memoryLvalue` 同样的顺序重新
渲染——先试 `fieldAccess`（结构体字段名），再试 `globalName`（具名全局），
最后才是裸的 `(*(T*)...)` 转换。新增一条形状 F 不需要的排除规则：一个读者若把
该 Load 的结果当作*另一次* Load/Store 的地址（`isAddressOperand`），则不折叠——
这正是 `fieldAccess` 靠给基址值查名字来识别 `n->next->value` 链式访问的形状,
折叠掉会把字段名换成裸指针算术,是退步不是进步(用
`tests/emit/test_c_types.cpp` 的"指针字段携带类型"回归验证过这条排除的必要性)。

### 结果（同一份 `0x2a2428`，参数一致）

| 指标 | Phase 1 后 | Phase 3 后 |
|------|-----:|-----:|
| 总行数 | 3446 | **3253** |
| `tN = (*(T*)...);`（G） | 243 | **79** |
| `_cseN = ...`（I3，含 I1 级联） | 1138 | **1131** |

剩下的 79 处与形状 F 剩余的 95 处 `tN = var_XXX` 同类：读者在另一个 block、
两者之间有 clobber，或读者把结果当地址用（`fieldAccess` 命名排除）。

### 回归

新增 `tests/analysis/test_load_inline.cpp`（8 用例：global/arg+offset 单读折叠、
栈槽地址留给 stack_load_fold、store/call/跨 block 各自挡住折叠、当地址用不折叠
及其与纯值读混合的情形、已死读者不计入）与 `tests/emit/test_c_load_inline.cpp`
（3 用例：global 单读内联、arg+offset 单读内联、call 阻断内联）。`xdec_tests.exe`
514 个用例、131,243 个断言全过；`eval/` baseline 96/96、typed 36/36 无回归；
`samples/` 4/4（含 `sample_core_mba`）无回归。

## 2026-08-10：写端 CSE 合并打印（H2）

`docs/09` 形状 H2：一个活着的（后续确实被读的）栈槽 store，其值恰好也是一个
共享节点，于是先被 `ExprPrinter::materialized()` 无条件命名成 `_cseN`，store
再把 `_cseN` 抄进 `var_X`——两行说的是同一件事：

```c
_cse9 = bswap32(t30);
var_abc = _cse9;
```

这与 Phase 1 处理的 H1（写了从不读的 dead spill）不同：这里 `var_abc` 确实有
后续读者，不能整句删掉，能省的只是「先起个 `_cseN` 名字、再抄一遍」这一步。

新增 `ExprPrinter::materializeAs`（`src/emit/c_expr.h`/`.cpp`）：在
`printOp` 的 `Store` 分支里，抢在 `materialized()` 给 store 的值节点分配
`_cseN` 之前，检查这次命名是不是「本作用域第一次给这个共享节点起名」，如果是，
且 store 目标是一个「裸名」局部变量——`lvalue == local->name`，用这一个等式就
排除了字段访问、别名局部、宽度不匹配（这三种情况 `memoryLvalue`/`stackSlotLvalue`
会打印出不同的文本，不等于裸名）——并且没有指针 cast 前缀，就直接把这个节点
命名成 `var_X` 而不是 `_cseN`，一行打印 `var_X = <expr>;`。因为命名结果照旧写进
`ExprPrinter` 自己的 `materializedText_` 缓存，本作用域里对同一节点的后续引用
（`rootText`/`rootInteger`/`materialized` 都先查这个缓存）自动读到 `var_X`，
不会另起 `_cseN`。这是 ERE 框架里目前唯一不挂在 `CContext` 三个字段上的机制：
一个节点算不算"共享"是 `ExprPrinter` 自己按打印顺序做的引用计数，`CContext`
的预扫描分析（`findFoldableStackLoads` 那一类）看不到这个信息，也就没法把这
个判断挪到 `CContext` 里去做。

`0x2a2428` 的效果比预想更好：不少 case 里 store 的值本身就是巨大的 MBA 求和树，
合并后不只省了一行——像 `var_a0c = ((_cse8 + (_cse42 + ...)) + ...);`
这类合并结果，后面一条式子还会直接用 `rotr32(var_a1c, ...)` 读回上一次合并
产生的 `var_a1c`，而不是再起一个 `_cseN`，省的行数比单纯「两行并一行」更多。

### 结果（同一份 `0x2a2428`）

| 指标 | Phase 3 后 | Phase 4 后 |
|------|-----:|-----:|
| 总行数 | 3253 | **3169** |
| `var_X = _cseN;`（H1/H2） | 185 | **60** |
| `_cseN = ...`（I3，含 I1 级联） | 1131 | **1054** |

剩下的 60 处是 `materializeAs` 主动放弃合并的情形：该共享节点在本作用域内已被
更早的语句命名过（名字已经定了，不能事后改），或 store 目标不是纯裸名局部
（指针 cast 槽、字段访问、别名/宽度不匹配局部）。IL 层报告（`--emit-report`）
不变——`materializeAs` 只是打印层重命名，不影响 `findDeadStackStores`/
`findFoldableMemoryLoads` 能从 IL 本身证明什么。

### 回归

修改 `tests/emit/test_c_expr_reuse.cpp` 中一条既有用例（原先断言
`switch (_cse0)` 恰好出现一次；合并后该共享值直接以 `var_10` 命名，`switch`
和回读都直接用 `var_10`，全文件不再出现任何 `_cse`，断言相应更新为检查
`var_10 = (a0 + 0x1000);`、`switch (var_10)` 各恰好一次、`_cse` 出现零次）。
`xdec_tests.exe` 514 个用例、131,243 个断言全过；`eval/` baseline 96/96、
typed 36/36 无回归；`samples/` 4/4（含 `sample_core_mba`）无回归。

## 2026-08-10：跨 scope 重复 expr 检测（I2，仅度量，不做 hoist）

计划把 Phase 5 拆成两半：识别互斥 switch arm 间打印文本相同的 `_cseN` RHS（要做），
以及把它 hoist 到 dispatcher loop 共享 prologue（计划自己标注为"可选 emit"）。
这里只做前一半。

给 `tools/emit_metrics.ps1` 加了 `duplicate_cse_rhs_groups` /
`duplicate_cse_rhs_occurrences`：按 `_cseN = <expr>;` 这一行的 RHS 原始打印
文本分组，统计有多少组出现 ≥2 次、总共出现多少次。`0x2a2428` 上是 **19 组、
181 次**，和方案原文的估计（"20 种 RHS 重复 ≥2 次"）量级一致。

没有做实际的 hoist，是有意的决定，不是因为工期不够简单跳过：Phase 1–4 的每
一步都是"从 IL 事实能直接证明安全"的纯打印决策（一个 load 的读者集合、一个
store 的读者集合、一个 scope 自己的引用计数），出错了最多是少省几行；跨
scope hoist 不一样，它是真正的代码搬移——把只在某几个互斥 switch arm 里执行
的计算，搬到一个所有路径（包括原来不执行这段计算的路径）都会经过的共享
prologue 里。方案自己写的安全约束（docs/09 形状 D）要求证明"所有到达这个
switch 的路径都已经执行过等价计算"，这是一个全函数可达性论证，不是
`ExprPrinter` 现在这套按打印顺序做的逐 scope 引用计数（Phase 1–4 全部机制的
共同基础）能回答的问题。证明做错了是悄悄改变行为，而不是只影响可读性，风险
类别和这个方案里其他形状都不一样。鉴于方案原文本就把这步标成可选，而上面的
统计已经回答了"还剩多少"，这里选择把 hoist 留作后续工作，而不是在没有把安全
证明做扎实的情况下强行实现一个正确性敏感的重写。

## 2026-08-12：docs/18 架构优化方案 Track B J1（switchFor region-aware 2-way collapse）

对应计划：`docs/18-architecture-optimization-plan.md` §5.2、W1/M1 里程碑。J1 给
`Structurizer` 加了 `StructureOptions{minRegionSites=8, deferRegionCollapse=false}`
（`include/xdec/emit/structure.h`），`switchFor`（`src/emit/structure.cpp`）在把一个
2-way table-mode dispatch collapse 成 `if`/`else` 之前，先用新的
`isMemberOfLargeDispatchRegion` 查这个 dispatch site 是否属于
`analysis::DispatchRegion` 里 ≥ `minRegionSites` 个 site 的一个 region——是则保留
table-mode `switch`，不再走 if/else 折叠。新增
`tests/emit/test_structure_dispatch_region.cpp`（4 用例：region 小于/等于/自定义
门槛的对照、`deferRegionCollapse` 诊断开关独立验证）。

### `sample_libscplugin` 前后对比

| 指标 | J1 前 | J1 后 | 说明 |
|------|------:|------:|------|
| switch | 0 | **234** | 精确匹配方案 §5.2 的预期效果（"switch 0 → ~234"）——该函数唯一的 234-site region 现在整体保留 table-mode |
| goto | 407 | 407 | 不变，符合预期：J1 不处理 goto，那是 J3/J4 的范围 |
| while(true) | 39 | **2** | 副作用，非缺陷：旧算法把这 234 个 site 折成 if 链后，`wrapAsLoop` 才能把其中的链识别成循环；J1 让它们继续以 switch 呈现，不再触发那条 if 链驱动的循环识别。这正是方案 §11.2 自己把 while(true) 列为"非门禁、方向性"指标（39 期望逐步降到 <15）的原因 —— 不是这次改动应该规避的回归 |
| 行数 | 8253 | 6368 | table-mode switch 比对应的 if 链更紧凑 |

### manifest 调整（用户已确认）

`samples/manifest.json` 的 `sample_libscplugin` 原有 `min_loops_while_true: 30`
是照着 J1 之前"if 链折叠产生大量 while(true)"这个副作用校准的门槛，J1 落地后
必然被打破——但这不代表退步，方案 §11.2 本就把这项标成方向性、非门禁指标。
经用户确认（选项：移除/下调该门槛并 `-UpdateBaseline`），改为：移除
`min_loops_while_true`，新增 `min_switches: 220`（在 234 之下留薄余量）直接
守住 J1 的真实收益，`max_lines` 从 10500 收紧到 6600、`max_gotos` 从 460 收紧到
420（均在实测值上留薄余量）。`comment` 字段同步改写，记录 J1 的因果链。
`samples/baseline.json` 已用 `-UpdateBaseline` 刷新。

### 回归

`xdec_tests.exe`：612 test cases、133,664 assertions 全过（含新增
`test_structure_dispatch_region.cpp`）。`eval/run.ps1`：baseline 98/98、
typed 38/38，vs baseline 均无 fixed/regressed。`samples/run.ps1`：5/5，
vs（刷新后的）baseline 无 fixed/regressed。

## 2026-08-12：docs/architecture-optimization-eval-prompt.md J2d（region handler clone）

对应计划：`docs/architecture-optimization-eval-prompt.md`（M1 W2）与
`docs/18-architecture-optimization-plan.md` §5.3 的短期 goto 缩减方向。J1 把大 region
的 2-way site 从 if/else 折叠改回 table-mode `switch`，但 `switchFor` 的 N-way/table-mode
case 循环此前只试 `claimDispatcherCaseBody`/`claimCaseBody`，从未试
`claimOrCloneSharedCaseBody`（该 fallback 此前只接在 if/else 折叠路径上）——一个 handler
若被同一张表的 ≥2 个 site 共同指向（`claimOrCloneSharedCaseBody` 本就认的形状），在
table-mode 分支里永远直接落到 `addGotoTarget`。J2d 把同一个 fallback 也接进这条循环
（`src/emit/structure.cpp` `switchFor`），让"被 J1 送进 table-mode 的 site"也能享受到
"handler 克隆内联"。

### 排查过程中发现并修复的两个通用问题

1. **`sharedCaseBodyCache_` 从不随 `rollback` 撤销**：`claimOrCloneSharedCaseBody`
   可能嵌套在另一个失败后要整体回滚的推测性尝试内部（`tryDiamond`/`tryOneSided`/
   `claimCaseBody` 等任何会递归 `emitRegion` 的路径都可能触发）——那次尝试失败回滚后，
   对应 block 不再是 `emitted_`，但缓存里那份克隆体仍会被后续真正的调用者取走，等于
   同一段代码既作为克隆体打印，又在它自己的自然位置再打印一次。修法：新增
   `sharedCaseBodyInsertions_`，记录每条缓存插入发生时的 `trail_.size()`；`rollback`
   现在按同一个 `trailSnapshot` 一并清掉本轮新增的缓存项（`structurizer.h`/
   `structure.cpp`）。
2. **`kMaxSharedBodySize`（trail 增量计数）不是可靠的"这段代码会打多长"代理**：
   一个 handler 未被声明的 case 不会把自己的子树计入 `trail_`（那是留给顶层
   walk 之后单独处理的），但打印时仍会把整棵 `Switch` 子树完整展开——所以一个
   trail 增量很小的克隆体，仍可能在打印时嵌进另一整个 switch。scatter-dispatcher
   的 handler 经常"做几步就再走同一张表分发一次"，恰好踩中这个空子，第一次上线时
   把 `sample_libscplugin` 从 407 goto / 6368 行推高到 511 goto / 7921 行——比 J1 基线
   更差。修法两层：（a）`containsSwitch`——克隆前对候选 body 的语句树做一次遍历，
   查是否嵌了 `Switch`，嵌了就按尺寸超限一样拒绝（`structure.cpp`，两处调用方都受益）；
   （b）`reachesFurtherDispatch`——在真正调用 `emitRegion` 之前，先用一次不产生任何
   `mark`/`trail_`/`gotoTargets_` 副作用的纯只读 BFS（只看 `successors` 和
   terminator），提前发现"这条链在预算内还会再分发一次"就直接拒绝，省下(a)要
   完整走一遍才能发现、且失败后无法退还的那部分 `budget_` 消耗。

### `sample_libscplugin` 前后对比（诚实结果）

| 指标 | J1 基线 | J2d（未加两层防护，仅供记录） | J2d（最终） |
|------|------:|------:|------:|
| goto | 407 | 511 → 加 containsSwitch 后 572 | **407**（不变） |
| 行数 | 6368 | 7921 → 加 containsSwitch 后 6678 | **6368**（不变） |
| switch | 234 | 338（新增的全是被克隆体带出来的嵌套 switch） | 234（不变） |

最终结果对这一个样本没有可观测收益：`libscplugin` 的 234-site region 里，几乎每个
被多个 site 共享的 handler 自己也在预算内再次分发，`reachesFurtherDispatch`/
`containsSwitch` 因此几乎全数拒绝克隆，行为回落到与 J1 基线逐字节一致。这是诚实的
结果而不是缺陷：J2d 针对的形状（"小、不再分发的共享 handler"）在合成 fixture 里
确认可用（见下），但 `libscplugin` 这个真实样本恰好几乎不含这种形状——`docs/
architecture-optimization-eval-prompt.md` 的 goto 分析已经指出 65% 的 case goto
来自 case 本身直接跳转，真正的结构性收敛仍需 J2（`collapseRegionDispatchTree`）。

### 新增测试

`tests/emit/test_structure_dispatch_region.cpp` 新增 4 个用例：
- 2-site（`deferRegionCollapse`）与 8-site（自然达到 `minRegionSites`、共享点相隔
  两个 site）两个正例——共享 handler 被克隆进每个 case，不再是单一 goto 目标。
- 反例：共享 handler 的其中一个前驱不是 resolved 2-way table dispatch（一个普通
  无条件跳转）——`claimOrCloneSharedCaseBody` 正确拒绝，两个 switch 的对应 case
  仍是未声明（打印为 goto）的插槛。
- 通过 A/B（feature-flag 环境变量 + 手工 diff 生成的 C）验证了 containsSwitch/
  reachesFurtherDispatch 两层防护缺一都会在 `libscplugin` 上产生可观测回归，
  确认两层都是必要的，不是防御性冗余。

### 回归

`xdec_tests.exe`：619 test cases、133,705 assertions 全过。`eval/run.ps1`：baseline
98/98，vs baseline 无 fixed/regressed。`samples/run.ps1`：5/5，vs baseline 无
fixed/regressed（`sample_libscplugin` 逐字节回到 J1 基线，见上表）。`samples/
manifest.json`/`baseline.json` 均未改动——J2d 对这个样本没有把门槛推得更紧的理由。

## 2026-08-12：libscplugin 核心提升方案 Phase 0–5 收尾

对应计划：`libscplugin 反核心提升`（见该 plan 文件自身的诊断与分阶段设计，
不重复摘要）。核心约束照旧：**不开发插件系统**，所有改动 IL/形状驱动，
`libscplugin`（`0x1164f8`）只作 L2 观测，不写任何该样本专属的地址/常量分支。

### 交付内容

- **Phase 1**：`analysis::DispatchRegion`
  （`include/xdec/analysis/dispatch_region.h`、`src/analysis/dispatch_region.cpp`）
  —— 按物理跳表身份（base/stride/entryBits/clamp）聚类分散的两路 dispatch
  site，并在 pooled targets 上做 `matchDispatcherShape` 式的多数票，识别出
  单个 site 自己凑不出三个目标、但整个 region 能确认的共享尾。接入
  `AnalysisCache`（新增 `"dispatch"` tag）与 `Structurizer::tryDispatcherLoop`
  的 fallback。完整设计与已知局限见 `docs/17-dispatch-region.md`（新增）。
- **Phase 3**：`emit/c_stmt.cpp` 的 `Store` 打印顺序调整——共享的 `Select`
  值现在优先走 `ExprPrinter::materializeAs`（赋给 local 自己的名字），只有
  不共享时才回退到 `printSelectAssign` 的 if/else 展开；`deadStateDiscriminantStore`
  的 Block+Switch 折叠一并验证仍在生效。细节与验收用例见
  `docs/09-expression-reuse.md` 的 H2 扩展说明。
- **Phase 4**：`passes/fold.cpp` 新增 `FlagPhiDistributor`（跨 flags-phi 分发
  `FlagCond` 测试）；`passes/fold_resolved_branch.cpp` 新增 `removeOrphanedLoads`
  （已解析分支丢弃目标表达式后的孤儿 load DCE）；`vtable_call`
  （`analysis/vtable_call.h`）接线到 `printCall`，确认的 vtable slot 现在打印
  `/* vtable slot 0x... */` 注释。
- **`xdec_dispatch_index_*` 内联 helper 已按此前决定移除**：clamp 现在总是打印
  成普通三元表达式，`quantify_c.py` 的 `clamp-ternary` 正则据此更新以匹配带
  `(intNN_t)`/`(uintNN_t)` 强转的形式。

### `sample_libscplugin` 前后对比（`samples/build/out/sample_libscplugin.c`）

| 指标 | 方案落地前 | 本次（2026-08-12） | 说明 |
|------|-----------:|-------------------:|------|
| 行数 | 8295 | 8253 | H2 折叠 + region-confirm 打通带来的净减少 |
| `state =` | 1192 | 1188 | Phase 3 的共享 `Select` 折叠（`state = (cond) ? A : B;` 复用同一次计算） |
| `switch` | 0 | 0 | 见下文：该函数唯一 region 的 `sharedTail=false`，Phase 2 的 region-confirm 无票可用 |
| `while (true)` | 39 | 39 | 同上 |
| `goto` | 407 | 407 | 同上 |
| `L_0x` 标签 | 295 | 295 | 同上 |
| `clamp-ternary`（新指标） | — | 4 | 确认 4 处 clamp 都已是普通三元，无 `xdec_dispatch_index_*` 残留 |
| `flagcond-stub` | 29 | 29 | 该函数里现存的 stub 并非跨 phi 链路（`FlagPhiDistributor` 覆盖的形状），本次未变 |
| `dispatch-load-sites`（新指标） | — | 37 | Phase 0 新增度量，仅供后续跟踪，无历史基线 |
| `duplicate-routing-if`（新指标） | — | 10 | 同上 |

`xdec decompile ... --emit-report` 的诊断行确认了 Phase 1 的分析结果：

```
dispatch-regions: 1 region(s), 234 site(s) total
  region[0]: table=0x1e70a0 stride=8 entryBits=64 clamp=0x2cc/0x213 sites=234 sharedTail=false
```

**为什么 `switch`/`while(true)`/`goto` 三项没有变化**：`libscplugin` 这一个函数
里，234 个 dispatch site 的目标确实各自散落到不同的下一状态，没有多数
site 收敛到同一个 merge block——`sharedTail=false` 是分析给出的真实结论，
不是没找全。`Structurizer::matchRegionConfirmedShape` 在 `region.sharedTail`
为空时没有证据可用，因此对这个函数完全不生效，`switchFor` 的 2-way collapse
策略（Phase 2c，本方案未修正）继续把两路 dispatch 打印成 `if`/`else`。
这与方案诊断阶段的预期一致（该函数从未被认为会自己长出 `sharedTail`），
`docs/17-dispatch-region.md` 的"What this does and does not change for
libscplugin"一节记录了同样的结论，以及为什么 region-confirm 路径本身在
`tryDispatcherLoop` 里目前还没有一个能端到端触发它的合成 fixture
（`emitRegion` 遇到内嵌的 `IndirectBranch` 就不会继续走向调用者要求的
`stop` 块，这是一个独立于本方案的既有限制，不是这次改动引入的）。

### 回归

- `xdec_tests`：612 test cases、133664 assertions 全过（含本次新增的
  `tests/analysis/test_dispatch_region.cpp`、`tests/analysis/test_analysis_cache.cpp`
  的 `dispatchRegions()` 用例、`tests/emit/test_c_vtable_call.cpp`、
  `tests/emit/test_c_printer.cpp` 的共享 `Select` 折叠用例）。
- `eval/run.ps1`：98/98，vs baseline 无 fixed / 无 regressed。
- `samples/run.ps1`：5/5（`sample_jni_onload`、`sample_mega_dispatcher`、
  `sample_core_mba`、`sample_afRDLog`、`sample_libscplugin`），vs baseline
  无 fixed / 无 regressed；`manifest.json` 的阈值本次未放宽
  （`sample_libscplugin` 仍在 `max_gotos=460`、`min_loops_while_true=30`、
  `max_lines=10500` 内，用不着改）。

## 2026-08-12：反编译质量继续优化方案 Phase 1-4（J5/J3/J2e/J2/J2f + Track A 并行项）

对应计划：`docs/architecture-optimization-eval-prompt.md`，J2d 完成后审查得出
「407 个 goto 的结构性收敛必须靠 J2」的结论，本轮把方案的四个阶段全部落地。
起点（J2d 完成时，`sample_libscplugin` @ `0x1164f8`）：goto 407、switch 234、
行数 6368、`while(true)` 2、`dispatch-load-sites` 37、`duplicate-routing-if` 10。

### Phase 1：J5 dead dispatch load DCE + J3 路由三写消除（Track B）与 PipelineFixture/AnalysisCache（Track A）

**J5**：扩展 `c_stmt.cpp` 的 `deadJumpTableLoad` 判定并系统接入 `collectDeadOps`
遍历路径——`state = f(cond); t = load(table[clamp(state)]);` 里 `t` 若无读者、
routing 分支本身不读它、也没有 call/memory 副作用，则整条 `load` 判死。
`sample_libscplugin` 的 `dispatch-load-sites` 从 37 降到 30。

**J3**：新增 `deadRoutingStateStore`（`c_stmt.cpp`），识别 `state=(cond)?A:B`
紧邻同 `cond` 的 `if`/`CondBranch` 这一路由三写形状。核心难点是不能只看
「无读者」的一般死存储判据（`findDeadStackStores` 已经处理了那种更简单的
形状，且过早地吃掉了合成测试的目标结构）——这里需要一个有界的正向可达性
检查（`localMayBeReadBeforeRewrite`）：只在同 block 内、以及预算内可达的
后继 block 里，确认该 local 在被下一次写覆盖之前不会被路由路径读到，才判定
这次 `Store` 真正冗余。`sample_jni_onload` 的 `duplicate-routing-if`
10 → **0**，且 `state=` 相应减少。回归：`tests/emit/test_c_dead_routing_store.cpp`。

**Track A（H+C）**：`tests/fixture/pipeline_fixture.h` 新增
`structureFunction()`，把「建 `Dominators`/`PostDominators`/`NaturalLoop` 再调
`structureFunction`」这套每个 structure 测试都手写的样板收进一个 helper，
`test_structure.cpp`/`test_structure_dispatch_region.cpp` 已迁移。新增
`AnalysisCacheObserver`（`xdec_decompile`，实现 `pass::Observer`）：某个 pass
报告有变更时，读它的 `PassInfo::invalidates` 标签并调
`AnalysisCache::invalidate()`，堵上「pass 跑完但 cache 没失效」的既有缺口。
回归：`tests/decompile/test_analysis_cache_observer.cpp`。

### Phase 2：J2e region join block epilogue + J2 设计冻结

**J2e**：`sharedTail` 是整 region 的 ≥80% 多数票，抓不住「19 个各自只有
2-3 个前驱的独立 merge hub」这种形状。新增 `analysis::findDispatchJoins`
（`dispatch_region.h`/`.cpp`）：找一个块 `hub`，它是 ≥2 个「私有 handler
尾块」（恰好一个前驱来自 region 内某 dispatch site、恰好一个后继指向
`hub`）的共同目标，且 `hub` 自身的全部前驱都能被这些尾块覆盖（排除 region
外还有第三方前驱混进来的假阳性）。`Structurizer::switchFor` 在
`matchDispatcherShape` declines 之后，用 `joinHubByTail()` 缓存试第一个
尚未被占用的 `DispatchJoin`，把它的 `hub` 当作这个 switch 的 epilogue 打印
一次（`stmt->frame` 仅在 `DispatcherShape` 的全套证明成立时才赋值，避免
J2e 派生的合并被误套上并不适用的活跃寄存器帧）。回归：
`tests/emit/test_structure_join_epilogue.cpp`。

**J2 设计冻结**：`StructureOptions::regionStructuring`（默认 `false`）、
`kRegionPatterns` 元数据数组、CLI `--region-structuring`（`cmd_pipeline.cpp`）、
7-site 合成 fixture（`test_structure_region_switch.cpp`）——为 Phase 3 的
实现先把接口和测试骨架定下来，本阶段不改变默认输出。

### Phase 3：J2 `collapseRegionDispatchTree`

新建 `structure_dispatch_region.cpp`：`Structurizer::collapseRegionDispatchTree`
在 `switchFor` 建好一个 outer `Switch` 之后，检查它每个 case body 是否恰好是
`[Block, Switch]`，且内外两个 `Switch` 都是 `tableMode`、共享同一个
`il::ExprId` discriminant（`outer.cond == inner.cond`）、`caseValues` 状态
一致（同为空或同非空）、内层没有自己的 `epilogue`——只在这四条都成立时，把
内层的 case/predicate/body 拼进外层对应位置，物理上合并成一个 mega-switch。
故意不做的是「跨 site 猜测同一个 discriminant」：只合并已经证明共享同一个
`ExprId` 的相邻两层，不推断两个本来互不相关的 site 的 index 其实是同一个
状态变量——那是本文档 `17-dispatch-region.md` "Non-goals" 一节明确排除的
更大、更冒险的主张。

`sample_libscplugin` 上开 `--region-structuring` 观测：goto/switch/行数
**零变化**——该函数的 234 个 site 各自读取独立的 discriminant，从未出现
两层嵌套共享同一个 `ExprId` 的形状，这是分析给出的诚实结果（无嵌套树可
合并），不是实现的缺口。合成 fixture（7-site 同表、可恢复 caseValues 的
线性链）验证了算法本身确实能把嵌套树压成一个 ≥7-case switch。回归：
`tests/emit/test_structure_region_switch.cpp`。

### Phase 4：J2f labeled natural loop

`sample_libscplugin` 149 条回边里的大多数落在「header 是已解析
`IndirectBranch`」的 handler 簇内——这个形状既不匹配 `tryLoop`/
`tryDispatcherLoop`（两者都专门找 `CondBranch` 头），`wrapAsLoop` 也只在
它刚建好的那一个 switch 内部找回边，看不到从别的、隔了好几个 block 才到达
的独立顶层分组回来的边。新增 `Structurizer::collapseLabeledNaturalLoops`
（`structure.cpp`，`run()` 的两轮 RPO 扫描之后跑一次）：对每个
`NaturalLoop`，找到 header 自己代码所在的那个顶层分组——**不要求 header 是
该分组自己的起始块**，因为一次普通的 fallthrough 链（`emitRegion` 的
`Branch` 分支本就不断往同一个 `Sequence` 继续追加）经常把 header 的代码接在
一段与循环无关的前置代码后面（典型例子：函数真正的入口块直接落进循环
header）。做法是在该分组自己的 `items` 里定位 header 的 `Block` 语句所在
下标，只把从那个下标开始的尾段（而不是整个分组）当作候选循环体，之前的
内容原样留在分组里、留在 `while` 外面。随后要求循环的每个其余成员块都能在
某个尚未被消费的顶层分组里找到、且该分组自身没有已经嵌套的循环，才把这些
分组按 RPO 顺序拼接进循环体，调用（原本只在 `structure_dispatch.cpp` 内部
用的）`Structurizer::continueAtBackEdges`（现已提升为共享的 `static`
方法，并扩展到能把裸 `Switch` 的 case/default 目标一并改写）把 `goto
header` 改成 `continue`，最后包成 `while (true)`。

`sample_libscplugin`：`while(true)` **2 → 49**，`goto` **407 → 388**
（比方案自己估计的「60-100」小得多，原因是这批回边与 J2e 已经claim的 hub
高度重叠——两项收益本就不是简单相加，方案 §Phase2 的注释已经提到这点）。
行数从 6368 上升到 6702（`max_lines: 6600` 因此被突破，见下）。回归：
`tests/emit/test_structure_labeled_loop.cpp`（两个用例：`head`/`a`/`b`
分裂成三个独立顶层分组、`a`/`b` 都落进同一个未被 claim 的 `tail` 的正例；
某个循环成员自己已经先被 `wrapAsLoop` 包成自环的负例，确认外层循环不会
去拆一个已经结构化好的内层循环）。

### `sample_libscplugin` 汇总对比（J2d 完成 → 本轮四阶段落地后）

| 指标 | J2d 完成时 | 本轮后 | 说明 |
|------|-----:|-----:|------|
| goto | 407 | **388** | 全部来自 J2f；J2/J2e 本身对这个样本零直接贡献（见上） |
| switch | 234 | 234 | J2（`collapseRegionDispatchTree`）零命中，J2e 只挂 epilogue 不改 switch 数 |
| `while(true)` | 2 | **49** | J2f |
| 行数 | 6368 | 6702 | 主要是 47 个新 `while(true)` 各自的结构开销 |
| `dispatch-load-sites` | 37 | **30** | J5 |
| `duplicate-routing-if` | 10 | **0** | J3（在 `sample_jni_onload` 上验证；`sample_libscplugin` 本身这项此前已是低个位数） |

### manifest 阈值（待用户确认，暂未调整）

`samples/manifest.json` 的 `sample_libscplugin.max_lines: 6600` 被本轮的
6702 行突破，`samples/run.ps1` 现报 4/5。这是 J2f 新增 47 个 `while(true)`
的直接、可解释的结构开销，不是回归——但按方案 §8 与用户既有约定，
manifest 阈值收紧/放宽只在用户确认后用 `-UpdateBaseline` 落地，本轮未改动
`manifest.json`/`baseline.json`，把决定权留给用户。

### 回归

`xdec_tests.exe`：**636** test cases、133778 assertions 全过（含本轮新增的
`test_analysis_cache_observer.cpp`、`test_c_dead_routing_store.cpp`、
`test_structure_join_epilogue.cpp`、`test_structure_region_switch.cpp`、
`test_structure_labeled_loop.cpp`）。`eval/run.ps1`：baseline 98/98、
typed 38/38，vs baseline 均无 fixed/regressed。`samples/run.ps1`：4/5——
仅 `sample_libscplugin` 因上述 `max_lines` 阈值未随 J2f 更新而报"NO"，
其余 4 个样本与其余所有指标均无回归。

## 2026-08-13：J2g unique goto expansion

对应用户直接指出的具体案例：`sample_libscplugin.c` 里 `case 0x1ab: /* handler
@0x11f500 */ goto L_0x11f500;` 紧接着 `L_0x11f500:` 只被这一处引用，却仍要单独
占一个顶层分组和一行 label——J2f 落地后遗留的、更细粒度的一类 goto。

**根因**：`Structurizer::claimCaseBody` 在这个形状上失败（多数情况是
`regionClosed` 因为 handler 自己吸收的后续 block 撞上了别的 case 共享的
merge，而不是"handler 本身有第二个前驱"）之后，`switchFor` 只留下
`caseBodies[i] == nullptr` + `cases[i] == H` 这一对裸值——`c_stmt.cpp` 打印
`printSwitch` 遇到空 `caseBodies[i]` 时直接吐 `goto label(cases[i]);`，从不经过
`Stmt::Goto` 节点。与此同时 `run()` 里两轮无检查的 RPO 补扫会把 `H` 单独收成一个
从 `Block(H)` 开始、可能吸收好几个 block 的顶层分组——如果这条链路只被这一个
`case` 引用，它的 label 和分组就纯属多余。

**实现**（`structure.cpp`/`structurizer.h`，`collapseLabeledNaturalLoops`
之后、`stable_sort` 之前跑）：

1. `countReferences`（`collectReferences` 的计数版）：不满足于"引用过"，要分清
   "恰好一次"和"不止一次"——一个自然循环回边落到 `continue` 而非 `Goto`/裸
   case 槽，是这版计数天然看不到的，所以还要叠加下一条硬约束。
2. `Structurizer::expandUniqueCaseGotos`：Phase 1 建 orphan 索引（一个顶层分组
   够"干净"——`items[0]` 恰是 `Block(自己的 head)`，且不是入口块——才算候选）；
   Phase 2 跑 `countReferences` 拿到整棵树（含每个分组自身内容）的引用计数；
   Phase 3 用新的 `Structurizer::expandGotoTargets` 递归遍历每个分组，对每个
   `Switch` 的每个裸 case/`default` 槽尝试折叠：目标必须 `refCount==1`、IL 级
   `predecessors.size()==1`（防止一个树扫描看不见的第二引用，如上面的回边
   `continue`）、orphan body `alwaysLeaves`、且不含嵌套 `Switch`
   （`containsSwitch`，避免把整棵子 dispatch 树错塞进一个 case）——四条全满足
   才 `cloneStmt` 整个 orphan 分组填进 `caseBodies[i]`，并把该分组标记
   consumed、跑完后统一从 `groups` 里删除。
3. 递归天然处理链式 orphan（一个 orphan 的唯一引用又指向另一个只被它引用的
   orphan）：`expandGotoTargets` 在遇到非空 `caseBodies[i]` 时照样递归进去找
   更深的裸槽——真实样本里外层 `Switch` 早已通过 `claimCaseBody` 内联，裸槽都在
   被内联的子树深处，不下钻就找不到。

**为什么不是简单地"goto 恰好一次就展开"**：`refCount==1` 只是这棵已经打印出的
树 **看得见** 的引用数；`continueAtBackEdges`（J2f）已经把回边改写成
`Continue`，不会被 `countReferences` 数进去，所以一个自然循环 header 若恰好
只被树内一处 `Goto`/裸槽引用，会在树扫描这一关误判为"唯一引用"。IL 级
`predecessors.size()==1` 是独立于树形状的真相来源，堵住了这个洞——回归用
`tests/emit/test_structure_unique_goto_expand.cpp` 的第二个用例专门验证。

**验收**（`sample_libscplugin` @ `0x1164f8`，J2f 完成时 → J2g 后）：

| 指标 | J2f 后 | J2g 后 | 说明 |
|------|-----:|-----:|------|
| goto | 388 | **326** | −62；用户举的 `L_0x11f500` 已确认消失，内容直接内联进 `case 0x1ab` |
| labels | 317 | **231** | −86 |
| 行数 | 6702 | **6572** | 折叠掉的重复 label/goto 行数超过内联展开的净增，`max_lines: 6600` 的既有阈值突破**不修自愈**，`samples/run.ps1` 由 4/5 回到 **5/5** |
| switch | 234 | 234 | 不变——J2g 只填 `caseBodies`，不改 `cases`/`caseValues` |
| while(true) | 49 | 49 | 不变——只在 `collapseLabeledNaturalLoops` 之后跑，不触碰循环结构 |

比 J2g 方案自己估计的 120–140 goto 更保守：真实样本里不少裸 case 槽的目标
恰好还是嵌套 dispatch 树的一部分（`containsSwitch` 正确拒绝），或者目标块
被两条独立路径共享（IL 双前驱正确拒绝）——都是方案 §"风险与缓解"里预先列出、
故意不碰的形状，不是实现遗漏。

**回归**：新建 `tests/emit/test_structure_unique_goto_expand.cpp`，四个用例——
一个正例（两条 case 因共享 `regionClosed` 冲突的下游 block 都失败 claim、
各自成为只被引用一次的顶层分组，两个都被折叠回自己的 case）+ 三个负例
（IL 双前驱不折叠；handler 自身没有 terminator、`alwaysLeaves` 判否不折叠；
handler 自身是嵌套 dispatch、`containsSwitch` 判否不折叠，即使它经由自己的
`epilogue` 确实"leaves"）。`xdec_tests.exe`：**640** test cases、133804
assertions 全过。`eval/run.ps1`：baseline 98/98、typed 38/38，vs baseline
均无 fixed/regressed。`samples/run.ps1`：**5/5**（`sample_libscplugin` 的
`max_lines` 阈值突破随行数下降自愈，未改动 `manifest.json`）。

## 2026-08-13：J2g × J2f 执行顺序 — 循环体内 remnant 补折叠

对应用户直接指出的具体案例：`sample_libscplugin.c` 5705-5711 行的
`case 0x97: goto L_0x1224c8;` / `case 0xd6: goto L_0x11efc0;` 在 J2g 落地后
仍然没有被折叠，尽管形状看起来和已经修好的 `L_0x11f500` 一样。

**根因**：先在 `expandGotoTargets` 的 `tryFold` 里加了针对这两个目标 VA 的
`fprintf(stderr, ...)` 诊断（每条拒绝路径各打一行），重新跑
`samples/run.ps1` 抓到的输出是两个目标都停在同一行——`"no clean orphan
group (in orphanIndex=0)"`，也就是说 Phase 1 建 orphan 索引时压根没找到
这两个目标对应的顶层分组，跟 `refCount`/IL 前驱数/`alwaysLeaves` 都没关系。

顺着这条线看 `Structurizer::run()` 的调用顺序：`collapseLabeledNaturalLoops`
（J2f）先跑，`expandUniqueCaseGotos`（J2g）在其后。J2f 对每个自然循环
header 找"贡献者"（`loopPtr->blocks` 里除 header 外、`ownerOf` 映射到某个
其它顶层分组的成员，按 `NaturalLoop::blocks` 的构造——从每条回边的 latch
反向沿 predecessors 走到 header 为止——这个集合覆盖的不只是闭合回边的那
一个 block，是"只要还在循环里、走不出去"的每一个 block），把这些贡献者
分组的 `items` **原样拼接**进循环体的 `Sequence`。拼接之后，`0x1224c8`/
`0x11efc0`（连同 `0x11ef70`/`0x11ef20`）各自原来的顶层 `groups[i]` 条目
不复存在，只剩下拼进同一个 `Sequence` 里的匿名 `Block` 分段——它们仍然
在树里（`collectReferences` 找得到，标签仍然打印），但 J2g 事后重建的
`orphanIndex`（要求 `region->items.front()->block == head`）已经没有
入口能索引到它们了，folding 从这一步起永久错过，跟 `refCount`/`alwaysLeaves`
/`containsSwitch` 这些后续检查完全无关——诊断行印的正是 Phase 1 就拒绝了。

**方案**（`structure.cpp`）：`Structurizer::run()` 里 `expandUniqueCaseGotos`
改成跑两遍——

1. 一遍在 `collapseLabeledNaturalLoops` **之前**：此时循环体的贡献者还是
   各自独立的顶层分组，`orphanIndex` 能正常找到它们，按 `switch` case 的
   唯一引用先把能折的都折进 case body（哪怕它们本身包含一条回到循环
   header 的裸 `goto`——`alwaysLeaves` 只要求"离开"，不要求离开到哪）。
2. `collapseLabeledNaturalLoops` 照常跑：折叠只挪动内容
   （`cloneStmt`+`consumed` 剔除顶层分组），不影响 `ownerOf`/`ranks` 的
   计算方式，只是这一轮里很多本该被它拼接的贡献者已经不在顶层
   `groups` 里了（`contributing` 集合可能因此完全清空）——但它仍然
   无条件把 header 自己的 `switch` 包进 `while (true)` 并对其递归跑
   `continueAtBackEdges`（递归本就会下钻进已经内联的 `caseBodies`），
   于是被第一遍 J2g 折叠进某个 case body 深处的裸 `goto header;` 照样
   被就地改写成 `continue;`——循环结构不因为提前折叠而丢失。
3. 一遍在其后：保留原有位置，专门补 J2f 完全没碰到（不属于任何自然
   循环）的顶层 remnant，即 §6.8 最初解决的那类。

两遍共用同一份 `expandUniqueCaseGotos`/`expandGotoTargets` 实现，没有新增
状态、字段或 CLI 开关。

**为什么两遍是安全的、不会互相踩**：第一遍折叠的判定完全依赖 IL 级
`predecessors.size()==1` 和树扫描 `refCount==1`，两者都是"这一刻的 CFG/
树形状"的真相，不依赖 J2f 是否已经跑过；folding 本身只是把内容从一个
`StmtPtr` 挪到另一个（`cloneStmt` 深拷贝、原分组整体从 `groups` 剔除），
不会遗留悬空引用——`xdec_tests.exe` 全量跑、`eval/run.ps1`、
`samples/run.ps1` 三层回归均确认零回归；另外用同一颗二进制反复跑
`sample_libscplugin`（102 round、664 extra entries 的真实大样本）观察到
一个与本改动无关的、极低概率（约 2/24 次）driver SSA 校验阶段
（`il::verify`，在 Structurizer 被调用之前）随机 `SIGSEGV`——改动前的
基线同等复测 16 次里 0 次崩溃，样本量不足以排除这是本就存在的稀有
flaky 项，但栈回溯确认崩溃帧在本次改动的调用路径之外，记录在此供后续
单独复现。

**验收**（`sample_libscplugin` @ `0x1164f8`，J2g 单遍 → J2g×J2f 双遍）：

| 指标 | J2g 单遍 | J2g×J2f 双遍 | 说明 |
|------|-----:|-----:|------|
| goto | 326 | **289** | −37；用户举的 `L_0x1224c8`/`L_0x11efc0` 已确认消失，`L_0x11ef70`/`L_0x11ef20` 同一循环里的另外两个同形状 remnant 也一并折叠 |
| 行数 | 6572 | **6498** | |
| switch | 234 | 234 | 不变 |
| while(true) | 49 | 49 | 不变——所有已识别的自然循环结构均完整保留 |

**回归**：`test_structure_unique_goto_expand.cpp` 新增一例——两条 case 各自
因共享 `merge` 的 `regionClosed` 冲突失败 claim（构造手法同 §"J2g unique
goto expansion" 的正例），但 `merge` 自己再跳回 dispatcher 形成回边，使
`loopByHeader_` 出现以 dispatcher 为 header、成员覆盖两个 handler 的自然
循环；断言两个 case 都被折叠、循环仍以 `while (true)` 形式存在且包着
同一个 `switch`、树里已没有任何裸 `goto dispatcher`（转成了 `continue`）。
`xdec_tests.exe`：**641** test cases、133822 assertions 全过。`eval/run.ps1`：
baseline 98/98，vs baseline 无 fixed/regressed。`samples/run.ps1`：**5/5**。

## 2026-08-13：scatter dispatch 目标结构（`StructureOptions::collapseScatterRegionSites`）

对应计划：`docs/19-scatter-dispatch-target-shape.md`（该文档不可编辑，本节
记录实测）。此前所有 J1-J5 的改动都把 `libscplugin` 这一 234-site scatter
region 当作"暂不处理，先保持可见"（J1 的 `minRegionSites` defer），本轮的
问题是这个 defer 到底还要不要继续——从汇编重新核实了这个 region
`sharedTail=false`（无 region 级共享 merge），J1 的 defer 本来就是为
`sharedTail` 存在的"真实 flattened loop"形状保留的，对这种 scatter 形状
它从一开始就没有对应的收益，只是"让 site 数量在打印里可见"这一个诊断目的。
`analysis::buildDispatchNestGraph`（新增）把这个可见性移到
`--emit-report` 的 `dispatch-nest:` 诊断行里之后，defer 本身就可以对
`sharedTail=false` 的 region 关掉了。

### 实现

- `analysis::DispatchNestGraph`/`buildDispatchNestGraph`
  （`dispatch_region.h`/`.cpp`）：纯分析，从每个 site 的目标出发做有界
  BFS，找它是否直接落入 region 内另一个 site——`roots`/`maxDepth`/
  `nestedSiteCount`，与此前 `tools/analyze_dispatch_nesting.py` 的规则
  一致（`libscplugin` 上跑出 `roots=93 depth=28 nested=141`，与该脚本
  当时的结论逐字吻合）。
- `StructureOptions::collapseScatterRegionSites`（默认 `false`）：
  `switchFor` 的 `deferCollapse` 从"region 够大就 defer"改成"region 够大
  **且**投票出了 `sharedTail` 才 defer"——`sharedTail` 不存在时,2-way
  site 照单站规则收敛成 `if`/`else`,`claimCaseBody` 的既有递归自动把
  链式 site 收敛成一整棵嵌套 `if`/`else if`,不需要新的结构化 pass。
  CLI：`--collapse-scatter-sites`。
- 未新增代码但复用生效：`claimOrCloneSharedCaseBody`（J2d）此前只在
  table-mode switch 分支里被验证过对 `libscplugin` 无可观测收益（见
  §6.2 的诚实结论）——现在同一份代码在 `if`/`else` 收敛分支里对新收敛出
  来的 site 生效，`tests/emit/test_structure_dispatch_region.cpp` 新增
  一例直接断言两条链式 site 共享的 handler 只印一次、克隆进两个 `if`
  分支，没有 goto。

### 验收（`sample_libscplugin` @ `0x1164f8`，`--allow-unresolved --rounds 128`）

```
dispatch-regions: 1 region(s), 234 site(s) total
  region[0]: table=0x1e70a0 stride=8 entryBits=64 clamp=0x2cc/0x213 sites=234 sharedTail=false
    nest: roots=93 depth=28 nested=141
```

| 指标 | 关闭（现状基线） | 开启 `--collapse-scatter-sites` | 说明 |
|------|-----:|-----:|------|
| switch | 234 | **0** | 每个 2-way site 都收敛，§2 的目标 |
| if（含 else if） | 140 | **533** | 吸收了被收敛的 site，形成嵌套链 |
| goto | 289 | **332** | **上升**——见下方"诚实结论" |
| while(true) | 49 | 49 | 不变，不是本次目标 |
| unresolved_branches | 0 | 0 | 不变 |
| 行数 | 6499 | 6865 | |

**诚实结论（goto 为什么涨了）**：table-mode switch 能把一个多前驱 join hub
（`analysis::DispatchJoin`，J2e）当作 `Stmt::epilogue` 只印一次，让每个
落入它的 case 用 `Break` 收尾而不用 `goto`；`if`/`else` 收敛分支目前没有
这个机制，每条落入同一个 hub 的 arm 各自独立走
`claimCaseBody`/`claimOrCloneSharedCaseBody`/`goto` 兜底，一个多 site 共享
的 hub 就要付出"一 site 一个 goto"而不是"一次共享内联"的代价。这正是
`docs/19` §2.4 表格里明确允许的下限（"3+ 前驱 → 保留 label+goto（或一次
shared epilogue）"，括号内的 shared epilogue 是可选项不是必须），把
`epilogue` 机制扩展到 `If` 节点是后续工作,不是本次改动的必要条件。

亲自检查了方案原文点名的 `0x117164` 区域：现在确实是嵌套
`if (var_730 == 0x0) ... else if (var_8a8 == 0x1) ... else if (var_8a8
== 0x2) ...`,不再是嵌套 `switch (state)`；链内没有跳回链头自身的 goto；
唯一一条 `goto L_0x117164` 来自函数后段一个真实的自然循环回边（重新计算
`var_730`/`var_734` 后回到同一棵决策树,不是收敛残留)。

### 回归

- `xdec_tests.exe`：**646** test cases、133851 assertions 全过（新增
  `tests/analysis/test_dispatch_region.cpp` 的 `buildDispatchNestGraph`
  两例，`tests/emit/test_structure_dispatch_region.cpp` 的
  `collapseScatterRegionSites` 四例）。
- `samples/run.ps1`：**5/5**（`manifest.json` 为 `sample_libscplugin`
  加上 `--collapse-scatter-sites`，把 `min_switches: 220` 换成
  `min_ifs: 500`，`max_gotos` 420→380、`max_lines` 6600→6900，均为
  本节实测数字加轻量余量，不是预设目标）。
- 默认行为（`collapseScatterRegionSites=false`）零改动：所有既有
  J1 回归测试（`minRegionSites`/`deferRegionCollapse` 相关）原样通过。

## 2026-08-13：结构化优化默认开启 + Goto 消除方案（Phase 0-6）

对应计划：`goto_elimination_plan`（该文件不可编辑，本节记录实测）。上一节
（`scatter dispatch 目标结构`）把 `collapseScatterRegionSites` 做成用户可选
CLI 开关，本轮的第一步是把它和 `regionStructuring`（J2 的
`collapseRegionDispatchTree`）一起收回：两者都不再是「用户可能没打开的优化」，
而是反编译器自己的默认行为，不通过启动参数暴露。收回默认值之后，
`sample_libscplugin` 的收敛形状（234 switch → 0 switch/533 if）成了唯一路径，
goto 从 289 涨到 332 的老问题（上一节的"诚实结论"）才第一次成了非 opt-in 的真实
回归，后续 Phase 1-5 就是按 §J2e/J2f/J2g/J3 的顺序把这个缺口逐步关掉。

### Phase 0：所有结构化优化内建为默认行为

- 删除 `StructureOptions::collapseScatterRegionSites`/`regionStructuring`
  两个用户字段，`switchFor` 的 `deferCollapse` 判定和 `collapseRegionDispatchTree`
  调用点都改成无条件走 always-on 分支；CLI 侧删掉
  `--collapse-scatter-sites`/`--region-structuring`。
- `deferRegionCollapse`、`minRegionSites` 仍保留在 `StructureOptions`，但只
  作为单元测试注入点存在，`decompileToC()` 的默认路径永远不设置它们。

### Phase 1（J2e-if）：`If` 也获得 join-hub epilogue

`Stmt` 的 `epilogue`/`mergeBlock` 字段本来只服务 `Switch`：

```162:164:d:\funtune\xdec\include\xdec\emit\structure.h
  StmtPtr epilogue;          // Switch, If
  /// The block `epilogue` was built from; invalid when `epilogue` is null.
  il::BlockId mergeBlock{};  // Switch, If
```

`switchFor` 的 2-way 收敛分支现在先查 `joinHubByTail()`，命中且 hub 未被占用
就把 hub 结构化一次挂成 `epilogue`，两条 arm 改用
`claimDispatcherCaseBody(..., appendBreak=false)` 收尾到 merge 而不是各自
`goto`；`printIf`/`collectDeadOpsInto`/`alwaysLeaves`/`continueAtBackEdges`/
`collectReferences` 全部跟 `Switch` 一样递归进 `epilogue`。这就是上一节留的
缺口本身：一个多 site 共享的 hub，`If` 路径现在也能"结构化一次、`Break` 收尾"，
不必每个 site 各付一个 `goto`。

### Phase 2：`if(cond) goto MERGE; else {WORK}` 补漏重组

Phase 1 的 join-hub 只在两条 arm **都**指向已注册的 pooled hub 时才生效；
`switchFor` 单条 2-way site（不是多 site 共享 hub）收敛出的
`if (cond) goto M; else {WORK; ...M 的内容...}` 这个更常见的"跳过型" diamond
仍然落不到 Phase 1。新增的 `restructureSkipGotos`/`trySplitSkipGoto`
（匿名命名空间）在 `Structurizer::run()` 里、`elideFallthroughGotos` 之前
跑一遍，把这个形状重写成 `if (!cond) {WORK}` 接着内联 `M` 的内容：

```284:323:d:\funtune\xdec\src\emit\structure.cpp
StmtPtr trySplitSkipGoto(Stmt& ifStmt) {
  ...
}

void restructureSkipGotos(StmtPtr& node) {
```

只在 CFG 层能证明「goto 目标 = 另一臂末尾 fallthrough 到的首 block，且该
label 只有这一个引用来源」时才动手，不与 `tryDiamond`/`tryOneSided` 的
既有优先级冲突（这是它们之后的 tree-level 补丁，不是抢块）。

### Phase 3（J3 扩展）：消除 `state=select(...)` 与重复 `if` 的双轨

最初以为 Phase 1/2 收敛后残留的 `if (t26==0) {state=0x18;} else
{state=0x21;}` 是 `Stmt::If` 节点，为此写了一整套
`isDeadRoutingStateDiamond`/`dropDeadRoutingDiamonds`——调试后发现这其实是
`c_stmt.cpp` 的 `printSelectAssign` 内联打印的 `Store`-of-`Select`，不在
`Stmt` 树上，那一整块代码（连同 `singleArmBlock`/`singleConstStoreOp`/
`mayReadBeforeRewrite`）已从 `structure.cpp` 撤回。真正要改的是既有的
`deadRoutingStateStore`（`c_stmt.cpp`）：它原来要求配对的 `if` 两条 arm
**都**是裸 `goto`/`continue` leaf 才信任"这个决策已经体现在控制流里"，
Phase 1/2 把其中一条 arm 变成"内联的真实 work、不再是 leaf"之后这个条件就
一直不满足，`state=...` 的冗余 store 反而更难删。放宽成"至少一条 arm 是
裸 leaf 或整条 arm 缺失"即可信任：

```912:914:d:\funtune\xdec\src\emit\c_stmt.cpp
            const bool nextArmsLeaveDirectly = isGotoLeaf(next.thenArm.get()) ||
                                               isGotoLeaf(next.elseArm.get()) || !next.thenArm ||
                                               !next.elseArm;
```

`sub_b7000` 的 if 数从 ~40 掉到 31（去掉了 `if (t26==0)` 那一整块），
`sample_libscplugin` 的 if 数从 500+ 掉到 386，goto 数不受影响（这一步删的
是重复赋值，不是控制流本身）。

### Phase 4（J2f 扩展）：if-tree 里的 `while(true)+guard` 恢复成 `while(cond)`

`wrapAsLoop` 给一个 `CondBranch` 头的自然循环包一层 `while (true)`，本意是
让循环体自己再判一次退出条件；Phase 0 的 scatter collapse 让很多这种头在
`wrapAsLoop` 看到之前就已经被收敛成 `If` 了，于是留下
`while (true) { if (cond) { continue; } else { WORK } }`——`continue` 那条
arm 纯粹是"再跳回来重新判一次"，本该就是 `while (cond)` 的循环头。新增的
`trySimplifyGuardedSpin`/`simplifyGuardedSpins`：

```1608:1610:d:\funtune\xdec\src\emit\structure.cpp
StmtPtr Structurizer::trySimplifyGuardedSpin(Stmt& whileStmt) {
  if (whileStmt.cond.valid() || !whileStmt.body) {
    return nullptr;
```

在 `restructureSkipGotos` 之后、`elideFallthroughGotos` 之前跑，把这个形状
的 `while (true)` 直接改写成 `while (cond) {}`，`WORK` 原样接在循环后面。
`sample_libscplugin` 的 `loops_while_true` 从 49 掉到 12，goto 303→273（Phase
2 的 fallthrough 消除在 splice 后的新落点上又生效了一批），if 386→349；
`sub_b7000` goto 20→19、if 31→30。

### Phase 5（J2g 扩展）：`If` 子树里的孤儿 goto 折叠

`expandUniqueCaseGotos`/`expandGotoTargets` 原来只在 `Switch` 的 case/default
槛位上找"全局唯一引用 + IL 单前驱 + 干净离开 + 不含嵌套 switch"的孤儿组，
折进去代替裸槛位；`switchFor` 2-way 收敛失败时的最终兜底
（`ifStmt->thenArm = gotoStmt(...)`）落在 `If` 的 arm 上，从未被这条 pass
扫到过。现在同一份 `tryFold` 判定逻辑也用在 `If` 的 `thenArm`/`elseArm`
本身是裸 `Goto` 的情形：

```1526:1544:d:\funtune\xdec\src\emit\structure.cpp
  } else if (node->kind == StmtKind::If) {
    // Phase 5 (goto-elimination plan §Phase5, J2g extension): switchFor's
    // own 2-way collapse (its epilogue-building branch) falls back to a
    // bare `Goto` arm whenever Phase 1's join-hub epilogue could not be
    // built for that target -- most often because some other site already
    // claimed the hub. ...
    if (node->thenArm && node->thenArm->kind == StmtKind::Goto) {
      tryFold(node->thenArm, node->thenArm->block);
    }
    if (node->elseArm && node->elseArm->kind == StmtKind::Goto) {
      tryFold(node->elseArm, node->elseArm->block);
    }
  }
```

顺带把 `node->epilogue` 的递归从只在 `Switch` 分支内调用改成无条件调用
（`If` 的 epilogue，Phase 1 加的，此前从未被这条 pass 递归过）。
`tests/emit/test_structure_unique_goto_expand.cpp` 新增两例：一例复用第一个
Switch 测试的"两臂共享 sink、regionClosed 两边都失败"技巧，只是 dispatcher
换成 `twoWaySite`（收敛成 `If`），验证 arm 折叠生效；一例验证目标仍有真实
第二引用时 arm 保持裸 `goto` 不动。在 `sub_b7000`/`sample_libscplugin` 两个
现有语料上实测无变化——两者残留的 goto 全是货真价实的多引用 hub，不是
遍历顺序偶然搁浅的孤儿，这条 pass 目前没有本地样本能触发，但机制本身由
新增单测覆盖。

### 累计效果

| 函数 | 指标 | Phase0/1 后 | Phase2 后 | Phase3 后 | Phase4 后 | Phase5 后 |
|------|------|-----:|-----:|-----:|-----:|-----:|
| `sub_b7000 @ 0xb7000` | goto | 26 | 20 | 20 | 19 | 19 |
| `sub_b7000 @ 0xb7000` | if | ~40 | ~40 | 31 | 30 | 30 |
| `sample_libscplugin @ 0x1164f8` | goto | 332 | 303 | 303 | 273 | 273 |
| `sample_libscplugin @ 0x1164f8` | if | 533 | 533 | 386 | 349 | 349 |
| `sample_libscplugin @ 0x1164f8` | while(true) | 49 | 49 | 49 | 12 | 12 |

对照计划开篇的验收目标（`sub_b7000` 26→~8，`sample_libscplugin`
332→~150-180）：两个函数都还没到位，Phase 5 的机制在这两个具体样本上没找到
可折的孤儿，主要缺口仍是 `docs/19` §4 记录的诚实下限——一个多前驱共享 hub，
`If` 路径付出"每 site 一个 goto"而不是"一次共享内联"的代价，这是比 Phase
1 的单-hub epilogue 更进一步的"多 hub 共享 epilogue"能力，未在本轮范围内。

### 回归

- `xdec_tests.exe`：**650** test cases、133860 assertions 全过（新增
  `tests/emit/test_structure_skip_goto.cpp` 两例、
  `tests/emit/test_structure_unique_goto_expand.cpp` 两例；
  `tests/emit/test_c_dead_routing_store.cpp` 新增一例、既有一例改标题澄清
  判定边界；`tests/emit/test_structure_dispatch_region.cpp`/
  `tests/emit/test_structure_region_switch.cpp` 随 Phase 0 的默认值变化
  改写）。
- `samples/run.ps1`：**6/6**（新增 `sample_sub_b7000` @ `libscplugin
  0xb7000`，`max_gotos: 22`/`min_ifs: 27`/`max_undef: 2`；
  `sample_libscplugin` 的 `max_gotos` 380→300、`min_ifs` 500→340（Phase 3 的
  `deadRoutingStateStore` 扩展先把它从 500 压到 386，Phase 4 的
  `simplifyGuardedSpins` 再压到 349，均为实测数字加轻量余量，不是预设
  目标——按 `samples/README.md` "increment slowly" 的规则，后续 Phase
  继续关闭 §J2e/J2f/J2g 缺口时再收紧）。

## 2026-08-13：absd `start()` 间接跳转 + t 变量传递（Tier A / Tier B）

对应计划：`absd_start_反编译修复`（该文件不可编辑，本节记录实测）。目标函数是
iOS Mach-O `absd` 的 `LC_MAIN` 入口 `0x100023290`，一个把状态机摊平到"每个
dispatcher 自带 anchor、共读同一张偏移表"的混淆函数。计划分两层：Tier A 让
`--rounds 1` 稳定产出可读的入口；Tier B 在 discovery 围栏下把状态机本体放进来
而不爆炸。

### Tier A（A1-A6）

| 项 | 落点 | 实测 |
|----|------|------|
| A1 稀疏值集 | `preciseIndexSet`（`index_bound.cpp` 的 `exactValues`）+ `tableCandidates` 优先用它 | L0 候选 12 → 2 |
| A2 `Add`/`Sub` + cinc | `localBound`/`armBound` 认常量偏移与 `select(cond, x, x+1)` | 单测覆盖；absd 的 L1 本身无 guard，候选数不变 |
| A3 anchor 常量 | `macho.cpp` 把 `__DATA_CONST`/`__AUTH_CONST` 记为 fixups 后只读；`image.cpp` 的 `isImmutable` 放宽 `RelocKind::Relative` | anchor 折成常量，`g_1000a8140` 写入解析正确 |
| A4 tableMode | `switchFor` 强制 table 模式 + `matchDispatchValues` 回落 `preciseIndexSet` | 入口 `BR` 输出 `switch (index)` / `case 0xa:` / `case 0xb:` |
| A5 边拷贝裁剪 | `edgeCopies` 跳过恒等拷贝（`tN = tN`） | rounds=3 时 temp 94830 → 94484 |
| A6 manifest | `sample_absd_start`（rounds=1） | 117 行 / 2 goto / 34 temp / 1 switch / 2 sealed |

### Tier B（B1-B6）

B1/B2 是"能不能有边界"：`FunctionFence::enforce` 让围栏外的 discovery 真的不
进 entries（此前围栏只是 advisory），`DriverOptions::maxDiscoveryPerBranch` +
CLI `--discovery-cap` 让单条分支一次提供的 missing 目标超过上限时整条分支被拒
（留作 sealed），CLI 另加 `--max-span`。absd 的分支目标数分布是两级的——

| `--rounds 4 --discovery-cap` | 行数 | goto | temp | switch | label | sealed |
|---:|---:|---:|---:|---:|---:|---:|
| 16 / 32 / 36 | 117 | 2 | 34 | 1 | 2 | 2 |
| 40 / 48 / 64 / 128 | 10831 | 366 | 20879 | 10 | 269 | 24 |

**没有中间尺寸**：这个函数唯一"大"的 dispatcher 提供约 40 个目标，放它进来就
一次带进约 300 个 block，所以计划里 Tier B 的 500–2000 行目标对*这个函数*不可
达。cap 32 的输出与 `--rounds 1` **逐字节相同**（`Compare-Object` 差异 0 行），
因此没有必要为它再注册一个样本；注册的 `sample_absd_start_l2` 用的是 cap 64
的"放进来"形态，按 `sample_libscplugin` 的先例作观测型（非目标型）阈值。

B3 才是让那 10831 行变得能读的部分，两处都是"分析没认出表"而不是"结构化没折"：

1. **表项指回自己的表读之后**。表 `0x100081cb0` 的第 37 项是 `0x1c`，加上
   anchor `0x1000238b8` 正好落在 dispatcher 自己的 `add x11, x11, x12; br x11`
   上——跳过去等于不重读表、拿着已经算完的地址再加一次 anchor，下一跳必然落到
   映射外。留着它，这条边会以自环解析，而自环给表读前面插了一个 phi，
   `matchJumpTable` 就再也认不出后面是表：整个 dispatcher 退化成 40 路
   `if (t + 0x1000238b8) == 0x...` 比较链。修法是在候选阶段就把"落在本块表读
   之后、分支之前"的候选丢掉（落在表读*之前*的保留——那是重读表的普通
   dispatch 循环）：

```157:164:d:\funtune\xdec\src\passes\resolve_indirect.cpp
  /// Where in `block` this branch reads the table it dispatches through: the
  /// address of the last memory read in `block` that `target` depends on,
  /// following values transitively. Nullopt when the expression reads no
  /// memory this block defines -- the read happened somewhere else, and
  /// nothing about where a target lands inside this block can skip it.
  [[nodiscard]] static std::optional<uint64_t> lastTableReadIn(const il::Function& function,
                                                               il::BlockId block,
                                                               il::ExprId target) {
```

2. **region 聚类不该用 anchor**。偏移表的 anchor 是"读它的那个 site 自己在哪
   儿"，absd 的 6 个 site 各有各的 anchor，于是 `findDispatchRegions` 把它们分
   成 6 个单 site region，`findDispatchJoins`/`matchRegionSharedTail` 的"跨 site
   汇总证据"就无从谈起。`TableKey` 去掉 anchor 之后：7 region/7 site →
   2 region/7 site，其中一个是 6 site 的 scatter region（nest depth 6）。

这两条合起来把 5 个地址比较型 dispatcher 变回 `switch (index)`（10 switch 中
现在只剩 1 个是地址判别式）。另外 `switchFor` 在表匹配失败时不再打印比较链，
而是打印以目标地址为判别式的 `switch`（`Stmt::addressCases`）：同样的语义，
判别式只写一次而不是每臂一次；`alwaysLeaves` 对它保持保守（和比较链一样没有
default，可以落空）。

### 两个测量后回退/无效的项

- **B4（state 寄存器提升）**：计划假设 absd 的状态在某个寄存器里跨 block 存活，
  只是没到 promotion 阈值。实测不成立。按"同一寄存器出现在 ≥4 个 site 的
  index 里"命名，选出的寄存器有 **252** 个 phi；放宽成"index 读到的所有
  register phi"是 **68** 个，其中还包含被后续指针推断定型成 `uint64_t*` 的基
  址值和宽度为 0 的标志值——没有一个是"读者能跟着走的那个 state"。真实形状
  是：absd 每个 site 都从新载入的标志位重算 index（`w9 + w10`、
  `w14 + or(zext(!cmp)...)`），libscplugin 的 index 干脆是两个字面量的
  `select`（`matchDispatchValues` 已经把它们打成 case 标签）。语料里没有
  "一个寄存器承载 scatter 状态"的样本，因此这一项整体回退，`variables.cpp`
  只留一条指回本节的注释。
- **B5（scatter 边拷贝抑制）**：`matchLiveRegisterFrame` 原来只在
  `shape->merge == mergeBlock`（单块投票出的 dispatcher shape）时才匹配，J2e
  的 pooled join hub 因为"没有自己的 `hub` 可比对"而总是全量打印 case 保存。
  新增 `Structurizer::frameThroughJoinHub`：hub 只有一个后继时，就用那个后继
  充当 shape 的 `hub`，relay 的判定与投票出的 merge 完全一致。机制由
  `tests/emit/test_structure_join_epilogue.cpp` 新增一例覆盖（hub 与其下方
  block 都有 `reg:x5` phi → frame 有 1 个 slot），但在 8 个样本上实测 0 变化：
  absd 的 scatter region 根本没有 join hub（label 最多 5 个前驱，
  `sharedTail=false`），已有 hub 的样本走的是投票路径。
- 顺带把 absd 剩下的 8102 条拷贝行做了分类，供后续参考：**7122** 条是纯
  `tA = tB;`。这些是 phi 下降的必然产物，要真正消掉需要带干涉判定的 phi
  coalescing（SSA destruction），不在本轮范围内——这是"t 变量传递"这条根因
  剩下的主体。

### 回归

- `xdec_tests.exe`：**665** test cases 全过（新增
  `tests/passes/test_resolve_indirect.cpp` 两例（表项指回表读之后 / 指回表读
  本身）、`tests/analysis/test_dispatch_region.cpp` 两例（同表不同 anchor 归
  一个 region / 不同表仍分开）、`tests/emit/test_structure_join_epilogue.cpp`
  一例）。
- `samples/run.ps1`：**8/8**（新增 `sample_absd_start_l2`；`baseline.json`
  已更新）。

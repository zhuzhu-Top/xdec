# 27 — Library-First 反 VMP 架构方案（以 128-Register `XIMG` 虚拟机为端到端参照）

> **定位**：本文档定义 `xdec` 作为**可编程底层反编译库（C++ SDK + Python 绑定 `pyxdec`）**支撑通用反 VMP（Devirtualization）的完整架构设计，并以一个真实的 **64 位 128 寄存器自定义虚拟机载荷（`XIMG`）**作为端到端参考工作流与验收标尺。
>
> **约束**：严格遵守 [`00-core-vs-plugin-prompt.md`](./00-core-vs-plugin-prompt.md) 的核心边界——**Core 只提供架构无关、保护器无关的通用原语与库接口；所有针对特定 VMP 的解包、AST/IL 模式匹配识别、虚拟 ISA 提升策略均留在外部 Python 脚本或插件中。**

---

## 1. 为什么采用「xdec 做底层库 + Python 做去 VMP 编排」

### 1.1 现实困境：各家 VMP 千差万别，硬编码或纯 C++ 插件迭代太重

商业 VMP 与自定义虚拟机在以下维度几乎每家都不一样：
- **载荷封装**：明文段、XOR/zlib 压缩壳、内嵌类 ELF 镜像（如 RQT `XIMG`）、运行时 `call_once` 解密；
- **虚拟架构**：寄存器机（如 RQT 128×64-bit 寄存器）、栈机（`vSP` push/pop）、混合内存机；
- **字节码编码**：定长/变长、PC 相关 XOR 解混淆、滚动 key、opcode 二次重映射表（`.rodata_v`）、源寄存器差分编码；
- **Handler 形态**：大 `switch(opcode)`、跳表分发、直接线程化（Direct Threaded `br x8`）、夹杂 MBA/OLLVM/自校验。

如果每分析一家新 VMP 都要改 `xdec` 核心代码，或者反复编译调试 C++ `.dll` 插件，探索周期极长；反之，如果只用纯 Python 手工扣模拟器、盯几百个基本块汇编逐条翻译几十个 opcode、再从十几万条指令 trace 里人肉归纳加密算法（例如此前的 RQT 还原方式），虽然能做出来，但**无法复用反编译器的 SSA 优化、常量折叠、MBA 化简、控制流结构化与类型恢复能力**。

### 1.2 目标范式：Library-First 双向开放

让 `xdec` 从单一 CLI/MCP 工具演进为 **Library-First 反编译基础设施**（`libxdec` + `pyxdec` Python 绑定），形成一条极其通用的四阶段闭环：

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│                     用户脚本层 (Python / pyxdec，按具体 VMP 定制)              │
│                                                                              │
│  [1] Load & Bootstrap     调 xdec.Image / ExecSession / Tracedb 获取解密镜像  │
│            │                                                                 │
│            ▼                                                                 │
│  [2] AST/IL 模式识别      调 xdec.decompile() 拿到解释器的结构化 AST + IL      │
│      (Handler Discovery)  用 Python 模式匹配自动识别每个 case/handler 语义     │
│            │                                                                 │
│            ▼                                                                 │
│  [3] 提升为 xdec IL       按识别出的解码规则 + Handler 语义，调 FunctionBuilder│
│      (Lift to xdec::il)   把字节码流提升为标准 il::Function (支持自定义虚拟ISA)│
│            │                                                                 │
│            ▼                                                                 │
│  [4] 优化发射与差分校验    送回 xdec 核心管线输出干净 C + il::Interpreter 校验 │
└───────────────────────────┬──────────────────────────────────────────────────┘
                            │  libxdec / pyxdec (零样本特化的稳定底层 API)
                            ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                        xdec Core (通用底层基础能力)                           │
│  • Dual-View 导出: 结构化 HlStmt/HlExpr AST + 精确关联的 il::Function/Expr    │
│  • 数据驱动虚拟 ISA: il::RegisterFile + VirtualAbi + decompileFromIL()        │
│  • 分层内存与区域别名: LayeredByteReader + MemoryRegionMap                    │
│  • 通用虚拟状态消除: RegionMem2Reg + RegionDSE + Full-Width (FP/SIMD) SSA     │
│  • 可选部分求值展开: devirt::Specializer (按 (pc, StateKey) 自动特化)         │
│  • 执行与校验闭环: ExecSession + Tracedb Oracle + DiffVerifier                │
│  • 现有全套后端: Ssa -> Optimized(SCCP/Algebra/MBA) -> Vars -> Structured -> C│
└──────────────────────────────────────────────────────────────────────────────┘
```

**这一范式的最大价值**：
- **探索期极快**：用 Python 遍历解释器的 AST/IL 写模式规则，秒级批量识别几十上百个 handler；
- **彻底白嫖反编译器后端**：一旦字节码被提升为合法的 `xdec::il::Function`，`xdec` 现有的常量传播、MBA 代数化简、死码消除、循环/分支结构化、变量恢复、C 代码发射全部自动生效；
- **自带正确性闭环**：提升后的 IL 可直接交由 `il::Interpreter` 执行，与真机向量或原版 `ExecSession` 自动做 bit-exact 差分校验。

---

## 2. 端到端参考任务详述：RQT 虚拟机去 VMP

下面以一组脱敏后的真实 **64 位 128 寄存器虚拟化密码模块（RQT）**作为完整蓝本，逐层拆解真实去 VMP 任务对 `xdec` 底层库提出的具体需求。

### 2.1 分层保护结构

```text
TokenManager::computeToken(data, len, cmd, ...)
  └─ 安全模块虚表工厂 vm_ctx_factory(&ctx, 0x28)
       调用 ctx[+0x28](md5_hex_ptr, 32, cmd):
         ├─ Stage 0: std::call_once 解密内嵌载荷 "vm_payload.dat" -> XIMG (ELF64 变体)
         │      step1 = raw ^ (len(raw) % 255)      # xor 7 -> zlib stream
         │      step2 = zlib.decompress(step1)      # 115,776 B
         │      plain = step2 ^ (len(step2) % 255)  # xor 6 -> "XIMG"
         ├─ Stage 1: 初始化 0x80440 B VM 上下文
         │    +0x00000 .. : 虚拟寄存器 / 运行态字段，最终 *(uint32_t*)vm 为返回值
         │    +0x80418    : 初始停机哨兵 HALT_PC (指向 OP 21)
         │    +0x80420    : 已加载的 XIMG 句柄
         └─ Stage 2: 进入 ARM64 解释器 vm_interp(vm, ctx)
              解释执行 XIMG 导出函数 cs(md5_hex, 32, &out_u32, &out_len)
              动态恒定执行 134,059 条 VM 指令，输出 0x44xxxxxx
```

> 注：外层工厂与重型入口函数常进一步叠加**重度 OLLVM 控制流平坦化与浮点/向量寄存器搬运**，这对 `xdec` 的底层能力提出了更高要求。

### 2.2 载荷镜像 `XIMG` 的内部布局

`vm_payload.bin`（`115,776 B`）是一个魔改 magic（`XIMG`）的标准 ELF64 镜像，包含完整的代码、数据与重定位信息：

| 节区 / 结构 | 偏移 / 大小 | 作用 |
|---|---|---|
| `PT_LOAD` 段 | 映射至基址 `IMG_BASE`（16KB 对齐，满足 `ADRP` `& ~0x3fff`） | 装载虚拟代码 `.text`、只读常量 `.rodata`、数据 `.data` |
| `.dynsym` | 唯一导出符号 `cs`（`.text @ 0x4000`） | 虚拟机内入口函数 `cs(md5_hex, len, &out, &out_len)` |
| `.rela.dyn` | 24B/项重定位表 | 修正镜像内全局指针（`IMG_BASE + r_addend`） |
| `.init_array` | `0x1c000` -> `0x16520` | 运行时初始化函数（解密 `.data` 处的调试串 `"sign.c\0"`） |
| `.rodata` | `0x290`（`0x240 B`） | `0x290..0x2cf`：64B HMAC Key；`0x2d0..0x3cf` & `0x3d0..0x4cf`：两张 256B 魔改 SHA-256 轮常数表 |
| `.rodata_v` | `0x4d0`（`0x100 B`） | 54 对 `(enc_op, dec_op)` 操作码重映射表 |

### 2.3 虚拟 ISA 与指令编码格式（解释器 `vm_interp`）

`vm_interp` 包含 **513 个基本块**，实现了一台类 RISC 的 **64 位寄存器虚拟机**：

- **寄存器文件**：`R[0..127]`（128 个 64-bit 通用寄存器），其中：
  - `R[0..7]`：参数传递与返回值（遵循类似 AAPCS64 的虚拟 ABI）；
  - `R[125]`：虚拟栈指针 `vSP`（指向 `0x80000 B` 虚拟栈）；
  - `R[127]`：虚拟链接寄存器 `vLR`（入口置 `HALT_PC`）。
- **指令字解混淆**（每条定长 32-bit `insn`，位于 `cur_pc`）：
  - 操作码：`raw_op = ((insn >> 26) & 0x3f) ^ ((cur_pc >> 8) & 0x3f)` → 查 `.rodata_v` 重映射得真实 `op ∈ [1..55]`；
  - 指令格式：查 `OP_FMT[56]` 表，得到 `fmt ∈ {1, 2, 3, 4}`；
  - 目的寄存器：`rd = ((insn >> 19) & 0x7f) ^ ((cur_pc >> 2) & 0x7f)`；
  - 源寄存器（差分编码）：`rs1 = ((insn >> 12) ^ (insn >> 19)) & 0x7f`，`rs2 = ((insn >> 5) ^ (insn >> 12)) & 0x7f`；
  - 立即数：按 `fmt` 从低位切片并做符号扩展 / 移位（如 `ADRP` 页偏移、`CALL`/`B`/`BCC` PC 相对跳转、`LOAD/STORE` 偏移）。
- **47 个核心 Opcode 的语义分类**（典型 RISC 语义，极易从解释器 AST/IL 自动聚类）：
  1. **算术与位运算（ALU）**：64/32-bit `ADD / SUB / MUL / SDIV / UDIV / SREM / UREM / AND / OR / XOR / SHL / LSR / ASR / ROR`（寄存器-寄存器 与 寄存器-立即数两套）；
  2. **访存（Memory）**：`LD8 / LDS8 / LD16 / LDS16 / LD32 / LDS32 / LD64` 与 `ST8 / ST16 / ST32 / ST64`（基址+立即数偏移 / 基址+变址）；
  3. **常量与地址加载**：`MOV_IMM`、`MOVK`、`ADRP`（`(cur_pc & ~0x3fff) + (imm << 14)`）；
  4. **比较与控制流**：`CMP` / 条件分支（`BEQ/BNE/BLT/BLE/BGT/BGE/BLTU/...`）、无条件跳转 `B`、函数调用 `BL`（写 `R[127] = cur_pc + 4`）、间接跳转/返回 `BR / RET`（`cur_pc == HALT_PC` 停机）；
  5. **防篡改与杂项**：解释器中穿插自校验调用（`sub_1138b009c`、`sub_11389e238`），但与 `cs` 的数据流完全无关。

### 2.4 剥离虚拟机后的真实算法（期望反编译出的目标形态）

当 `cs` 及其内部子函数被提升为 `xdec::il::Function` 并经由 `xdec` 后端优化与结构化后，134,059 条动态虚拟机指令应直接坍缩为 4 个清晰的标准 C 函数：

1. **`cs(md5_hex, 32, &out_u32, &out_len)`**：编排主流程，无任何输入相关分支；
2. **`mod_sha256` + 魔改 HMAC**：
   - `ipad = HMAC_KEY ^ 0xec`，`opad = HMAC_KEY ^ 0xdf`（`HMAC_KEY` 来自 `.rodata@0x290`）；
   - 6 处魔改：自定义 IV、按 `i % 3 == 0` 切换两张轮常数表（`.rodata@0x2d0` / `0x3d0`）、填充起始字节 `0xfc` + 位长按位取反 `~bitlen`、消息字字节序 `(b0, b3, b2, b1)`、自定义移位量 `(3,11,5)/(5,9,2)/(5,11,19)/(7,13,17)`、反向 Davies-Meyer 累加 `H[i] += state[7-i]`；
3. **`mod_md5`**：
   - 6 处魔改：自定义 IV、填充起始字节 `0xda` + 位长低 32 位异或 `0x64130efb`、消息字序 `(b3, b0, b1, b2)`、自定义 64 轮常数表 `T[0..63]`、自定义 4 轮非线性函数且全部 `ROTL32 -> ROTR32`、Davies-Meyer 交换 `a/d` 累加；
4. **`roll133` 与最终拼装**：
   - 8 位滚动哈希 `acc = (acc * 133 + b) & 0xFF` 扫过 `s3[0:14]`、`s3[1:15]`、`s3[2:16]`；
   - `rqt = 0x44000000 | ((h2 & 0x7B) << 16) | ((h1 & 0x6F) << 8) | (h0 & 0x3F)`。

**验证标尺**：`rqt/data/rqt_probe_vectors.json` 中的真机抓包向量（如 `a4ca8e3beece0a45f3545b0421b10a4a -> 0x44704227`）及 100 组随机输入，要求反编译前后的 IL 执行结果 **106 / 106 Bit-Exact 一致**。

---

## 3. 从 RQT 抽象出的 `xdec` 底层 7 大通用基础能力

要把上面的人工逆向过程变成**任何 VMP 都能复用的通用 Python + `xdec` 库工作流**，`xdec` Core 需要补齐以下 7 项通用基础能力（全部满足 `00-core-vs-plugin-prompt.md` 的泛化性检验）。

---

### 能力 1：Dual-View AST / IL 结构化查询接口（解决 Handler 模式识别）

#### 现状缺口
检查 `include/xdec/emit/ast.h` 可知，当前 `xdec` 已经有非常清晰的控制流语句树 `HlStmt`（`Switch` / `HlSwitchCase` / `If` / `While` / `Assign` / `Return`），但表达式节点 `HlExpr` 在 `ast_lower.cpp` 中把算术、位运算、移位、比较都预渲染成了扁平字符串 `HlExprKind::Raw`。
- 如果外部 Python 脚本只拿到字符串 `Raw`，就只能写脆弱的正则去解析 C 代码，无法做可靠的 AST 模式匹配。

#### 设计方案
`xdec` 暴露 **Dual-View（双视图）检查接口**，同时兼顾结构化可读性与精确语义匹配：

1. **AST 控制流视图（`HlFunc` / `HlStmt` / `HlExpr`）**：
   - 补齐 `HlExpr` 结构化模式（`EmitAstOptions::structuredExprs = true`），将算术、位运算、移位、比较、截断/扩展、条件选择输出为真正的 `HlExpr` 树节点（`Binary`/`Unary`/`Cast`/`Deref`/`Select`），而不是塌缩成 `Raw` 字符串；
   - 每个 `HlStmt` 与 `HlExpr` 可选携带其溯源的 `il::BlockId`、`il::OpId`、`il::ExprId` 与 `va` 列表。
2. **规范化 IL DAG 视图（`il::Function` / `il::Block` / `il::Op` / `il::Expr`）**：
   - `il::Expr` 本身已经经过 `ssa-optimize` + `algebra` 规范化，并且是 **hash-consed DAG**，天然拥有强类型 `ExprOp`（`Add`, `Sub`, `Xor`, `Shl`, `Lshr`, `Ashr`, `Rotr`, `CmpLtS`, `ZExt`, `SExt`, `Select` 等）与精确位宽 `Type`（`i8/i16/i32/i64/i128/f32/f64`）。
   - Python 层可以用 `HlStmt(Switch)` 瞬间定位所有 `case op_id:` 对应的块范围，再用统一的模式匹配器直接匹配 `HlExpr` 或底层 `il::Expr` DAG。
3. **提供通用的树/图匹配工具（Core 可内置轻量 matcher，也可在 Python 侧封装）**：
   - 提取一个 handler 块内的 **Effect Summary（副作用摘要）**：
     - 读取了哪些相对基址槽位（如 `vm_ctx + rs1*8`、`vm_ctx + rs2*8`）；
     - 写入了哪个相对基址槽位（如 `vm_ctx + rd*8`）或修改了哪个 `vPC` 变量；
     - 核心纯表达式树是什么（如 `Xor(Load(vm_ctx + rs1*8), Load(vm_ctx + rs2*8))`）；
     - 是否触发内存访问 `Load/Store(addr, width, signed)`、分支条件 `Cmp(op, a, b)` 或外部 `Call`。
   - **效果**：对 RQT 这样 47 个 opcode 的大 `switch`，Python 写 ~60 行模式规则即可自动识别出 40+ 个标准 handler，只需人工检查剩下的几个特殊指令（如 `ADRP`、`HALT`、系统调用桥）。

---

### 能力 2：架构无关的 `il::Function` 构建器与中段接入（`VirtualAbi` + `decompileFromIL`）

#### 现状发现
`include/xdec/il/register_file.h` 本身已经做得极其超前：**`RegisterFile` 是完全数据驱动的，不硬编码 ARM64**！它可以动态添加任意数量、任意位宽、带父子视图的虚拟寄存器（如 `R0..R127` + `vSP=R125` + `vLR=R127`）。

真正的缺口在**后半段管线对 AArch64 的隐式假设**：
- 目前 `decompile::decompileToC` 只能从 `uint64_t entry` + `SpecEngine`（机器码 lifter）起步；
- `passes/vars`、`analysis/stack_frame`、`analysis/call_abi` 内部默认按 AArch64 调用约定（`x0..x7` 入参、`x0` 返回、`sp`/`x29` 栈帧、`x19..x28` callee-saved）分析函数。

#### 设计方案

1. **抽象 `il::CallingConvention` / `VirtualAbi`（纯数据描述，挂在 `DecompileFromILOptions`）**：
   ```cpp
   namespace xdec::analysis {

   struct AbiSpec {
     std::string name;                        // e.g. "aapcs64", "rqt-vm-64", "stack-vm"
     il::RegId stackPointer;                  // e.g. R125 (或无效表示无显式 SP)
     il::RegId framePointer;                  // 可选
     il::RegId linkRegister;                  // e.g. R127
     std::vector<il::RegId> intArgRegs;       // e.g. R0..R7
     std::vector<il::RegId> intRetRegs;       // e.g. R0
     std::vector<il::RegId> calleeSavedRegs;  // 跨调用保持不变的虚拟寄存器
     std::vector<il::RegId> callerSavedRegs;  // 调用会破坏的虚拟寄存器
     int64_t stackGrowthDir = -1;             // -1 向下增长, +1 向上增长
     unsigned pointerBits = 64;
   };

   }  // namespace xdec::analysis
   ```
   - AArch64 只是内置的默认 `AbiSpec` 实例；任何自定义虚拟机的 `RegisterFile` + `AbiSpec` 都是一等公民！
2. **开放中段反编译入口 `decompile::decompileFromIL`**：
   ```cpp
   namespace xdec::decompile {

   struct DecompileFromILOptions {
     analysis::AbiSpec abi;                   // 虚拟或原生 ABI 描述
     devirt::LayeredByteReader* memory = nullptr; // 虚拟镜像只读/叠加内存 (用于常量折叠与字符串/表解析)
     const devirt::MemoryRegionMap* regions = nullptr; // 区域别名提示
     const types::TypeDatabase* types = nullptr;
     pass::NameAt names;                      // 虚拟符号表 (如 0x10004000 -> "cs")
     emit::EmitOptions emit;
     il::Maturity startMaturity = il::Maturity::Cfg; // 允许从 Lifted / Cfg / Ssa 任意合法阶段切入
   };

   /// 跳过原生指令 lifter，直接将外部构造的 il::Function 校验后送入
   /// Ssa -> RegionMem2Reg -> Optimized -> Vars -> Structured -> Typed -> Emit 管线
   [[nodiscard]] Result<DecompileToCResult> decompileFromIL(
       std::unique_ptr<il::Function> function,
       pass::Registry& registry,
       const DecompileFromILOptions& options);

   }  // namespace xdec::decompile
   ```
3. **安全的 `il::FunctionBuilder`（供 C++ 与 Python 调用）**：
   - 封装 `il::Function` 的块创建、表达式构造、Op 追加、`rebuildEdges()` 与自动 `il::verify()` 报错（定位到具体 block/op 与违规原因）；
   - 允许为每个虚拟指令设置 `va = vPC`，从而在最终生成的 C 代码里保留完整的 `/* vPC */` 溯源注释，并支持通过 `xdec` 的观测器（`observe`）逐步 dump `Cfg -> Ssa -> Optimized -> Vars -> Structured` 中间态！

---

### 能力 3：分层覆盖内存与区域属性契约（`LayeredByteReader` + `MemoryRegionMap`）

反 VMP 往往涉及多层内存叠加（原始二进制 + 解密出来的 `XIMG` + 经过 `.rela.dyn` 重定位与 `.init_array` 初始化的节区），并且需要区分「常量字节码/查表」、「私有虚拟寄存器/栈」与「真实业务 I/O 内存」。

```cpp
namespace xdec::devirt {

enum class RegionKind : uint8_t {
  ImmutableData,   // 只读字节码 / XIMG .rodata (如 HMAC_KEY, SHA256 K 表)：允许 const-fold-memory 折叠
  PrivateRegFile,  // 虚拟寄存器数组 (如 vm_ctx[0..N])：无外部别名，允许 RegionMem2Reg 提升
  PrivateStack,    // 虚拟栈内存 (如 vSP 所在 0x80000 B 缓冲区)：允许栈槽提升与 DSE
  ExternalMemory,  // 业务输入/输出缓冲 (如 md5_hex, &out_u32)：严格保留内存语义
};

struct MemoryRegion {
  std::string name;
  RegionKind kind;
  std::optional<uint64_t> fixedBase;
  std::optional<il::RegId> baseReg;  // 例如指向 vm_ctx 或 vSP 的寄存器
  uint64_t size = 0;
};

class LayeredByteReader {
 public:
  void setBase(ByteReader base);
  void addOverlay(uint64_t baseVa, std::vector<uint8_t> data, bool immutable);
  [[nodiscard]] ByteReader asByteReader() const;
  [[nodiscard]] MemoryFacts asMemoryFacts() const; // 自动生成给 const-fold-memory 的不可变区间
};

}  // namespace xdec::devirt
```

**对 RQT 的直接收益**：
- 把装载并重定位后的 `XIMG`（基址 `0x10000000`）作为 `ImmutableData` 叠加层喂给 `decompileFromIL`；
- `xdec` 现有的 `const-fold-memory`（`passes/const_fold_memory.h`）会自动把 `ADRP + ADD` 指向 `.rodata@0x290` / `0x2d0` / `0x3d0` 的常量加载解析为具体数据引用或内联常量，无需脚本手工处理！

---

### 能力 4：通用基址内存提升与死存储消除（`RegionMem2Reg` + `RegionDSE`）

- **解决什么问题**：
  - 路线 1（直接提升到虚拟寄存器 `R0..R127`）：虚拟函数内部会在虚拟栈 `R125 (vSP)` 上保存局部变量、SHA-256 的 64 词消息扩展数组 `W[64]`、MD5 的 16 词缓冲等；需要现有 `stack_prop` + `vars` 遵循 `AbiSpec::stackPointer`（不再硬编码 ARM64 `sp`）。
  - 路线 2（直接对解释器做状态展开，不重写虚拟寄存器）：展开出的 IL 全是围绕堆/TLS 指针 `vm_ctx`（如 `*(vm_ctx + 0x18)`）或 `vSP` 的访存。
- **Core 新增两个通用中端 Pass（位于 `Ssa -> Optimized`）**：
  1. **`passes::RegionMem2Reg`**：将 `MemoryRegionMap` 中标记为 `PrivateRegFile` / `PrivateStack` 且经逃逸分析证实未逃逸到外部未知调用的常量偏移槽位 `base + offset`，利用标准 Dominator Tree + SSA 构造算法整体提升为 SSA 标量与 Phi 节点。
  2. **`passes::RegionDSE`**：根据 `AbiSpec` 声明的返回/活出槽位做反向活跃分析，清除所有无外部消费者的私有区域写入（如死虚拟标志位、`vPC` 中间更新、自校验冗余状态）。

---

### 能力 5：可选的状态敏感部分求值引擎（`devirt::Specializer`）

除了「AST 识别 handler → Python 提升到自定义虚拟 ISA IL」（下文称 **模式 1：Lifting 路线**），还存在一类特殊场景：
- 某些保护器没有规整的 47-case 字节码（例如 8.0.80 的 `0x11475b5e4` / `0x114d2eee0` 重度 OLLVM CFF，或 handler 数量达数千且带多态加密无法逐个手写解码规则）。

为此 Core 同步提供通用的 **模式 2：`devirt::Specializer`（上下文敏感部分求值器）**：
- 以 `(native_pc, StateKey)` 为展开节点（OLLVM 的 `StateKey = (state_reg)`，滚动 key VM 的 `StateKey = (vPC, rolling_key)`）；
- 常量内存读取与常量条件分支在展开期直接求值折叠，符号分支自动分叉，命中已有 `(native_pc, StateKey)` 自动连回边形成循环；
- 最终同样产出标准 `il::Function`，与模式 1 在 `decompileFromIL()` 处完全合流。
- **Python 同样可直接驱动 `Specializer`**：只需传一个 Python 回调 `state_key_fn(pc, state)`，即可对任意 OLLVM/VM 函数执行受控展开。

---

### 能力 6：统一执行/Trace 预言机与 Bit-Exact 差分验证（`RuntimeOracle` + `DiffVerifier`）

`xdec` 已经内置了三大重型组件：
- `exec::ExecSession`（`include/xdec/exec/session.h`）：基于 `xspec` 的确定性跨块执行器；
- `storage::Tracedb`（`include/xdec/storage/tracedb.h`）：v5/v6 动态执行轨迹数据库；
- `il::Interpreter`（`include/xdec/il/interp.h`）：直接解释执行任意 `il::Function` 的 IL 解释器！

将它们统一暴露到库层（C++ & Python）后，反 VMP 工作流将具备两大杀手级能力：
1. **自动预热解包（Bootstrap）**：Python 调 `xdec.ExecSession` 直接跑原二进制里的解密初始化函数（如 RQT 的 `sub_1138b1518` 或 `XIMG` 的 `.init_array`），把执行产出的脏内存页一键导出为 `LayeredByteReader` 叠加层；
2. **自动化 Bit-Exact 差分验证（`DiffVerifier`）**：
   - 输入一组测试向量（如 `rqt_probe_vectors.json` + 100 组随机 MD5）；
   - 一侧跑 `il::Interpreter` 执行**提升并优化后的 `il::Function`**（在 `Cfg` 与 `Optimized` 两个阶段都可跑，既验 lifting 正确性，也验 Core 优化 pass 有无引入语义偏差！）；
   - 另一侧对照真机向量或 `ExecSession` 跑原版 VMP 函数；
   - 逐字节断言输出一致，任何 1 bit 偏差立即定位到首次分歧的 `vPC` / 基本块。

---

### 能力 7：底层 IR 补齐（支撑现代高强度混淆样本）

结合大型真实 iOS Mach-O 混淆函数的实测结果，以下底层缺口直接制约重型安全函数的 AST 分析，需纳入地基一并补齐：

1. **浮点与向量寄存器（`v0..v31 / d0..d31 / q0..q31`）接入 SSA**：彻底消除 `not tracked by SSA` 与 `/*fcmp.uno?*/0x0` 退化，保证用浮点/NEON 寄存器搬运常量或做加密运算的 handler 不丢数据流；
2. **可验证的代数 / MBA 规则注册（`AlgebraRuleRegistry`）**：允许 Python/C++ 插件注册额外恒等化简规则，注册时强制经 `il::tryEvalConst` 随机绑定 oracle 校验等价性，拒绝错误规则；
3. **Driver 发现拦截器（`IDiscoveryHook`）**：反编译大解释器函数时（如 `0x1148eac3c`），允许外部脚本过滤无效共享跳表或提供 `Tracedb` 观测目标，防止 1 轮爆炸上千不可达块导致超时。

---

## 4. Library 与 Python Binding (`pyxdec`) API 设计

### 4.1 构建与绑定架构

- **C++ Library 层**：将当前紧耦合在 CLI 的加载/分析/反编译能力整理为干净的静态/动态库 `libxdec`（扩展 `include/xdec/api.h`）；
- **Python Binding 层 (`pyxdec`)**：使用 **`nanobind`**（轻量、编译快、零开销支持 `std::span` / `std::vector` / enum / 回调，适合 C++20）直接绑定 `libxdec`，打包为可直接 `import xdec` 的原生扩展模块（`.pyd` / `.so`）；
- **与现有 C++ 插件系统的关系**：Python (`pyxdec`) 负责探索期 AST/IL 识别、快速去 VMP 脚本开发、批量分析与向量校验；成熟后如需零 Python 依赖集成进 `xdec serve --mcp` 或 CLI，可将同一套逻辑无痛下沉为 `XdecPluginHost` C++ 插件，底层调用完全一致的 Core API。

### 4.2 `pyxdec` 核心 Python API 一览

```python
import xdec

# 1. 镜像、内存叠加与基础查询
img = xdec.Image.open("target_binary")
mem = xdec.LayeredMemory(img)
mem.add_overlay(base_va=0x10000000, data=ximg_bytes, immutable=True)

# 2. 反编译解释器 -> 获取 Dual-View (结构化 AST + IL + 原始 C)
res = img.decompile(interp_va, structured_exprs=True)
ast_fn = res.ast        # HlFunc: 可遍历 HlStmt (Switch/If/While/Assign/Call) 与 HlExpr 树
il_fn  = res.il         # il.Function: 可遍历 Block / Op / Expr DAG
c_code = res.c_text     # 人类可读 C

# 3. 自定义虚拟架构与 IL 构建器
rf = xdec.il.RegisterFile()
regs = [rf.add(f"r{i}", 64, xdec.il.RegClass.General) for i in range(128)]
rf.set_class(regs[125], xdec.il.RegClass.StackPointer)
abi = xdec.AbiSpec(
    name="rqt-vm-64",
    stack_pointer=regs[125],
    link_register=regs[127],
    int_arg_regs=regs[0:8],
    int_ret_regs=[regs[0]],
    callee_saved_regs=regs[19:29],
)

builder = xdec.il.FunctionBuilder(arch=xdec.Arch.Custom64, registers=rf, entry_va=0x10004000, name="cs")
b0 = builder.create_block(va=0x10004000)
# builder.read_reg / write_reg / load / store / add / sub / xor / rotr / cond_branch / call / ret ...
v_fn = builder.finalize(maturity=xdec.il.Maturity.Cfg)   # 内置自动 il::verify()

# 4. 送回 xdec 核心管线优化、结构化、发射 C
out = xdec.decompile_from_il(
    v_fn,
    abi=abi,
    memory=mem,
    regions=[
        xdec.MemoryRegion("ximg_rodata", xdec.RegionKind.ImmutableData, fixed_base=0x10000290, size=0x240),
        xdec.MemoryRegion("vm_stack",    xdec.RegionKind.PrivateStack,  base_reg=regs[125],    size=0x80000),
    ],
    names={0x10004000: "cs"},
)
print(out.c_text)

# 5. IL 解释执行与 Bit-Exact 校验
interp = xdec.il.Interpreter(out.il, memory=mem)
ret = interp.call(args=[md5_buf_va, 32, out_u32_va, out_len_va], stack_top=0x2007ff00)
```

---

## 5. 端到端实战蓝图：用 Python + `pyxdec` 去虚拟化 RQT

下面展示用户期望的完整端到端开发体验——**不手写庞大模拟器，从解释器 AST 自动识别 handler 模式，提升为 `xdec` IL，直接反编译出 C 算法并自动验证**。

### Step 1：解包载荷并构建叠加内存视图

```python
import struct, zlib, xdec

img = xdec.Image.open("target_binary")

# 方式 A: 直接读镜像解包 (或方式 B: 用 xdec.ExecSession 跑初始化函数自动拿解密脏页)
raw = img.read_bytes(payload_va, payload_size)
step1 = bytes(b ^ (len(raw) % 255) for b in raw)
step2 = zlib.decompress(step1)
ximg_raw = bytes(b ^ (len(step2) % 255) for b in step2)

# 解析 XIMG ELF64 的 PT_LOAD / .rela.dyn / .rodata_v / .dynsym("cs")
ximg_mem, remap_table, cs_va, rodata_range = load_ximg_elf(ximg_raw, img_base=0x10000000)
op_fmt = struct.unpack("<56I", img.read_bytes(op_fmt_va, 56 * 4))

# 构造分层叠加内存（供后续 const-fold-memory 与 il.Interpreter 共同使用）
layered = xdec.LayeredMemory(img)
layered.add_overlay(0x10000000, ximg_mem, immutable=True)
```

### Step 2：从解释器 `vm_interp` 的 AST/IL 自动模式识别 47 个 Handler

```python
interp_res = img.decompile(interp_va, structured_exprs=True)

# 1) 在 AST 中自动定位最大的 Switch 分发语句 (即 opcode 分发器)
switch_stmt = max(
    interp_res.ast.find_all(xdec.ast.StmtKind.Switch),
    key=lambda s: len(s.cases),
)

# 2) 基于 AST + 关联 IL 表达式树的通用模式识别器
def classify_handler(case_stmt) -> dict:
    """
    分析一个 case 的副作用摘要:
      - 匹配写回 R[rd] 的赋值目标 (如 *(vm_ctx + rd * 8) = rhs)
      - 匹配 rhs 的根运算类型 (Binary/Unary/Load/Shift/Rotate) 及操作数来源 (rs1/rs2/imm)
      - 匹配内存写入 Store(addr_expr, val_expr, width)
      - 匹配 vPC 更新与条件分支 (If(cmp(R[rs1], R[rs2]), vPC = target))
      - 匹配函数调用 (R[127] = vPC + 4; vPC = target) 与停机
    """
    summary = xdec.analysis.summarize_case_effects(interp_res.il, case_stmt.il_blocks)

    # 示例 1: 标准二元 ALU 寄存器指令: R[rd] = op(R[rs1], R[rs2])
    if summary.writes_reg_slot("rd") and summary.rhs.is_binary():
        return {
            "kind": "alu_bin",
            "op": summary.rhs.expr_op,         # e.g. ExprOp.Xor / Add / Sub / Rotr / Shl ...
            "bits": summary.rhs.type.bits,     # 32 or 64
            "lhs": summary.operand_role(summary.rhs.lhs),  # "rs1"
            "rhs": summary.operand_role(summary.rhs.rhs),  # "rs2" or "imm"
        }

    # 示例 2: 访存 Load: R[rd] = ext(load<W>(R[rs1] + imm))
    if summary.writes_reg_slot("rd") and summary.rhs.is_load():
        return {
            "kind": "load",
            "width": summary.rhs.load_bits,
            "signed": summary.rhs.is_sext(),
            "base": summary.operand_role(summary.rhs.addr_base),
            "off": "imm",
        }

    # 示例 3: 访存 Store / 条件分支 / Call / Ret / ADRP ...
    if summary.writes_memory():
        return {"kind": "store", "width": summary.store_bits, ...}
    if summary.updates_vpc_conditionally():
        return {"kind": "cond_branch", "cmp_op": summary.cond_op, ...}
    if summary.writes_reg_const_index(127) and summary.updates_vpc():
        return {"kind": "call"}
    if summary.is_return_or_halt():
        return {"kind": "halt"}

    return {"kind": "unknown", "case": case_stmt}

handlers = {}
for case in switch_stmt.cases:
    for val in case.values:
        handlers[val] = classify_handler(case)

# 未自动识别的极少数特殊指令（如自定义自校验/ADRP）清晰打印，人工补充覆写即可
unknown = [op for op, h in handlers.items() if h["kind"] == "unknown"]
print(f"Auto-classified {len(handlers) - len(unknown)}/{len(handlers)} handlers, manual review: {unknown}")
```

### Step 3：将 `XIMG` 中的 `cs`（及子函数）字节码提升为标准 `xdec::il::Function`

```python
# 1) 构造 128×64-bit 虚拟寄存器文件与 ABI
rf = xdec.il.RegisterFile()
R = [rf.add(f"r{i}", 64, xdec.il.RegClass.General) for i in range(128)]
rf.set_class(R[125], xdec.il.RegClass.StackPointer)
rqt_abi = xdec.AbiSpec(
    name="rqt-vm-64",
    stack_pointer=R[125], link_register=R[127],
    int_arg_regs=R[0:8], int_ret_regs=[R[0]], callee_saved_regs=R[19:29],
)

def lift_rqt_function(entry_va: int, func_name: str):
    # 沿直接分支/条件分支递归发现虚拟函数内所有基本块 (类似标准 CFG 发现，遇 BL 记录 callee 不内联)
    cfg_blocks, callees = discover_vm_cfg(entry_va, layered, remap_table, op_fmt, handlers)

    b = xdec.il.FunctionBuilder(xdec.Arch.Custom64, rf, entry_va, name=func_name)
    il_blocks = {pc: b.create_block(va=pc) for pc in cfg_blocks}

    for pc, insns in cfg_blocks.items():
        b.set_insert_point(il_blocks[pc])
        for cur_pc, op, fmt, rd, rs1, rs2, imm in insns:
            b.set_current_va(cur_pc)      # 每条 IL 保留 vPC 溯源！
            h = handlers[op]
            if h["kind"] == "alu_bin":
                lhs = b.read_reg(R[rs1])
                rhs = b.const(64, imm) if h["rhs"] == "imm" else b.read_reg(R[rs2])
                res = b.binary(h["op"], lhs, rhs, bits=h["bits"])
                b.write_reg(R[rd], res)
            elif h["kind"] == "load":
                addr = b.add(b.read_reg(R[rs1]), b.const(64, imm))
                val = b.load(h["width"], addr, signed=h["signed"], out_bits=64)
                b.write_reg(R[rd], val)
            elif h["kind"] == "store":
                addr = b.add(b.read_reg(R[rs1]), b.const(64, imm))
                b.store(h["width"], addr, b.read_reg(R[rd]))
            elif h["kind"] == "cond_branch":
                cond = b.cmp(h["cmp_op"], b.read_reg(R[rs1]), b.read_reg(R[rs2]))
                b.cond_branch(cond, il_blocks[cur_pc + imm], il_blocks[cur_pc + 4])
            elif h["kind"] == "call":
                b.call(b.const(64, cur_pc + imm))
            elif h["kind"] == "ret":
                b.ret()
            # ... 其余 ADRP / B / MOV_IMM 同样各 2~3 行

    return b.finalize(maturity=xdec.il.Maturity.Cfg), callees
```

### Step 4：回流 `xdec` 核心管线反编译 + 自动 Bit-Exact 差分验证

```python
# 1) 对 cs 及所有发现的子函数 (mod_sha256 / mod_md5 / roll133) 一键走 xdec 核心优化与发射
for fn_va, fn_name in discovered_functions.items():
    v_il, _ = lift_rqt_function(fn_va, fn_name)
    result = xdec.decompile_from_il(
        v_il,
        abi=rqt_abi,
        memory=layered,
        regions=[
            xdec.MemoryRegion("ximg_rodata", xdec.RegionKind.ImmutableData,
                              fixed_base=0x10000290, size=0x240),
            xdec.MemoryRegion("vm_stack", xdec.RegionKind.PrivateStack,
                              base_reg=R[125], size=0x80000),
        ],
        names=symbol_map,
    )
    Path(f"out/{fn_name}.c").write_text(result.c_text)

# 2) 用 xdec 内置 il.Interpreter 对照真机向量跑 100% Bit-Exact 校验
for vec in json.loads(Path("rqt_probe_vectors.json").read_text()):
    got = run_il_graph(optimized_il_map, entry="cs", memory=layered, md5_hex=vec["md5_hex"])
    assert got == vec["expected_rqt"], f"Mismatch: {got:#x} != {vec['expected_rqt']:#x}"
print("All vectors 100% BIT-EXACT verified against Optimized xdec IL!")
```

**对比过去手工还原的质变**：
- 过去：手写 400 行模拟器 + 人肉阅读 513 块解释器汇编 + 人工追踪 134,059 条动态指令反推 SHA-256 / MD5 魔改点；
- 现在：Python 几十行 AST 模式匹配识别 47 个 handler + 几十行提升到 `xdec::il` → **`xdec` 直接吐出 `mod_sha256`、`mod_md5`、`roll133` 和 `cs` 的可读 C 源码**，常量表、魔改移位量、填充字节 `0xfc`/`0xda`、反向累加全部一目了然，且自带 `il::Interpreter` Bit-Exact 校验。

---

## 6. 通用性验证：同一套库如何适配其他 VMP 家族

为了确认上述设计不是为 RQT 定制，对照 3 类完全不同的保护器：

| VMP 家族 | 与 RQT 的核心差异 | Python + `pyxdec` 适配方式（零 Core 修改） |
|---|---|---|
| **栈式滚动 Key VMP**（如商业壳/游戏保护） | 无 `R[0..127]`，全部通过 `vSP` 压栈弹栈；指令解码依赖前一条的 `rolling_key` | `RegisterFile` 只声明 `vSP`/`vPC`；解码时跟踪 `(vPC, key)`（或直接调 Core `devirt::Specializer`）；声明 `vSP` 为 `PrivateStack` → Core `RegionMem2Reg + RegionDSE` 自动把所有 push/pop 折叠成干净的 SSA 算式 |
| **直接线程化 / 无集中 Switch 的分散 Handler VM** | 没有单个 `switch(opcode)`，每个 handler 末尾直接 `br next_handler` | 用 `xdec.analysis.find_dispatch_regions(il_fn)` 聚合所有共享同一跳表的分散 handler 块，再用同一个 `summarize_case_effects()` 对每个 handler 块做语义模式识别 |
| **重度 OLLVM CFF + 自解密混合保护**（如大型 iOS Mach-O 混淆函数） | 没有独立字节码，逻辑本身被打散在状态机与加密常量中 | Python 调 `ExecSession` 或挂载 `Tracedb` 预热解密内存，用 `devirt::Specializer` 以 `StateKey = (state_var)` 自动展开去平坦化 → 送入 `decompile_from_il()` 与 `DiffVerifier` |

---

## 7. 分阶段落地路线图（M1 ~ M5）

严格遵守 `00-core-vs-plugin-prompt.md` 的回归分层纪律：**每个里程碑先用不依赖任何真实二进制的合成 L0 单元测试验收，再以 RQT / 真实样本作 L1/L2 端到端验证。**

| 里程碑 | Core / Library 交付物 | L0 合成单元测试门禁 | 真实样本验收目标 |
|---|---|---|---|
| **M1 · 底层 IR 与 Dual-View AST** | ① 浮点/向量寄存器 (`v/d/q`) 接入 SSA（消灭 `/*fcmp.uno?*/0x0`）；<br>② `HlExpr` 全结构化模式 + `HlStmt/HlExpr` 关联 `il::OpId`/`il::ExprId`；<br>③ 通用 `summarize_case_effects()` 副作用摘要分析。 | 合成浮点比较 + NEON 算术测试；合成 10-case `switch` 解释器，验证从 AST/IL 100% 还原每条 case 的算子与操作数角色。 | 真实浮点函数正常反编译；对 `vm_interp` 的主分发 `switch` / 状态机能结构化遍历全部 case。 |
| **M2 · 架构无关中段管线 (`decompileFromIL`)** | ① 抽象 `analysis::AbiSpec`（将 `vars`/`stack_frame`/`call_abi` 从 AArch64 硬编码解耦）；<br>② `il::FunctionBuilder` + `decompile::decompileFromIL()`；<br>③ `LayeredByteReader` + `MemoryRegionMap`。 | 用纯 C++ 构造合成 16 寄存器虚拟 ISA 函数（含循环、条件分支、栈数组、`.rodata` 查表），走 `decompileFromIL` 产出等价可编译 C。 | C++ 原型验证能把手工 lift 的子函数直接反编译为标准 C。 |
| **M3 · Python 绑定 (`pyxdec`) + 虚拟状态消除** | ① 封装 `pyxdec`（Image / LayeredMemory / Decompile Dual-View / RegisterFile / FunctionBuilder / decompile_from_il / Interpreter）；<br>② `passes::RegionMem2Reg` + `passes::RegionDSE`。 | Python pytest 套件：在 Python 里构造栈式与寄存器式微型 VM，端到端自动识别 handler → lift → 优化 → `il.Interpreter` 校验。 | **RQT 端到端闭环**：用 Python 脚本基于 `pyxdec` 提升 `XIMG` 字节码，输出 `cs` 及辅助函数可读 C，**`Interpreter` Bit-Exact 通过**。 |
| **M4 · 通用部分求值展开器 (`devirt::Specializer`)** | ① `(native_pc, StateKey)` 上下文敏感特化器（支持 Python/C++ 策略回调）；<br>② `ExecSession` + `Tracedb` 统一 `RuntimeOracle` 暴露给 Python 与 `pass::Context`；<br>③ `IDiscoveryHook`。 | 合成 OLLVM CFF 样本（嵌套循环+多分支）与合成滚动 key 字节码解释器，零手写 opcode 解码器展开还原。 | Python 驱动 `Specializer` 对重度 OLLVM 样本消除平坦化状态机。 |
| **M5 · 差分验证闭环与插件下沉** | ① `devirt::DiffVerifier`（原版 `ExecSession` / `Tracedb` vs `il::Interpreter` 自动化 bit-exact 校验）；<br>② 可经 oracle 校验的 `AlgebraRuleRegistry`；<br>③ `XdecPluginHost` v3 对齐库接口，允许成熟脚本下沉为原生插件并接入 `serve --mcp`。 | 对 M2~M4 所有合成样本跑自动化差分校验，并验证故意注入错误 handler/MBA 规则时必须报错。 | 形成完整的「Python 快速探索反 VMP → 自动化 Bit-Exact 验证 → 按需下沉插件/MCP」工程闭环。 |

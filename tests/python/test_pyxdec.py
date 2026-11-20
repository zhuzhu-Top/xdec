"""L0 pytest suite for pyxdec: Dual-View AST handler discovery, custom ISA lifting,
RegionMem2Reg/RegionDSE optimization, decompile_from_il, and bit-exact IL execution.
"""

from pathlib import Path
import struct
import sys

import pytest

# Allow importing compiled xdec.*.pyd from build/gcc-release/bin directly.
_REPO_ROOT = Path(__file__).resolve().parents[2]
_BIN_DIR = _REPO_ROOT / "build" / "gcc-release" / "bin"
if _BIN_DIR.exists() and str(_BIN_DIR) not in sys.path:
    sys.path.insert(0, str(_BIN_DIR))

import xdec  # noqa: E402


def _build_register_vm_interpreter() -> tuple[xdec.DecompileResult, xdec.LayeredMemory]:
    """Builds a synthetic 8-case register-based VM interpreter in xdec IL and
    decompiles it with structured_exprs=True so Python can auto-classify handlers.
    """
    rf = xdec.il.RegisterFile()
    x = [rf.add(f"x{i}", 64, xdec.il.RegClass.General) for i in range(31)]
    sp = rf.add("sp", 64, xdec.il.RegClass.StackPointer)
    abi = xdec.AbiSpec(
        name="aapcs64-mini",
        stack_pointer=sp,
        int_arg_regs=x[0:8],
        int_ret_regs=[x[0]],
    )

    b = xdec.il.FunctionBuilder(xdec.Arch.AArch64, rf, 0x1000, "vm_interp")
    entry = b.create_block(0x1000)
    jt = b.create_block(0x1020)
    default_blk = b.create_block(0x1f00)
    cases = [b.create_block(0x2000 + i * 0x100) for i in range(8)]

    # Jump table at 0x9000 in LayeredMemory.
    mem = xdec.LayeredMemory()
    jt_bytes = b"".join(struct.pack("<Q", 0x2000 + i * 0x100) for i in range(8))
    mem.add_overlay(0x9000, jt_bytes, immutable=True)

    b.set_insert_point(entry)
    b.set_current_va(0x1000)
    insn_entry = b.read_reg(x[1])
    op_entry = b.and_(b.lshr(insn_entry, b.i64(26)), b.i64(0x3f))
    b.cond_branch(b.cmp(xdec.il.ExprOp.CmpLtU, op_entry, b.i64(8)), jt, default_blk)

    b.set_insert_point(jt)
    b.set_current_va(0x1020)
    insn_jt = b.read_reg(x[1])
    op_jt = b.and_(b.lshr(insn_jt, b.i64(26)), b.i64(0x3f))
    target_expr = b.load(64, b.add(b.i64(0x9000), b.shl(op_jt, b.i64(3))))
    b.indirect_branch(target_expr, cases)

    def fields(blk: xdec.il.BlockId, va: int):
        b.set_insert_point(blk)
        b.set_current_va(va)
        ctx = b.read_reg(x[0])
        insn = b.read_reg(x[1])
        rd = b.and_(b.lshr(insn, b.i64(19)), b.i64(0x7f))
        rs1 = b.and_(b.lshr(insn, b.i64(12)), b.i64(0x7f))
        rs2 = b.and_(b.lshr(insn, b.i64(5)), b.i64(0x7f))
        imm = b.and_(insn, b.i64(0xfff))
        return ctx, rd, rs1, rs2, imm

    def slot(ctx, ridx):
        return b.add(ctx, b.shl(ridx, b.i64(3)))

    # Cases 0..3: XOR, ADD, SUB, ROTR
    for idx, expr_op in enumerate(
        [xdec.il.ExprOp.Xor, xdec.il.ExprOp.Add, xdec.il.ExprOp.Sub, xdec.il.ExprOp.RotR]
    ):
        va = 0x2000 + idx * 0x100
        ctx, rd, rs1, rs2, _ = fields(cases[idx], va)
        lhs = b.load(64, slot(ctx, rs1))
        rhs = b.load(64, slot(ctx, rs2))
        b.store(64, slot(ctx, rd), b.binary(expr_op, lhs, rhs))
        b.ret()

    # Case 4: ADD_IMM
    ctx, rd, rs1, _, imm = fields(cases[4], 0x2400)
    b.store(64, slot(ctx, rd), b.add(b.load(64, slot(ctx, rs1)), imm))
    b.ret()

    # Case 5: LD32 zero-extended
    ctx, rd, rs1, _, imm = fields(cases[5], 0x2500)
    addr = b.add(b.load(64, slot(ctx, rs1)), imm)
    b.store(64, slot(ctx, rd), b.zext(64, b.load(32, addr)))
    b.ret()

    # Case 6: ST32
    ctx, rd, rs1, _, imm = fields(cases[6], 0x2600)
    addr = b.add(b.load(64, slot(ctx, rs1)), imm)
    b.store(32, addr, b.trunc(32, b.load(64, slot(ctx, rd))))
    b.ret()

    # Case 7: HALT / return R[0]
    ctx, _, _, _, _ = fields(cases[7], 0x2700)
    b.write_reg(x[0], b.load(64, ctx))
    b.ret()

    b.set_insert_point(default_blk)
    b.set_current_va(0x1f00)
    b.ret()

    fn = b.finalize(xdec.il.Maturity.Cfg)
    res = xdec.decompile_from_il(
        fn, abi=abi, memory=mem, structured_exprs=True, capture_ast=True
    )
    return res, mem


def test_register_vm_ast_discovery_lift_and_bit_exact():
    interp_res, _ = _build_register_vm_interpreter()
    switches = interp_res.ast.find_all(xdec.ast.StmtKind.Switch)
    assert len(switches) == 1
    sw = switches[0]

    # Auto-classify every handler from the Dual-View AST + summarize_case_effects!
    handlers = {}
    for c in sw.cases:
        if c.is_default:
            continue
        summary = xdec.analysis.summarize_case_effects(interp_res.il, c.il_blocks)
        if summary.stores:
            st = summary.stores[0]
            if st.target.scale == 8:
                if st.value.is_memory_load:
                    handlers[c.value] = ("load", st.value.load_bits)
                else:
                    handlers[c.value] = ("alu", st.value.root_op)
            else:
                handlers[c.value] = ("store", st.width_bits)
        else:
            handlers[c.value] = ("halt",)

    assert handlers[0] == ("alu", xdec.il.ExprOp.Xor)
    assert handlers[1] == ("alu", xdec.il.ExprOp.Add)
    assert handlers[2] == ("alu", xdec.il.ExprOp.Sub)
    assert handlers[3] == ("alu", xdec.il.ExprOp.RotR)
    assert handlers[4] == ("alu", xdec.il.ExprOp.Add)
    assert handlers[5] == ("load", 32)
    assert handlers[6] == ("store", 32)
    assert handlers[7] == ("halt",)

    # Lift a small VM program using the auto-classified handlers into Custom64 IL:
    #   r2 = *(uint32_t*)(0x10000100 + 0)   (rodata seed = 0x11223344)
    #   r3 = r0 ^ r1
    #   r0 = rotr64(r3 + r2, 7)
    #   halt
    rf = xdec.il.RegisterFile()
    R = [rf.add(f"r{i}", 64, xdec.il.RegClass.General) for i in range(16)]
    sp = rf.add("vsp", 64, xdec.il.RegClass.StackPointer)
    abi = xdec.AbiSpec(
        name="vm16",
        stack_pointer=sp,
        int_arg_regs=R[0:4],
        int_ret_regs=[R[0]],
    )

    prog_mem = xdec.LayeredMemory()
    prog_mem.add_overlay(0x10000100, struct.pack("<I", 0x11223344), immutable=True)

    pb = xdec.il.FunctionBuilder(xdec.Arch.Custom64, rf, 0x10004000, "vm_prog")
    blk = pb.create_block(0x10004000)
    pb.set_insert_point(blk)
    pb.set_current_va(0x10004000)

    # Emit according to discovered handler semantics:
    pb.write_reg(R[2], pb.zext(64, pb.load(handlers[5][1], pb.i64(0x10000100))))
    pb.write_reg(R[3], pb.binary(handlers[0][1], pb.read_reg(R[0]), pb.read_reg(R[1])))
    pb.write_reg(R[4], pb.binary(handlers[1][1], pb.read_reg(R[3]), pb.read_reg(R[2])))
    pb.write_reg(R[0], pb.binary(handlers[3][1], pb.read_reg(R[4]), pb.i64(7)))
    pb.ret()

    v_fn = pb.finalize(xdec.il.Maturity.Cfg)
    out = xdec.decompile_from_il(v_fn, abi=abi, memory=prog_mem)

    # Constant load from 0x10000100 must fold to 0x11223344, and rotr64 appears in C!
    assert "0x11223344" in out.c_text
    assert "rotr64" in out.c_text

    # Bit-exact verification via xdec.il.Interpreter on the optimized Vars IL!
    interp = xdec.il.Interpreter(out.il, memory=prog_mem)
    for a0, a1 in [(0x123456789abcdef0, 0x0fedcba987654321), (0, 0), (0xffffffff, 0x55aa55aa)]:
        got = interp.call(args=[a0, a1], abi=abi)
        val = ((a0 ^ a1) + 0x11223344) & 0xFFFFFFFFFFFFFFFF
        expected = ((val >> 7) | (val << (64 - 7))) & 0xFFFFFFFFFFFFFFFF
        assert got == expected


def test_stack_vm_region_mem2reg_and_dse_bit_exact():
    """Constructs a stack-machine VM fragment using PrivateStack pushes/pops +
    an immutable .rodata constant overlay, runs RegionMem2Reg + RegionDSE via
    decompile_from_il, and verifies all stack push/pop ops vanish while execution
    remains 100% bit-exact.
    """
    rf = xdec.il.RegisterFile()
    vsp = rf.add("vsp", 64, xdec.il.RegClass.StackPointer)
    arg = rf.add("v0", 64, xdec.il.RegClass.General)
    ret = rf.add("vret", 64, xdec.il.RegClass.General)

    abi = xdec.AbiSpec(
        name="stack-vm",
        stack_pointer=vsp,
        int_arg_regs=[arg],
        int_ret_regs=[ret],
    )

    mem = xdec.LayeredMemory()
    mem.add_overlay(0x30000, struct.pack("<Q", 0xCAFEBABE), immutable=True)

    b = xdec.il.FunctionBuilder(xdec.Arch.Custom64, rf, 0x4000, "stack_vm_fn")
    entry = b.create_block(0x4000)
    b.set_insert_point(entry)

    # Dead stack write + push arg + push rodata const -> pop both -> add -> ret
    b.store(64, b.sub(b.read_reg(vsp), b.i64(24)), b.i64(0xdeadbeef))
    b.store(64, b.sub(b.read_reg(vsp), b.i64(8)), b.read_reg(arg))
    b.store(64, b.sub(b.read_reg(vsp), b.i64(16)), b.load(64, b.i64(0x30000)))
    lhs = b.load(64, b.sub(b.read_reg(vsp), b.i64(8)))
    rhs = b.load(64, b.sub(b.read_reg(vsp), b.i64(16)))
    b.write_reg(ret, b.xor_(lhs, rhs))
    b.ret()

    fn = b.finalize(xdec.il.Maturity.Cfg)
    regions = [
        xdec.MemoryRegion("vm_rodata", xdec.RegionKind.ImmutableData, fixed_base=0x30000, size=8),
        xdec.MemoryRegion("vm_stack", xdec.RegionKind.PrivateStack, base_reg=vsp, size=0x1000),
    ]
    out = xdec.decompile_from_il(fn, abi=abi, memory=mem, regions=regions)

    # All private stack Loads/Stores & immutable rodata Load must be completely eliminated!
    for bid in out.il.blocks:
        for oid in out.il.block(bid).ops:
            code = out.il.op(oid).code
            assert code not in (xdec.il.OpCode.Load, xdec.il.OpCode.Store)
    assert "0xcafebabe" in out.c_text.lower()
    assert "deadbeef" not in out.c_text.lower()

    interp = xdec.il.Interpreter(out.il, memory=mem)
    assert interp.call(args=[0x12345678], stack_top=0x800000, abi=abi) == (
        0x12345678 ^ 0xCAFEBABE
    )


def test_specializer_cff_and_diff_verify_python():
    """Tests M4/M5 Python API: xdec.specialize + xdec.diff_verify on a CFF function."""
    rf = xdec.il.RegisterFile()
    x0 = rf.add("x0", 64, xdec.il.RegClass.General)
    st = rf.add("state", 64, xdec.il.RegClass.General)
    abi = xdec.AbiSpec(name="cff-py", int_arg_regs=[x0], int_ret_regs=[x0])

    b = xdec.il.FunctionBuilder(xdec.Arch.Custom64, rf, 0x1000, "cff_py")
    entry = b.create_block(0x1000)
    disp = b.create_block(0x1020)
    s1 = b.create_block(0x1040)
    s2 = b.create_block(0x1060)

    b.set_insert_point(entry)
    b.write_reg(st, b.i64(1))
    b.branch(disp)

    b.set_insert_point(disp)
    b.cond_branch(b.cmp(xdec.il.ExprOp.CmpEq, b.read_reg(st), b.i64(1)), s1, s2)

    b.set_insert_point(s1)
    b.write_reg(x0, b.xor_(b.read_reg(x0), b.i64(0x42)))
    b.write_reg(st, b.i64(2))
    b.branch(disp)

    b.set_insert_point(s2)
    b.write_reg(x0, b.add(b.read_reg(x0), b.i64(9)))
    b.ret()

    raw_fn = b.finalize(xdec.il.Maturity.Cfg)
    spec_fn = xdec.specialize(
        raw_fn,
        state_key_fn=lambda pc, state: f"{pc:#x}:{state.get(st)}",
    )
    vecs = [xdec.DiffVector(args=[v], expected_return=((v ^ 0x42) + 9)) for v in (0, 7, 0x1234)]
    rep = xdec.diff_verify(raw_fn, spec_fn, vecs, abi=abi)
    assert rep.matched
    assert rep.vectors_checked == 3

    out = xdec.decompile_from_il(spec_fn, abi=abi)
    assert "0x42" in out.c_text
    assert "0x9" in out.c_text
    assert xdec.diff_verify(raw_fn, out.il, vecs, abi=abi).matched


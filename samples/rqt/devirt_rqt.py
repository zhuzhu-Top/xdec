"""End-to-end 128-register VM devirtualization demo using pyxdec."""

from __future__ import annotations

import json
from pathlib import Path
import struct
import sys

REPO_ROOT = Path(__file__).resolve().parents[2]
BIN_DIR = REPO_ROOT / "build" / "gcc-release" / "bin"
if BIN_DIR.exists() and str(BIN_DIR) not in sys.path:
    sys.path.insert(0, str(BIN_DIR))

import xdec  # noqa: E402

IMG_BASE = 0x10000000
SAMPLE_DIR = Path(__file__).resolve().parent


def sext(val: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return val - (1 << bits) if (val & sign) else val


def load_vm_payload():
    ximg = (SAMPLE_DIR / "vm_payload.bin").read_bytes()
    assert ximg[:4] == b"XIMG", f"Unexpected magic: {ximg[:4]!r}"

    fmt_data = json.loads((SAMPLE_DIR / "vm_fmt.json").read_text(encoding="utf-8"))
    op_fmt = tuple(fmt_data["op_fmt"])

    rodata_v = ximg[0x4D0:0x5D0]
    remap = {
        enc: dec
        for enc, dec in (struct.unpack_from("<BB", rodata_v, i * 2) for i in range(54))
    }

    # Parse ELF64 PT_LOAD & .rela.dyn
    e_phoff, _e_shoff = struct.unpack_from("<QQ", ximg, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", ximg, 0x36)
    mem_size = 0x20000
    flat = bytearray(mem_size)
    for i in range(e_phnum):
        p_type, _flags, p_off, p_vaddr, _paddr, p_filesz, _p_memsz, _align = (
            struct.unpack_from("<IIQQQQQQ", ximg, e_phoff + i * e_phentsize)
        )
        if p_type == 1:
            flat[p_vaddr : p_vaddr + p_filesz] = ximg[p_off : p_off + p_filesz]

    # Apply .rela.dyn relocations (offset 0x200, size 0x90)
    for i in range(0x90 // 24):
        r_off, _r_info, r_addend = struct.unpack_from("<QQq", ximg, 0x200 + i * 24)
        struct.pack_into("<Q", flat, r_off, IMG_BASE + r_addend)

    # .dynsym entry 1 ("cs")
    _st_name, _info, _other, _shndx, cs_off, _sz = struct.unpack_from(
        "<IBBHQQ", ximg, 0x1C8 + 24
    )

    layered = xdec.LayeredMemory()
    layered.add_overlay(IMG_BASE, bytes(flat[:0x18000]), immutable=True)
    layered.add_overlay(IMG_BASE + 0x18000, bytes(flat[0x18000:0x1C000]), immutable=False)
    layered.add_overlay(IMG_BASE + 0x1C000, bytes(flat[0x1C000:0x1D000]), immutable=True)

    return bytes(flat), layered, op_fmt, remap, IMG_BASE + cs_off


def decode_insn(flat: bytes, op_fmt, remap, pc: int):
    off = pc - IMG_BASE
    w = struct.unpack_from("<I", flat, off)[0]
    enc_op = ((w >> 26) & 0x3F) ^ ((pc >> 8) & 0x3F)
    op = remap[enc_op]
    fmt = op_fmt[op]
    rd = ((w >> 19) & 0x7F) ^ ((pc >> 2) & 0x7F) if fmt in (1, 2, 3) else 0
    rs1 = (((w >> 12) ^ (w >> 19)) & 0x7F) if fmt in (1, 2) else 0
    rs2 = (((w >> 5) ^ (w >> 12)) & 0x7F) if fmt == 1 else 0
    if fmt == 2:
        imm = sext(w & 0xFFF, 12)
        imm_u = w & 0xFFF
    elif fmt == 3:
        imm = sext(w & 0x7FFFF, 19)
        imm_u = w & 0x7FFFF
    elif fmt == 4:
        imm = sext(w & 0x3FFFFFF, 26)
        imm_u = w & 0x3FFFFFF
    else:
        imm = 0
        imm_u = w & 0x1F
    return op, fmt, rd, rs1, rs2, imm, imm_u


def resolve_got_slot_in_block(
    flat: bytes, block_insns: list[tuple], call_idx: int, target_reg: int
) -> int | None:
    """Resolve `ADRP rd, pg; ADD_LO14 rd, lo; LD64 rd, [rd]` earlier in the block."""
    page = None
    addr = None
    for idx in range(call_idx):
        ipc, op, _fmt, rd, rs1, _rs2, imm, imm_u = block_insns[idx]
        if rd != target_reg:
            continue
        if op == 44:  # ADRP
            page = (ipc & ~0x3FFF) + (imm << 14)
            addr = None
        elif op == 47 and page is not None:  # ADD_LO14
            addr = page + (imm_u & 0x3FFF)
        elif op == 38 and rs1 == target_reg and addr is not None:  # LD64 [rd]
            if IMG_BASE <= addr < IMG_BASE + len(flat) - 8:
                return struct.unpack_from("<Q", flat, addr - IMG_BASE)[0]
        elif op in (3, 14, 17, 18, 52, 53):
            page = None
            addr = None
    return None


def discover_vm_cfg(flat: bytes, op_fmt, remap, entry_va: int):
    """Discover basic blocks inside one VM function starting at `entry_va`."""
    work = [entry_va]
    block_starts = {entry_va}
    blocks: dict[int, list[tuple]] = {}

    while work:
        start = work.pop()
        if start in blocks:
            continue
        insns = []
        pc = start
        while True:
            dec = decode_insn(flat, op_fmt, remap, pc)
            op, _fmt, _rd, _rs1, _rs2, imm, _imm_u = dec
            insns.append((pc, *dec))
            if op == 2:  # RET
                break
            if op == 27:  # B
                tgt = pc + imm
                if tgt not in block_starts:
                    block_starts.add(tgt)
                    work.append(tgt)
                break
            if op == 42:  # CB_EQ1
                tgt = pc + imm
                fall = pc + 4
                for t in (tgt, fall):
                    if t not in block_starts:
                        block_starts.add(t)
                        work.append(t)
                break
            pc += 4
            if pc in block_starts:
                break
        blocks[start] = insns

    final_blocks: dict[int, list[tuple]] = {}
    for start, insns in blocks.items():
        cur_start = start
        cur_list = []
        for idx, ins in enumerate(insns):
            ipc = ins[0]
            if idx > 0 and ipc in block_starts:
                final_blocks[cur_start] = cur_list
                cur_start = ipc
                cur_list = []
            cur_list.append(ins)
        final_blocks[cur_start] = cur_list
    return final_blocks


def lift_vm_function(
    flat: bytes, op_fmt, remap, rf: xdec.il.RegisterFile, R: list, entry_va: int, name: str
):
    cfg_blocks = discover_vm_cfg(flat, op_fmt, remap, entry_va)
    b = xdec.il.FunctionBuilder(xdec.Arch.Custom64, rf, entry_va, name)
    il_blocks = {pc: b.create_block(pc) for pc in sorted(cfg_blocks.keys())}

    for pc in sorted(cfg_blocks.keys()):
        b.set_insert_point(il_blocks[pc])
        insns = cfg_blocks[pc]
        terminated = False
        for idx, (ipc, op, _fmt, rd, rs1, rs2, imm, imm_u) in enumerate(insns):
            b.set_current_va(ipc)
            if op == 3:  # ADD_IMM
                b.write_reg(R[rd], b.add(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)))
            elif op == 53:  # ADD_REG
                b.write_reg(R[rd], b.add(b.read_reg(R[rs1]), b.read_reg(R[rs2])))
            elif op == 17:  # SUB_IMM
                b.write_reg(R[rd], b.sub(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)))
            elif op == 18:  # SUB_REG
                b.write_reg(R[rd], b.sub(b.read_reg(R[rs1]), b.read_reg(R[rs2])))
            elif op == 11:  # MUL_IMM
                b.write_reg(R[rd], b.mul(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)))
            elif op == 43:  # MUL_REG
                b.write_reg(R[rd], b.mul(b.read_reg(R[rs1]), b.read_reg(R[rs2])))
            elif op == 28:  # SDIV_REG
                b.write_reg(R[rd], b.binary(xdec.il.ExprOp.DivS, b.read_reg(R[rs1]), b.read_reg(R[rs2])))
            elif op == 50:  # UDIV_REG
                b.write_reg(R[rd], b.binary(xdec.il.ExprOp.DivU, b.read_reg(R[rs1]), b.read_reg(R[rs2])))
            elif op == 9:  # UREM_IMM
                b.write_reg(R[rd], b.binary(xdec.il.ExprOp.RemU, b.read_reg(R[rs1]), b.i64(imm_u)))
            elif op == 25:  # UREM_REG
                b.write_reg(R[rd], b.binary(xdec.il.ExprOp.RemU, b.read_reg(R[rs1]), b.read_reg(R[rs2])))
            elif op == 10:  # SREM_REG
                b.write_reg(R[rd], b.binary(xdec.il.ExprOp.RemS, b.read_reg(R[rs1]), b.read_reg(R[rs2])))
            elif op == 29:  # SREM_IMM
                b.write_reg(R[rd], b.binary(xdec.il.ExprOp.RemS, b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)))
            elif op == 6:  # AND_IMM
                b.write_reg(R[rd], b.and_(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)))
            elif op == 36:  # AND_REG
                b.write_reg(R[rd], b.and_(b.read_reg(R[rs1]), b.read_reg(R[rs2])))
            elif op == 49:  # OR_IMM
                b.write_reg(R[rd], b.or_(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)))
            elif op == 37:  # OR_REG
                b.write_reg(R[rd], b.or_(b.read_reg(R[rs1]), b.read_reg(R[rs2])))
            elif op == 15:  # XOR_IMM
                b.write_reg(R[rd], b.xor_(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)))
            elif op == 8:  # XOR_REG
                b.write_reg(R[rd], b.xor_(b.read_reg(R[rs1]), b.read_reg(R[rs2])))
            elif op == 31:  # SHL_IMM
                b.write_reg(R[rd], b.shl(b.read_reg(R[rs1]), b.i64(imm_u & 0x3F)))
            elif op == 35:  # SHL_REG
                b.write_reg(R[rd], b.shl(b.read_reg(R[rs1]), b.and_(b.read_reg(R[rs2]), b.i64(0x3F))))
            elif op == 24:  # LSR_IMM
                b.write_reg(R[rd], b.lshr(b.read_reg(R[rs1]), b.i64(imm_u & 0x3F)))
            elif op == 7:  # LSR_REG
                b.write_reg(R[rd], b.lshr(b.read_reg(R[rs1]), b.and_(b.read_reg(R[rs2]), b.i64(0x3F))))
            elif op == 16:  # ASR_IMM
                b.write_reg(R[rd], b.ashr(b.read_reg(R[rs1]), b.i64(imm_u & 0x3F)))
            elif op == 41:  # ASR_REG
                b.write_reg(R[rd], b.ashr(b.read_reg(R[rs1]), b.and_(b.read_reg(R[rs2]), b.i64(0x3F))))
            elif op == 52:  # MOV_REG
                b.write_reg(R[rd], b.read_reg(R[rs1]))
            elif op == 14:  # MOV_IMM (sext19)
                b.write_reg(R[rd], b.i64(imm & 0xFFFFFFFFFFFFFFFF))
            elif op == 30:  # MOVK19
                cur = b.read_reg(R[rd])
                b.write_reg(R[rd], b.or_(b.and_(cur, b.i64(0xFFFFFFFFFFF80000)), b.i64(imm_u & 0x7FFFF)))
            elif op == 44:  # ADRP
                b.write_reg(R[rd], b.i64(((ipc & ~0x3FFF) + (imm << 14)) & 0xFFFFFFFFFFFFFFFF))
            elif op == 47:  # ADD_LO14
                b.write_reg(R[rd], b.add(b.read_reg(R[rd]), b.i64(imm_u & 0x3FFF)))
            elif op == 45:  # CMOV_EQ1
                cond = b.cmp(xdec.il.ExprOp.CmpEq, b.read_reg(R[rs2]), b.i64(1))
                b.write_reg(R[rd], b.select(cond, b.read_reg(R[rs1]), b.read_reg(R[rd])))
            elif op == 33:  # LDS8
                b.write_reg(R[rd], b.sext(64, b.load(8, b.add(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)))))
            elif op == 46:  # LDS16
                b.write_reg(R[rd], b.sext(64, b.load(16, b.add(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)))))
            elif op == 22:  # LDS32
                b.write_reg(R[rd], b.sext(64, b.load(32, b.add(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)))))
            elif op == 38:  # LD64
                b.write_reg(R[rd], b.load(64, b.add(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF))))
            elif op == 48:  # ST8
                b.store(8, b.add(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)), b.trunc(8, b.read_reg(R[rd])))
            elif op == 23:  # ST16
                b.store(16, b.add(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)), b.trunc(16, b.read_reg(R[rd])))
            elif op == 40:  # ST32
                b.store(32, b.add(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)), b.trunc(32, b.read_reg(R[rd])))
            elif op == 13:  # ST64
                b.store(64, b.add(b.read_reg(R[rs1]), b.i64(imm & 0xFFFFFFFFFFFFFFFF)), b.read_reg(R[rd]))
            elif op in (4, 5, 12, 19, 20, 26, 34, 39, 54):
                a = b.read_reg(R[rs1])
                c = b.read_reg(R[rs2])
                if op == 26:
                    cond = b.cmp(xdec.il.ExprOp.CmpEq, a, c)
                elif op == 20:
                    cond = b.cmp(xdec.il.ExprOp.CmpNe, a, c)
                elif op == 39:
                    cond = b.cmp(xdec.il.ExprOp.CmpLtU, a, c)
                elif op == 34:
                    cond = b.cmp(xdec.il.ExprOp.CmpLeU, a, c)
                elif op == 19:
                    cond = b.cmp(xdec.il.ExprOp.CmpLtU, c, a)
                elif op == 4:
                    cond = b.cmp(xdec.il.ExprOp.CmpLeU, c, a)
                elif op == 54:
                    cond = b.cmp(xdec.il.ExprOp.CmpLeS, c, a)
                elif op == 5:
                    cond = b.cmp(xdec.il.ExprOp.CmpLtS, c, a)
                else:
                    cond = b.cmp(xdec.il.ExprOp.CmpLeS, a, c)
                b.write_reg(R[rd], b.zext(64, cond))
            elif op == 51:  # BLR
                resolved_target = resolve_got_slot_in_block(flat, insns, idx, rd)
                target_expr = b.i64(resolved_target) if resolved_target is not None else b.read_reg(R[rd])
                b.write_reg(R[127], b.i64(ipc + 4))
                b.write_reg(R[0], b.call(target_expr))
            elif op == 27:  # B
                b.branch(il_blocks[ipc + imm])
                terminated = True
                break
            elif op == 42:  # CB_EQ1
                cond = b.cmp(xdec.il.ExprOp.CmpEq, b.read_reg(R[rd]), b.i64(1))
                b.cond_branch(cond, il_blocks[ipc + imm], il_blocks[ipc + 4])
                terminated = True
                break
            elif op == 2:  # RET
                b.ret()
                terminated = True
                break
            else:
                raise NotImplementedError(f"Unhandled VM opcode {op} at {ipc:#x}")

        if not terminated:
            b.branch(il_blocks[insns[-1][0] + 4])

    return b.finalize(xdec.il.Maturity.Cfg), len(cfg_blocks)


def main():
    flat, layered, op_fmt, remap, cs_va = load_vm_payload()
    print(f"[1/4] Loaded anonymized VM payload (115,776 B), entry cs @ {cs_va:#x}")

    rf = xdec.il.RegisterFile()
    R = [
        rf.add(
            f"r{i}",
            64,
            xdec.il.RegClass.General if i != 125 else xdec.il.RegClass.StackPointer,
        )
        for i in range(128)
    ]
    abi = xdec.AbiSpec(
        name="vm-128-64",
        stack_pointer=R[125],
        link_register=R[127],
        int_arg_regs=R[0:8],
        int_ret_regs=[R[0]],
        callee_saved_regs=R[8:125] + [R[126], R[127]],
        caller_saved_regs=R[0:8],
    )

    out_dir = SAMPLE_DIR / "output"
    out_dir.mkdir(parents=True, exist_ok=True)

    names = {
        0x10004000: "cs",
        0x10016520: "vm_init_array",
        0x10016588: "vm_memcpy",
        0x10016A2C: "vm_memset",
    }

    print("[2/4] Lifting VM functions into xdec::il::Function ...")
    raw_fns = {}
    decompiled = {}
    for va, fn_name in names.items():
        fn_cfg, n_blks = lift_vm_function(flat, op_fmt, remap, rf, R, va, fn_name)
        raw_fn_copy, _ = lift_vm_function(flat, op_fmt, remap, rf, R, va, fn_name)
        raw_fns[va] = raw_fn_copy
        print(f"  lifted {fn_name} @ {va:#x}: {n_blks} basic blocks")

        print(f"[3/4] Decompiling {fn_name} via xdec.decompile_from_il ...")
        res = xdec.decompile_from_il(
            fn_cfg,
            abi=abi,
            memory=layered,
            names=names,
            structured_exprs=False,
            capture_ast=False,
        )
        decompiled[va] = res
        out_path = out_dir / f"{fn_name}.c"
        out_path.write_text(res.c_text, encoding="utf-8")
        print(
            f"  wrote {out_path} ({len(res.c_text.splitlines())} lines, "
            f"labeled={res.labeled_blocks}, vars_blocks={len(res.il.blocks)})"
        )

    print("[4/4] Executing cs via xdec.il.Interpreter (Bit-Exact check) ...")
    input_bytes = b"a4ca8e3beece0a45f3545b0421b10a4a"
    expected_out = 0x44704227

    for label, fn_map in [
        ("Cfg (raw lifted)", raw_fns),
        ("Vars (xdec optimized)", {k: v.il for k, v in decompiled.items()}),
    ]:
        interp = xdec.il.Interpreter(fn_map[0x10004000], memory=layered)
        in_va = 0x20000000
        out_u32_va = 0x20000100
        out_len_va = 0x20000108
        stack_top = 0x20080000
        interp.memory.seed(0x20000000, b"\x00" * 0x100)
        interp.memory.seed(in_va, input_bytes)
        interp.memory.seed(out_u32_va, b"\x00" * 8)
        interp.memory.seed(out_len_va, struct.pack("<Q", 4))
        interp.memory.seed(stack_top - 0x20000, b"\x00" * 0x24000)

        subs = {va: fn for va, fn in fn_map.items() if va != 0x10004000}
        interp_init = xdec.il.Interpreter(fn_map[0x10016520], exec_memory=interp.memory)
        interp_init.call(args=[], stack_top=stack_top - 0x4000, abi=abi)

        interp.call(
            args=[in_va, 32, out_u32_va, out_len_va],
            stack_top=stack_top,
            abi=abi,
            subfunctions=subs,
            max_steps=5_000_000,
        )
        got_u32 = interp.memory.read(out_u32_va, 4)
        got_len = interp.memory.read(out_len_va, 4)
        print(f"  {label}: out_u32={got_u32:#010x} (expected={expected_out:#010x}), out_len={got_len}")
        assert got_u32 == expected_out, f"Mismatch on {label}: {got_u32:#x} != {expected_out:#x}"

    print("SUCCESS: Self-contained VM devirtualization & Bit-Exact IL execution PASSED!")


if __name__ == "__main__":
    main()

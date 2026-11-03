"""Audit ARM64 instructions claimed only by xdec catch-all rules.

The ordinary coverage command counts deliberate ``simd_fp_unmodelled`` and
``memory_unmodelled`` rules as decoded. This tool identifies their concrete
opcodes in an executable, disassembles them with Capstone, and ranks the
missing instruction families by static occurrence count.

Reads ELF and 64-bit Mach-O. Only sections the linker marked as holding
instructions are scanned, but a literal pool inside one is indistinguishable
from code at this level, so a family Capstone names implausibly is usually
constants rather than a real gap.
"""

from __future__ import annotations

import argparse
import collections
import os
import struct
import subprocess
from pathlib import Path

import capstone

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
# S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS
MACHO_INSTRUCTION_ATTRS = 0x80000000 | 0x00000400


def _elf_sections(path: Path) -> list[tuple[int, bytes]]:
    from elftools.elf.constants import SH_FLAGS
    from elftools.elf.elffile import ELFFile

    sections = []
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        for section in elf.iter_sections():
            if section["sh_flags"] & SH_FLAGS.SHF_EXECINSTR:
                sections.append((section["sh_addr"], section.data()))
    return sections


def _macho_sections(image: bytes) -> list[tuple[int, bytes]]:
    ncmds = struct.unpack_from("<I", image, 16)[0]
    sections = []
    cursor = 32
    for _ in range(ncmds):
        command, size = struct.unpack_from("<II", image, cursor)
        if command == LC_SEGMENT_64:
            count = struct.unpack_from("<I", image, cursor + 64)[0]
            entry = cursor + 72
            for _ in range(count):
                addr, length, offset = struct.unpack_from("<QQI", image, entry + 32)
                flags = struct.unpack_from("<I", image, entry + 64)[0]
                if flags & MACHO_INSTRUCTION_ATTRS:
                    sections.append((addr, image[offset : offset + length]))
                entry += 80
        cursor += size
    return sections


def executable_words(path: Path) -> dict[int, list[int]]:
    image = path.read_bytes()
    if struct.unpack_from("<I", image, 0)[0] == MH_MAGIC_64:
        sections = _macho_sections(image)
    else:
        sections = _elf_sections(path)

    locations: dict[int, list[int]] = collections.defaultdict(list)
    for base, data in sections:
        for offset in range(0, len(data) - 3, 4):
            word = struct.unpack_from("<I", data, offset)[0]
            locations[word].append(base + offset)
    return locations


def traced_pcs(root: Path, image_base: int) -> set[int]:
    """Reads the compact per-segment PC indexes without loading trace rows.

    xdec_trace stores module-relative PCs in pc.idx. Each index starts with an
    eight-byte header followed by 16-byte entries: key, posting offset, count.
    Only the key is needed for an instruction-coverage gate.
    """
    result: set[int] = set()
    for path in root.glob("threads/*/segments/*/pc.idx"):
        data = path.read_bytes()
        if len(data) < 8 or (len(data) - 8) % 16 != 0:
            raise ValueError(f"malformed tracedb PC index: {path}")
        for offset in range(8, len(data), 16):
            (pc,) = struct.unpack_from("<Q", data, offset)
            result.add(image_base + pc)
    if not result:
        raise ValueError(f"no PC indexes found under {root}")
    return result


def xdec_decode(
    words: list[int], xdec: Path, spec: Path
) -> dict[int, str]:
    environment = os.environ.copy()
    environment["XDEC_SPEC"] = str(spec)
    input_text = "".join(f"0x{word:08x}\n" for word in words)
    completed = subprocess.run(
        [str(xdec), "decode"],
        input=input_text,
        text=True,
        capture_output=True,
        check=True,
        env=environment,
    )
    decoded: dict[int, str] = {}
    for line in completed.stdout.splitlines():
        fields = line.strip().split(maxsplit=1)
        if len(fields) == 2:
            decoded[int(fields[0], 16)] = fields[1]
    return decoded


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--xdec", type=Path, required=True)
    parser.add_argument("--spec", type=Path, required=True)
    parser.add_argument("--details", action="store_true")
    parser.add_argument(
        "--tracedb",
        type=Path,
        help="only audit PCs executed in this xdec_trace engine directory",
    )
    parser.add_argument(
        "--image-base",
        type=lambda value: int(value, 0),
        default=0x100000000,
        help="preferred image base added to module-relative tracedb PCs",
    )
    arguments = parser.parse_args()

    locations = executable_words(arguments.binary)
    if arguments.tracedb:
        executed = traced_pcs(arguments.tracedb, arguments.image_base)
        locations = {
            word: [address for address in addresses if address in executed]
            for word, addresses in locations.items()
        }
        locations = {word: addresses for word, addresses in locations.items() if addresses}
    decoded = xdec_decode(sorted(locations), arguments.xdec, arguments.spec)
    disassembler = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

    families: collections.Counter[str] = collections.Counter()
    encodings: dict[str, list[tuple[int, int, int, str]]] = (
        collections.defaultdict(list)
    )
    for word, addresses in locations.items():
        xdec_text = decoded.get(word, "")
        if not (
            xdec_text.startswith("simd #")
            or xdec_text.startswith("ldst #")
        ):
            continue
        instruction = next(
            disassembler.disasm(struct.pack("<I", word), addresses[0], 1),
            None,
        )
        if instruction is None:
            mnemonic = "<capstone-undecodable>"
            assembly = mnemonic
        else:
            mnemonic = instruction.mnemonic
            assembly = f"{instruction.mnemonic} {instruction.op_str}".rstrip()
        count = len(addresses)
        families[mnemonic] += count
        encodings[mnemonic].append(
            (word, count, addresses[0], assembly)
        )

    total = sum(families.values())
    print(
        f"{arguments.binary}: {total} catch-all words, "
        f"{len(families)} instruction families"
    )
    for mnemonic, count in families.most_common():
        unique = len(encodings[mnemonic])
        print(
            f"{mnemonic:<12} {count:>5} words  {unique:>3} encodings  "
            f"{100.0 * count / total:6.2f}%"
        )
        if arguments.details:
            for word, occurrences, address, assembly in sorted(
                encodings[mnemonic], key=lambda item: (-item[1], item[0])
            ):
                print(
                    f"  va=0x{address:x} opcode=0x{word:08x} "
                    f"count={occurrences:<3} {assembly}"
                )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

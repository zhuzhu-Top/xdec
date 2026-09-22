"""Audit ARM64 instructions claimed only by xdec catch-all rules.

The ordinary coverage command counts deliberate ``simd_fp_unmodelled`` and
``memory_unmodelled`` rules as decoded. This tool identifies their concrete
opcodes in an ELF executable, disassembles them with Capstone, and ranks the
missing instruction families by static occurrence count.
"""

from __future__ import annotations

import argparse
import collections
import os
import struct
import subprocess
from pathlib import Path

import capstone
from elftools.elf.constants import SH_FLAGS
from elftools.elf.elffile import ELFFile


def executable_words(path: Path) -> dict[int, list[int]]:
    locations: dict[int, list[int]] = collections.defaultdict(list)
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        for section in elf.iter_sections():
            if not (section["sh_flags"] & SH_FLAGS.SHF_EXECINSTR):
                continue
            data = section.data()
            base = section["sh_addr"]
            for offset in range(0, len(data) - 3, 4):
                word = struct.unpack_from("<I", data, offset)[0]
                locations[word].append(base + offset)
    return locations


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
    arguments = parser.parse_args()

    locations = executable_words(arguments.binary)
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

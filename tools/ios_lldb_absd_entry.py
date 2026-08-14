#!/usr/bin/env python3
"""Dump absd entry regs at dyld BLR X20 (HW breakpoint, full image addresses).

Beyond the human-readable register dump this always printed, a successful
run also writes `--out` (default: `absd.entry.json` in the current
directory) -- the sidecar SessionContext::open auto-discovers next to a
local copy of absd (see docs/21-entry-reg-platform.md). No xdec CLI flag is
involved on either end: dropping the exported file and a `dyld` binary next
to your local `absd` is the whole handoff.
"""
import argparse
import json
import re
import sys
import paramiko

HOST = "192.168.110.36"
USER = "root"
PASSWORD = "alpine"
ABSD_PATH = "/usr/sbin/absd"
ENTRY_OFF = 0x23290
DYLD_BLR_OFF = 0x183CC
DYLD_X22_OFF = 0x68310
DYLD_X21_OFF = 0x54000


def ssh_run(client: paramiko.SSHClient, cmd: str, timeout: int = 50) -> str:
    _stdin, stdout, stderr = client.exec_command(cmd, timeout=timeout)
    stdout.channel.recv_exit_status()
    return stdout.read().decode("utf-8", "replace") + stderr.read().decode("utf-8", "replace")


def parse_load_addr(text: str, path: str) -> int | None:
    for ln in text.splitlines():
        if path in ln:
            addrs = re.findall(r"0x[0-9a-fA-F]+", ln)
            if addrs:
                return max(int(a, 16) for a in addrs)
    return None


def parse_regs(text: str) -> dict[str, int]:
    out: dict[str, int] = {}
    for ln in text.splitlines():
        m = re.match(r"\s*(x\d+|fp|lr|sp|pc)\s*=\s*(0x[0-9a-fA-F]+)", ln)
        if m:
            out[m.group(1)] = int(m.group(2), 16)
    return out


def write_sidecar(out_path: str, dyld_base: int, dyld_companion_path: str,
                   literals: dict[str, int]) -> None:
    """Writes the `<binary>.entry.json` sidecar (see analysis/entry_reg.h and
    docs/21-entry-reg-platform.md for the schema). `literals` overrides the
    platform profile's own formulas/defaults per register -- only include a
    register here when this run actually measured it; a profile default
    (x21/x22's dyld-offset formula) is better than a stale one-off number.
    """
    doc = {
        "literal": {name: hex(value) for name, value in literals.items()},
        "companions": [
            {"name": "dyld", "path": dyld_companion_path, "base": hex(dyld_base)},
        ],
    }
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)
    print(f"\nwrote sidecar: {out_path}\n{json.dumps(doc, indent=2)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="absd.entry.json",
                        help="sidecar path to write (default: absd.entry.json, next to a "
                             "local copy of absd)")
    parser.add_argument("--dyld-path", default="dyld",
                        help="companion path recorded in the sidecar (default: 'dyld', "
                             "resolved next to the binary xdec is decompiling -- see "
                             "SessionContext::buildEntryRegFacts)")
    args = parser.parse_args()

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username=USER, password=PASSWORD, timeout=10)

    ssh_run(client, "killall -9 lldb debugserver 2>/dev/null; launchctl kill SIGTERM system/com.apple.absd 2>/dev/null; true")

    probe = ssh_run(
        client,
        f"lldb -b -o 'target create {ABSD_PATH}' -o 'process launch -s' "
        "-o 'image list absd' -o 'image list dyld' -o quit 2>&1",
        timeout=40,
    )
    print("===== PROBE =====\n", probe)
    absd = parse_load_addr(probe, "/usr/sbin/absd")
    dyld = parse_load_addr(probe, "/usr/lib/dyld")
    if not absd or not dyld:
        print("FAIL: parse bases")
        return 1
    print(f"absd={absd:#x} dyld={dyld:#x} entry={absd + ENTRY_OFF:#x} blr={dyld + DYLD_BLR_OFF:#x}")

    for label, addr in [("dyld BLR X20", dyld + DYLD_BLR_OFF), ("absd LC_MAIN", absd + ENTRY_OFF)]:
        ssh_run(client, "killall -9 lldb debugserver 2>/dev/null; true")
        script = f"""
target create {ABSD_PATH}
settings set target.process.stop-on-sharedlibrary-events false
process launch -s
breakpoint set -H -a {addr}
process continue
register read x0 x1 x2 x3 x19 x20 x21 x22 x28 pc
disassemble --start-address $pc-8 --count 4
quit
"""
        ssh_run(client, "cat > /tmp/xdec_hw.lldb << 'EOF'\n" + script + "EOF")
        out = ssh_run(client, "lldb -b -s /tmp/xdec_hw.lldb 2>&1", timeout=50)
        print(f"\n===== {label} @ {addr:#x} =====\n", out)
        regs = parse_regs(out)
        if regs:
            print("\n========== REGISTER DUMP ==========")
            print(f"dyld+0x68310 = {dyld + DYLD_X22_OFF:#x}")
            print(f"dyld+0x54000 = {dyld + DYLD_X21_OFF:#x}")
            for k in ["pc", "x0", "x1", "x2", "x3", "x19", "x20", "x21", "x22", "x28"]:
                if k in regs:
                    print(f"  {k} = {regs[k]:#018x}")
            if regs.get("x22") == dyld + DYLD_X22_OFF:
                print("  ** x22 == dyld+0x68310 CONFIRMED **")
            if regs.get("x21") == dyld + DYLD_X21_OFF:
                print("  ** x21 == dyld+0x54000 CONFIRMED **")
            # x28 is kernel-launch residue, not a dyld-relative formula (see
            # binary::TargetProfile::entryRegLiterals's own "0" default), so
            # it is the one register worth overriding per capture; x21/x22
            # already match the profile's dyld formula above and are left
            # for it to compute, rather than pinned to this one run's dyld
            # slide.
            literals = {}
            if "x28" in regs:
                literals["x28"] = regs["x28"]
            write_sidecar(args.out, dyld, args.dyld_path, literals)
            client.close()
            return 0

    print("\nNo breakpoint hit — absd likely exits before entry under non-launchd lldb launch.")
    print("Run interactively on device (see docs/20-absd-entry-registers.md §7).")
    client.close()
    return 1


if __name__ == "__main__":
    sys.exit(main())

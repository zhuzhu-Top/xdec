#!/usr/bin/env python3
"""Probe iOS device via SSH and run lldb batch commands for absd entry regs."""
import sys
import paramiko

HOST = "192.168.110.36"
USER = "root"
PASSWORD = "alpine"


def run(client: paramiko.SSHClient, cmd: str, timeout: int = 60) -> tuple[int, str, str]:
    print(f"\n=== {cmd}")
    stdin, stdout, stderr = client.exec_command(cmd, timeout=timeout)
    code = stdout.channel.recv_exit_status()
    out = stdout.read().decode("utf-8", "replace")
    err = stderr.read().decode("utf-8", "replace")
    if out:
        print(out, end="" if out.endswith("\n") else "\n")
    if err.strip():
        print("ERR:", err)
    return code, out, err


def main() -> int:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username=USER, password=PASSWORD, timeout=15)

    probes = [
        "uname -a",
        "which lldb",
        "lldb --version 2>&1 | head -5",
        "ls -la /usr/libexec/absd 2>&1",
        "file /usr/libexec/absd 2>&1",
        "ps aux | grep -i absd | grep -v grep",
        "launchctl list 2>/dev/null | grep -i absd | head -10",
        "ls -la /usr/lib/dyld 2>&1",
    ]
    for cmd in probes:
        run(client, cmd)

    # Batch lldb: attach if running, else launch and break at entry offset.
    lldb_script = r"""
target create /usr/libexec/absd
image list -o -f
process launch -s
image list absd
"""
    run(
        client,
        "cat > /tmp/xdec_absd.lldb << 'EOF'\n" + lldb_script + "EOF",
    )
    run(client, "lldb -b -s /tmp/xdec_absd.lldb 2>&1 | head -80", timeout=120)

    client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

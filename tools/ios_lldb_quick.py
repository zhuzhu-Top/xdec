import paramiko

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect("192.168.110.36", username="root", password="alpine", timeout=8)

cmds = [
    "killall -9 lldb 2>/dev/null; killall -9 debugserver 2>/dev/null; true",
    "launchctl kill SIGTERM system/com.apple.absd 2>/dev/null; sleep 1",
    "ps aux | grep absd | grep -v grep",
]
for cmd in cmds:
    print("===", cmd)
    _, o, _ = c.exec_command(cmd, timeout=15)
    print(o.read().decode())

lldb = """
target create /usr/sbin/absd
settings set target.process.stop-on-sharedlibrary-events false
breakpoint set -a 0x100023290
process launch
register read x0 x1 x2 x3 x19 x20 x21 x22 x23 x24 x25 x26 x27 x28 pc
image list -o -f
disassemble --start-address $pc --count 4
quit
"""
c.exec_command("cat > /tmp/q.lldb << 'EOF'\n" + lldb + "EOF", timeout=10)
print("=== lldb run")
_, o, _ = c.exec_command("lldb -b -s /tmp/q.lldb 2>&1", timeout=45)
text = o.read().decode()
print(text)

# parse and verify
import re
regs = {}
for ln in text.splitlines():
    m = re.match(r"\s*(x\d+|pc)\s*=\s*(0x[0-9a-fA-F]+)", ln)
    if m:
        regs[m.group(1)] = int(m.group(2), 16)
dyld = absd = None
for ln in text.splitlines():
    if "/usr/lib/dyld" in ln and "absd" not in ln:
        m = re.search(r"0x[0-9a-fA-F]+", ln)
        if m:
            dyld = int(m.group(0), 16)
    if "/usr/sbin/absd" in ln:
        m = re.search(r"0x[0-9a-fA-F]+", ln)
        if m:
            absd = int(m.group(0), 16)
if absd and dyld and absd < (1 << 32):
    absd = (dyld & 0xFFFFFFFF00000000) | absd

print("\n=== ANALYSIS ===")
if dyld:
    print(f"dyld+0x68310 = {dyld + 0x68310:#x}")
    print(f"dyld+0x54000 = {dyld + 0x54000:#x}")
if "x22" in regs and dyld:
    print(f"x22 match: {regs['x22'] == dyld + 0x68310} x22={regs['x22']:#x}")
if "x21" in regs and dyld:
    print(f"x21 match: {regs['x21'] == dyld + 0x54000} x21={regs['x21']:#x}")
for k in ["pc", "x19", "x21", "x22", "x28", "x0", "x1", "x2", "x3"]:
    if k in regs:
        print(f"{k} = {regs[k]:#018x}")

c.close()

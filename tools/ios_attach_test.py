import paramiko
import re

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect("192.168.110.36", username="root", password="alpine", timeout=8)

_, o, _ = c.exec_command("ps aux | grep /usr/sbin/absd | grep -v grep", timeout=10)
line = o.read().decode().strip()
print("proc:", line)
m = re.search(r"^\S+\s+(\d+)", line)
pid = m.group(1) if m else None
if not pid:
    print("no absd")
    c.close()
    raise SystemExit(1)

out = c.exec_command(
    f"lldb -b -o 'process attach -p {pid}' -o 'image list dyld' -o 'image list absd' -o quit 2>&1",
    timeout=20,
)[1].read().decode()
print(out)

# verify __NSConcreteStackBlock at dyld+0x68310
dyld = None
for ln in out.splitlines():
    if "/usr/lib/dyld" in ln:
        addrs = re.findall(r"0x[0-9a-fA-F]+", ln)
        if addrs:
            dyld = max(int(a, 16) for a in addrs)
if dyld:
    addr = dyld + 0x68310
    mem = c.exec_command(
        f"lldb -b -o 'process attach -p {pid}' -o 'memory read -fx -c 8 {addr}' -o quit 2>&1",
        timeout=20,
    )[1].read().decode()
    print(f"\nmemory @ dyld+0x68310 ({addr:#x}):\n", mem)

c.close()

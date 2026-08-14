import paramiko

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect("192.168.110.36", username="root", password="alpine", timeout=15)
cmds = [
    "nm -n /usr/sbin/absd 2>&1 | grep start | head -10",
    "otool -hv /usr/sbin/absd 2>&1 | head -5",
]
for cmd in cmds:
    print("===", cmd)
    _i, o, e = c.exec_command(cmd, timeout=30)
    print(o.read().decode())
    err = e.read().decode()
    if err:
        print("ERR", err)
c.close()

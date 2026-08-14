import paramiko
c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect("192.168.110.36", username="root", password="alpine", timeout=8)
c.exec_command("killall -9 lldb debugserver 2>/dev/null; true")
script = """
target create /usr/sbin/absd
process launch -s
register read pc
process thread step-inst
register read pc
process thread step-inst
register read pc
quit
"""
c.exec_command("cat > /tmp/t.lldb << 'EOF'\n" + script + "EOF")
_, o, _ = c.exec_command("lldb -b -s /tmp/t.lldb 2>&1", timeout=30)
print(o.read().decode())
c.close()

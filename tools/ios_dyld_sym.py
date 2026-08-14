import paramiko
c=paramiko.SSHClient(); c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect('192.168.110.36',username='root',password='alpine',timeout=8)
for cmd in ['nm -n /usr/lib/dyld 2>&1 | grep -i NSConcrete | head','nm -n /cores/usr/lib/dyld 2>&1 | grep -i NSConcrete | head','otool -l /usr/lib/dyld 2>&1 | head -3']:
    print('===',cmd)
    print(c.exec_command(cmd,timeout=15)[1].read().decode())
c.close()

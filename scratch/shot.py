import subprocess, time, socket, threading
lines=[]; lock=threading.Lock()
def rd(p):
    for l in p.stdout:
        with lock: lines.append(l.rstrip())
def mon(c):
    s=socket.socket(); s.connect(("localhost",45554)); s.sendall((c+"\n").encode()); time.sleep(0.15); s.close()
def key(k): mon("sendkey "+k); time.sleep(0.12)
subprocess.run("pkill -9 qemu-system-x86_64",shell=True); time.sleep(1)
p=subprocess.Popen(["qemu-system-x86_64","-boot","d","-cdrom","build/vibeos.iso","-m","512M","-vga","std","-no-reboot","-cpu","max","-accel","tcg","-netdev","user,id=net0","-device","e1000,netdev=net0","-audiodev","coreaudio,id=a0","-device","AC97,audiodev=a0","-drive","file=vibeos-disk.img,format=raw,if=ide,index=0,media=disk","-serial","stdio","-monitor","telnet:localhost:45554,server,nowait","-display","none"],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
t=threading.Thread(target=rd,args=(p,)); t.daemon=True; t.start()
time.sleep(6)
for c in "gui": key(c)
key("ret"); time.sleep(16)
mon("screendump scratch/s1.ppm"); time.sleep(1)
p.terminate()
try: p.wait(timeout=5)
except: p.kill()
with lock: ll=list(lines)
crash=[l for l in ll if "PROCESS CRASH" in l or ("FAULT" in l and "user fault" in l)]
print("CRASH" if crash else "no crash")
for l in crash[:6]: print(l)

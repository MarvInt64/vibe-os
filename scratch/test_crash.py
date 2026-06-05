import subprocess
import time
import socket

def send_qemu(cmd):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(('localhost', 45554))
        s.sendall((cmd + '\n').encode())
        s.close()
    except Exception as e:
        print(f"Telnet error: {e}")

key_map = {
    'a': 'a', 'b': 'b', 'c': 'c', 'd': 'd', 'e': 'e', 'f': 'f', 'g': 'g', 'h': 'h',
    'i': 'i', 'j': 'j', 'k': 'k', 'l': 'l', 'm': 'm', 'n': 'n', 'o': 'o', 'p': 'p',
    'q': 'q', 'r': 'r', 's': 's', 't': 't', 'u': 'u', 'v': 'v', 'w': 'w', 'x': 'x',
    'y': 'y', 'z': 'z',
    'A': 'shift-a', 'B': 'shift-b', 'C': 'shift-c', 'D': 'shift-d', 'E': 'shift-e',
    'F': 'shift-f', 'G': 'shift-g', 'H': 'shift-h', 'I': 'shift-i', 'J': 'shift-j',
    'K': 'shift-k', 'L': 'shift-l', 'M': 'shift-m', 'N': 'shift-n', 'O': 'shift-o',
    'P': 'shift-p', 'Q': 'shift-q', 'R': 'shift-r', 'S': 'shift-s', 'T': 'shift-t',
    'U': 'shift-u', 'V': 'shift-v', 'W': 'shift-w', 'X': 'shift-x', 'Y': 'shift-y',
    'Z': 'shift-z',
    '0': '0', '1': '1', '2': '2', '3': '3', '4': '4', '5': '5', '6': '6', '7': '7',
    '8': '8', '9': '9',
    '/': 'slash', ';': 'semicolon', '.': 'dot', ' ': 'spc', '-': 'minus', '_': 'shift-minus',
    '\n': 'ret', ':': 'shift-semicolon', '=': 'equal'
}

def type_string(s):
    for char in s:
        if char in key_map:
            send_qemu(f"sendkey {key_map[char]}")
        else:
            print(f"Skipping char: {char}")
        time.sleep(0.15)

def main():
    # Kill any running QEMU instances
    subprocess.run("pkill -9 qemu-system-x86_64", shell=True)
    time.sleep(1)

    print("Starting QEMU...")
    # Start QEMU headless
    qemu_cmd = [
        "qemu-system-x86_64",
        "-boot", "d",
        "-cdrom", "build/vibeos.iso",
        "-m", "512M",
        "-vga", "std",
        "-no-reboot",
        "-cpu", "max",
        "-accel", "tcg",
        "-drive", "file=vibeos-disk.img,format=raw,if=ide,index=0,media=disk",
        "-serial", "stdio",
        "-monitor", "telnet:localhost:45554,server,nowait",
        "-display", "none"
    ]
    
    proc = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    
    # Wait for boot shell to be ready
    time.sleep(5)
    
    print("Starting desktop (gui)...")
    type_string("gui\n")
    time.sleep(12)
    
    print("Running filebrowser in normal mode directly from the shell...")
    type_string("/bin/filebrowser\n")
    time.sleep(10)
    
    print("Reading journal.log...")
    type_string("cat /journal.log\n")
    time.sleep(2)
    
    print("Shutting down QEMU...")
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        
    output, _ = proc.communicate()
    print("\n--- QEMU Serial Output ---")
    print(output)

if __name__ == '__main__':
    main()

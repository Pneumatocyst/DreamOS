#!/usr/bin/env python3
"""Drive dream in headless QEMU: type text, press keys, move the mouse, take screenshots.

    python3 tools/qemu_drive.py script.txt out_dir

script lines:
    wait <seconds>          type <text>         key <qemu-key-name>
    mouse <dx> <dy>         click [left|right]  shot <name>
Each `shot` writes out_dir/<name>.png. Uses a scratch disk, never disk.img.
"""
import os, socket, subprocess, sys, time

KEYMAP = {' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal', '.': 'dot', ',': 'comma',
          '/': 'slash', ';': 'semicolon', "'": 'apostrophe', '[': 'bracket_left',
          ']': 'bracket_right', '\\': 'backslash', '`': 'grave_accent'}
SHIFTED = {'_': 'minus', '+': 'equal', '>': 'dot', '<': 'comma', '?': 'slash', ':': 'semicolon',
           '"': 'apostrophe', '|': 'backslash', '~': 'grave_accent', '!': '1', '@': '2', '#': '3',
           '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0'}

def keys_for(ch):
    if ch.isalpha():
        return ('shift-' + ch.lower()) if ch.isupper() else ch
    if ch.isdigit(): return ch
    if ch in KEYMAP: return KEYMAP[ch]
    if ch in SHIFTED: return 'shift-' + SHIFTED[ch]
    raise ValueError(repr(ch))

def main():
    script, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    sock = os.path.join(out, 'mon.sock')
    disk = os.path.join(out, 'scratch.img')
    if not os.path.exists(disk):
        with open(disk, 'wb') as f: f.truncate(32 << 20)
    if os.path.exists(sock): os.remove(sock)
    q = subprocess.Popen(['qemu-system-x86_64', '-M', 'pc', '-m', '512M', '-cdrom', 'dream.iso',
                          '-boot', 'd', '-drive', f'file={disk},format=raw,if=ide,index=0',
                          '-display', 'none', '-monitor', f'unix:{sock},server,nowait',
                          '-serial', f'file:{out}/serial.log', '-no-reboot', '-rtc', 'base=localtime'])
    for _ in range(100):
        if os.path.exists(sock): break
        time.sleep(0.05)
    s = socket.socket(socket.AF_UNIX); s.connect(sock); s.settimeout(0.2)
    def cmd(c):
        s.sendall((c + '\n').encode()); time.sleep(0.03)
        try:
            while s.recv(65536): pass
        except socket.timeout: pass
    try:
        for line in open(script):
            line = line.rstrip('\n')
            if not line.strip() or line.startswith('#'): continue
            op, _, arg = line.partition(' ')
            if op == 'wait': time.sleep(float(arg))
            elif op == 'type':
                for ch in arg.replace('\\n', '\n'): cmd('sendkey ' + keys_for(ch))
            elif op == 'key': cmd('sendkey ' + arg)
            elif op == 'mouse':
                dx, dy = map(int, arg.split()); cmd(f'mouse_move {dx} {dy}')
            elif op == 'click':
                b = 2 if arg == 'right' else 1
                cmd(f'mouse_button {b}'); time.sleep(0.1); cmd('mouse_button 0')
            elif op == 'shot':
                ppm = os.path.abspath(f'{out}/{arg}.ppm'); cmd(f'screendump {ppm}'); time.sleep(0.4)
                subprocess.run(['convert', ppm, f'{out}/{arg}.png']); os.remove(ppm)
    finally:
        cmd('quit'); q.wait(timeout=10)

if __name__ == '__main__':
    main()

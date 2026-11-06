#!/usr/bin/env python3
"""Drive dream in headless QEMU: type text, press keys, move the mouse, take screenshots.

    python3 tools/qemu_drive.py script.txt out_dir

script lines:
    wait <seconds>          type <text>             key <qemu-key-name>
    move <x> <y>            mouse <dx> <dy>         click [left|right]
    down / up               (hold / let go of the left button, for dragging)
    shot <name>             (writes out_dir/<name>.png)

It talks to QEMU over QMP (its JSON control socket) and moves the pointer in absolute
screen coordinates, the way QEMU's own window does when dream's vmmouse driver is
active, so positions are exact. `mouse` moves relative to where the pointer is.

It boots with a scratch disk (out_dir/scratch.img), never your disk.img. Settings saved
during a run persist in that scratch disk; delete it for a clean boot.
"""
import json, os, socket, subprocess, sys, time

SCREEN_W, SCREEN_H = 1280, 800      # what Limine gives dream in QEMU by default

SHIFTED = {'_': 'minus', '+': 'equal', '>': 'dot', '<': 'comma', '?': 'slash', ':': 'semicolon',
           '"': 'apostrophe', '|': 'backslash', '~': 'grave_accent', '!': '1', '@': '2', '#': '3',
           '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0',
           '{': 'bracket_left', '}': 'bracket_right'}
PLAIN = {' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal', '.': 'dot', ',': 'comma',
         '/': 'slash', ';': 'semicolon', "'": 'apostrophe', '[': 'bracket_left',
         ']': 'bracket_right', '\\': 'backslash', '`': 'grave_accent', '\t': 'tab'}


def keys_for(ch):
    """A character -> the key combination that types it, e.g. 'A' -> 'shift-a'."""
    if ch.isalpha():
        return 'shift-' + ch.lower() if ch.isupper() else ch
    if ch.isdigit():
        return ch
    if ch in PLAIN:
        return PLAIN[ch]
    if ch in SHIFTED:
        return 'shift-' + SHIFTED[ch]
    raise ValueError(f'no key for {ch!r}')


class Qemu:
    def __init__(self, out):
        self.out = out
        disk = os.path.join(out, 'scratch.img')
        if not os.path.exists(disk):
            with open(disk, 'wb') as f:
                f.truncate(32 << 20)
        sock = os.path.join(out, 'qmp.sock')
        if os.path.exists(sock):
            os.remove(sock)
        self.proc = subprocess.Popen([
            'qemu-system-x86_64', '-M', 'pc', '-m', '512M', '-cdrom', 'dream.iso', '-boot', 'd',
            '-drive', f'file={disk},format=raw,if=ide,index=0', '-display', 'none',
            '-qmp', f'unix:{sock},server,nowait', '-serial', f'file:{out}/serial.log',
            '-no-reboot', '-rtc', 'base=localtime'])
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.05)
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(sock)
        self.f = self.sock.makefile('rw')
        self.f.readline()                        # the greeting
        self.cmd('qmp_capabilities')
        self.x, self.y = SCREEN_W // 2, SCREEN_H // 2

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args} if args else {'execute': name}) + '\n')
        self.f.flush()
        while True:
            reply = json.loads(self.f.readline())
            if 'return' in reply or 'error' in reply:   # skip asynchronous events
                return reply

    def key(self, combo):
        keys = [{'type': 'qcode', 'data': k} for k in combo.split('-')]
        self.cmd('send-key', keys=keys)
        time.sleep(0.03)

    def move(self, x, y):
        self.x = max(0, min(SCREEN_W - 1, x))
        self.y = max(0, min(SCREEN_H - 1, y))
        self.cmd('input-send-event', events=[
            {'type': 'abs', 'data': {'axis': 'x', 'value': self.x * 32767 // (SCREEN_W - 1)}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': self.y * 32767 // (SCREEN_H - 1)}}])
        time.sleep(0.02)

    def button(self, down, which='left'):
        self.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': which}}])
        time.sleep(0.05)

    def shot(self, name):
        ppm = os.path.abspath(os.path.join(self.out, name + '.ppm'))
        self.cmd('screendump', filename=ppm)
        subprocess.run(['convert', ppm, os.path.join(self.out, name + '.png')], check=True)
        os.remove(ppm)

    def quit(self):
        try:
            self.cmd('quit')
        finally:
            self.proc.wait(timeout=10)


def main():
    script, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    q = Qemu(out)
    try:
        for line in open(script):
            line = line.rstrip('\n')
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            op, _, arg = line.partition(' ')
            if op == 'wait':
                time.sleep(float(arg))
            elif op == 'type':
                for ch in arg.replace('\\n', '\n'):
                    q.key(keys_for(ch))
            elif op == 'key':
                q.key(arg)
            elif op == 'move':
                x, y = map(int, arg.split())
                q.move(x, y)
            elif op == 'mouse':
                dx, dy = map(int, arg.split())
                steps = max(1, max(abs(dx), abs(dy)) // 40)   # glide, so hover effects happen
                x0, y0 = q.x, q.y
                for i in range(1, steps + 1):
                    q.move(x0 + dx * i // steps, y0 + dy * i // steps)
            elif op == 'click':
                which = 'right' if arg == 'right' else 'left'
                q.button(True, which)
                q.button(False, which)
            elif op in ('down', 'up'):
                q.button(op == 'down')
            elif op == 'shot':
                time.sleep(0.15)                 # let the desktop draw the last frame
                q.shot(arg)
            else:
                raise SystemExit(f'unknown command: {line}')
    finally:
        q.quit()


if __name__ == '__main__':
    main()

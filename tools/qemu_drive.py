#!/usr/bin/env python3
"""Drive the Socrates BSD 9 VM through the QEMU human monitor (telnet).

Usage: qemu_drive.py <port> <script-file>
Script lines:
  sleep <seconds>
  key <qemu-keyname>          e.g. key a / key ret / key shift-semicolon
  type <text>                 types ASCII text (letters, digits, most punct)
  mouse_home                  force pointer to (0,0)
  mouse <x> <y>               absolute move from (0,0) via one relative jump
  click                       left press+release
  shot <path.ppm>             screendump
  raw <monitor command>
"""
import socket
import sys
import time

KEYMAP = {
    ' ': 'spc', '.': 'dot', ',': 'comma', '/': 'slash', '-': 'minus',
    '=': 'equal', ';': 'semicolon', "'": 'apostrophe', '[': 'bracket_left',
    ']': 'bracket_right', '\\': 'backslash', '`': 'grave_accent',
    ':': 'shift-semicolon', '!': 'shift-1', '@': 'shift-2', '#': 'shift-3',
    '$': 'shift-4', '%': 'shift-5', '^': 'shift-6', '&': 'shift-7',
    '*': 'shift-8', '(': 'shift-9', ')': 'shift-0', '_': 'shift-minus',
    '+': 'shift-equal', '?': 'shift-slash', '<': 'shift-comma',
    '>': 'shift-dot', '"': 'shift-apostrophe',
}


def keyname(ch):
    if ch.isalpha():
        return ('shift-' + ch.lower()) if ch.isupper() else ch
    if ch.isdigit():
        return ch
    return KEYMAP.get(ch)


def main():
    port = int(sys.argv[1])
    script = open(sys.argv[2]).read().splitlines()

    s = socket.create_connection(('127.0.0.1', port), timeout=10)
    s.settimeout(0.05)

    def drain():
        try:
            while True:
                d = s.recv(65536)
                if not d:
                    break
        except socket.timeout:
            pass

    def cmd(c, pause=0.06):
        s.sendall((c + '\n').encode())
        time.sleep(pause)
        drain()

    drain()

    for line in script:
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        op, _, arg = line.partition(' ')
        if op == 'sleep':
            time.sleep(float(arg))
        elif op == 'key':
            cmd('sendkey ' + arg, 0.08)
        elif op == 'type':
            for ch in arg:
                k = keyname(ch)
                if k:
                    cmd('sendkey ' + k, 0.07)
        elif op == 'mouse_home':
            for _ in range(10):
                cmd('mouse_move -200 -200', 0.04)
        elif op == 'mouse':
            x, y = int(arg.split()[0]), int(arg.split()[1])
            for _ in range(12):
                cmd('mouse_move -200 -200', 0.04)
            # settle the guest's FIR smoothing at the origin
            for _ in range(2):
                cmd('mouse_move 1 1', 0.04)
                cmd('mouse_move -1 -1', 0.04)
            while x > 0 or y > 0:
                dx, dy = min(x, 100), min(y, 100)
                cmd(f'mouse_move {dx} {dy}', 0.05)
                x -= dx
                y -= dy
            # flush the FIR tail so the cursor lands on target
            for _ in range(2):
                cmd('mouse_move 1 1', 0.04)
                cmd('mouse_move -1 -1', 0.04)
        elif op == 'click':
            cmd('mouse_button 1', 0.12)
            cmd('mouse_button 0', 0.12)
        elif op == 'dblclick':
            cmd('mouse_button 1', 0.06)
            cmd('mouse_button 0', 0.06)
            cmd('mouse_button 1', 0.06)
            cmd('mouse_button 0', 0.06)
        elif op == 'shot':
            cmd('screendump ' + arg, 0.4)
        elif op == 'raw':
            cmd(arg, 0.2)
        else:
            print('?? ' + line, file=sys.stderr)
    s.close()


if __name__ == '__main__':
    main()

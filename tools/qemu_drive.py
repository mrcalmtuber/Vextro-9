#!/usr/bin/env python3
"""Drive the Vextro 9 VM through QEMU's QMP socket.

Usage: qemu_drive.py <qmp-port> <script-file>

Start the VM with:
    qemu-system-x86_64 ... -qmp tcp:127.0.0.1:4444,server,nowait

Script lines:
  sleep <seconds>
  key <qemu-keyname>          e.g. key a / key ret / key shift-semicolon
  type <text>                 types ASCII text (letters, digits, most punct)
  mouse <x> <y>               move the pointer to a pixel position
  mouse_home                  same as `mouse 0 0`
  click                       left press+release at the current position
  dblclick                    two of the above in quick succession
  wheel <n>                   scroll n notches (positive = up)
  rclick                      right press+release (jump lists)
  press / release             hold and let go of the left button
  drag <x0> <y0> <x1> <y1>    interpolated drag, for snap and window moves
  shake <x> <y> [amp] [n]     grab a title bar and swing it back and forth
  shot <path.ppm>             screendump
  raw <qmp-command>           execute a bare QMP command by name

This speaks QMP rather than the human monitor because the pointer is what
matters here: the monitor can only send *relative* motion, which reaches
the PS/2 mouse, while the guest prefers the absolute pointer whenever the
host offers one.  Mixing the two lets a monitor click carry the absolute
device's stale position and yank the cursor somewhere unintended — which
never happens with a real host, where every event carries a real
position.  Sending absolute events is both closer to reality and exact.
"""
import json
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

# QEMU's absolute axis range, independent of the guest's resolution
ABS_MAX = 0x7FFF


def keyname(ch):
    if ch.isalpha():
        return ('shift-' + ch.lower()) if ch.isupper() else ch
    if ch.isdigit():
        return ch
    return KEYMAP.get(ch)


class VM:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=15)
        self.f = self.s.makefile('rwb')
        self._reply()                      # greeting
        self.cmd('qmp_capabilities')
        self.w, self.h = self._screen_size()

    def _reply(self):
        while True:
            line = self.f.readline()
            if not line:
                raise EOFError('QMP closed')
            msg = json.loads(line)
            if 'event' in msg:             # async, not our answer
                continue
            return msg

    def cmd(self, name, **args):
        self.f.write((json.dumps({'execute': name,
                                  'arguments': args}) + '\n').encode())
        self.f.flush()
        return self._reply()

    def _screen_size(self):
        """Read the guest's mode off a throwaway screendump."""
        import tempfile
        import os
        path = os.path.join(tempfile.gettempdir(), 'qemu_drive_probe.ppm')
        self.cmd('screendump', filename=path)
        time.sleep(0.4)
        with open(path, 'rb') as fh:
            head = fh.read(64)
        os.unlink(path)
        return tuple(int(v) for v in head.split(b'\n')[1].split())

    def events(self, *evts):
        return self.cmd('input-send-event', events=list(evts))

    def move(self, px, py):
        self.events(
            {'type': 'abs', 'data': {'axis': 'x',
                                     'value': px * ABS_MAX // max(self.w - 1, 1)}},
            {'type': 'abs', 'data': {'axis': 'y',
                                     'value': py * ABS_MAX // max(self.h - 1, 1)}})

    def button(self, down, name='left'):
        self.events({'type': 'btn', 'data': {'down': down, 'button': name}})

    def wheel(self, notches):
        name = 'wheel-up' if notches > 0 else 'wheel-down'
        for _ in range(abs(notches)):
            self.button(True, name)
            self.button(False, name)
            time.sleep(0.05)

    def key(self, combo):
        keys = [{'type': 'qcode', 'data': k} for k in combo.split('-')]
        return self.cmd('send-key', keys=keys)


def main():
    vm = VM(int(sys.argv[1]))
    script = open(sys.argv[2]).read().splitlines()

    for line in script:
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        op, _, arg = line.partition(' ')

        if op == 'sleep':
            time.sleep(float(arg))
        elif op == 'key':
            vm.key(arg)
            time.sleep(0.08)
        elif op == 'type':
            for ch in arg:
                k = keyname(ch)
                if k:
                    vm.key(k)
                    time.sleep(0.07)
        elif op == 'mouse_home':
            vm.move(0, 0)
            time.sleep(0.1)
        elif op == 'mouse':
            x, y = (int(v) for v in arg.split()[:2])
            vm.move(x, y)
            time.sleep(0.2)
        elif op == 'click':
            vm.button(True)
            time.sleep(0.12)
            vm.button(False)
            time.sleep(0.12)
        elif op == 'rclick':
            vm.button(True, 'right')
            time.sleep(0.12)
            vm.button(False, 'right')
            time.sleep(0.12)
        elif op == 'press':
            vm.button(True)
            time.sleep(0.12)
        elif op == 'release':
            vm.button(False)
            time.sleep(0.12)
        elif op == 'drag':
            # drag <x0> <y0> <x1> <y1> [steps]
            # Interpolated rather than a jump: the guest tracks a drag
            # frame by frame, and a single leap looks like teleportation
            # to anything watching the path -- which snap-to-edge is.
            v = [int(t) for t in arg.split()]
            x0, y0, x1, y1 = v[0], v[1], v[2], v[3]
            steps = v[4] if len(v) > 4 else 12
            vm.move(x0, y0)
            time.sleep(0.15)
            vm.button(True)
            time.sleep(0.15)
            for s in range(1, steps + 1):
                vm.move(x0 + (x1 - x0) * s // steps,
                        y0 + (y1 - y0) * s // steps)
                time.sleep(0.05)
            time.sleep(0.15)
            vm.button(False)
            time.sleep(0.15)
        elif op == 'shake':
            # shake <x> <y> [amplitude] [strokes]
            # Grab a title bar and swing it back and forth. Each stroke is
            # one full crossing, which is what the guest counts.
            v = [int(t) for t in arg.split()]
            x, y = v[0], v[1]
            amp = v[2] if len(v) > 2 else 90
            strokes = v[3] if len(v) > 3 else 8
            vm.move(x, y)
            time.sleep(0.15)
            vm.button(True)
            time.sleep(0.15)
            for s in range(strokes):
                target = x + (amp if (s % 2 == 0) else -amp)
                for k in range(1, 5):
                    vm.move(x + (target - x) * k // 4, y)
                    time.sleep(0.02)
            vm.button(False)
            time.sleep(0.15)
        elif op == 'dblclick':
            for _ in range(2):
                vm.button(True)
                time.sleep(0.06)
                vm.button(False)
                time.sleep(0.06)
        elif op == 'wheel':
            vm.wheel(int(arg))
        elif op == 'shot':
            vm.cmd('screendump', filename=arg)
            time.sleep(0.4)
        elif op == 'raw':
            vm.cmd(arg)
            time.sleep(0.2)
        else:
            print('?? ' + line, file=sys.stderr)


if __name__ == '__main__':
    main()

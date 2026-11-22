#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# touchsim: a virtual touchscreen for testing sprite, shaped like the OpenFrame's (an absolute
# pointer, 0-32639 on each axis, left button), that reports a position every 28 ms as the
# real one does. Run as root on the OpenFrame (it needs /dev/uinput).
#   touchsim.py drag X1 Y1 X2 Y2 MS [REPEAT]   drag between two screen points, there and back
#   touchsim.py swipe X1 Y1 X2 Y2 MS          one way only
#   touchsim.py tap X Y
#   touchsim.py hold X Y MS                   press and hold (for screenshots of pressed states)
import fcntl, os, struct, sys, time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BTN_LEFT, ABS_X, ABS_Y = 0x110, 0, 1
RANGE, W, H, PERIOD = 32639, 800, 480, 0.028

fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
for ev in (EV_SYN, EV_KEY, EV_ABS):
    fcntl.ioctl(fd, UI_SET_EVBIT, ev)
fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_LEFT)
for a in (ABS_X, ABS_Y):
    fcntl.ioctl(fd, UI_SET_ABSBIT, a)
absmax = [0] * 64
absmax[ABS_X] = absmax[ABS_Y] = RANGE
# struct uinput_user_dev: name[80], input_id (4 x u16), ff_effects_max, then absmax/min/fuzz/flat
dev = struct.pack('80sHHHHi', b'sprite touchsim', 3, 0x1234, 0x5678, 1, 0)
dev += struct.pack('64i', *absmax) + struct.pack('64i', *([0] * 64)) * 3
os.write(fd, dev)
fcntl.ioctl(fd, UI_DEV_CREATE)
time.sleep(1.0)  # let libinput and sprite pick the device up

def emit(t, c, v):
    os.write(fd, struct.pack('llHHi', 0, 0, t, c, v))

def report(x, y, button=None):
    emit(EV_ABS, ABS_X, int(x * RANGE / W))
    emit(EV_ABS, ABS_Y, int(y * RANGE / H))
    if button is not None:
        emit(EV_KEY, BTN_LEFT, button)
    emit(EV_SYN, 0, 0)

def drag(x1, y1, x2, y2, ms):
    steps = max(1, int(ms / 1000 / PERIOD))
    report(x1, y1, 1)
    for i in range(1, steps + 1):
        time.sleep(PERIOD)
        report(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps)
    time.sleep(PERIOD)
    emit(EV_KEY, BTN_LEFT, 0)
    emit(EV_SYN, 0, 0)

try:
    if sys.argv[1] == 'drag':
        x1, y1, x2, y2, ms = map(float, sys.argv[2:7])
        for _ in range(int(sys.argv[7]) if len(sys.argv) > 7 else 1):
            drag(x1, y1, x2, y2, ms)
            time.sleep(1.0)
            drag(x2, y2, x1, y1, ms)
            time.sleep(1.0)
    elif sys.argv[1] == 'swipe':
        x1, y1, x2, y2, ms = map(float, sys.argv[2:7])
        drag(x1, y1, x2, y2, ms)
        time.sleep(0.6)
    elif sys.argv[1] == 'hold':
        x, y, ms = map(float, sys.argv[2:5])
        report(x, y, 1)
        time.sleep(ms / 1000)
        emit(EV_KEY, BTN_LEFT, 0)
        emit(EV_SYN, 0, 0)
        time.sleep(0.3)
    elif sys.argv[1] == 'tap':
        x, y = map(float, sys.argv[2:4])
        report(x, y, 1)
        time.sleep(0.08)
        emit(EV_KEY, BTN_LEFT, 0)
        emit(EV_SYN, 0, 0)
        time.sleep(0.3)
finally:
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)

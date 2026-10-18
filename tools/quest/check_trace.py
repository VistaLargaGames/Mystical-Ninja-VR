"""check_trace.py run.log: flags frames where the VR eye or view heading jumps.

Reads the PC desk test's [trace] lines (GOEMON_VR_TRACE + trace_on.txt) and prints any
frame-to-frame step of our eye position above EYE_STEP units (walking is ~3 a frame)
or of the view heading beyond what the head sweep explains (the sweep is printed as
"head yaw" in [order] lines; here we only flag heading steps above HEAD_STEP degrees).
"""
import re
import sys

EYE_STEP = 6.0
HEAD_STEP = 6.0

pat = re.compile(r'\[trace\] heading ([-\d.]+) \| \[trace\] game yaw ([-\d.]+) follow (\d) az ([-\d.]+) cut (\d) \| game eye ([-\d. ]+) at ([-\d. ]+) \| player ([-\d. ]+) \| our eye ([-\d. ]+)')
prev = None
n = 0
flags = 0
for line in open(sys.argv[1], errors='replace'):
    m = pat.search(line)
    if not m:
        continue
    n += 1
    heading = float(m.group(1))
    follow = int(m.group(3))
    cut = int(m.group(5))
    eye = [float(v) for v in m.group(9).split()]
    player = [float(v) for v in m.group(8).split()]
    if prev is not None:
        step = sum((a - b) ** 2 for a, b in zip(eye, prev['eye'])) ** 0.5
        dh = (heading - prev['heading'] + 180.0) % 360.0 - 180.0
        if step > EYE_STEP or abs(dh) > HEAD_STEP or follow != prev['follow'] or cut:
            flags += 1
            if flags <= 60:
                print('frame %d: eye step %.1f, heading step %+.1f, follow %d->%d, cut %d, player %s, eye %s' % (
                    n, step, dh, prev['follow'], follow, cut, ' '.join('%.0f' % v for v in player), ' '.join('%.0f' % v for v in eye)))
    prev = {'eye': eye, 'heading': heading, 'follow': follow}
print('%d traced frames, %d flagged' % (n, flags))

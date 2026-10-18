"""shift.py a.png b.png y0 y1 [x0 x1]: horizontal shift (pixels) of band rows y0..y1
from picture a to b, by brute-force matching of the band's column profile.
Used to check that distant scenery and the sky move together when the view turns."""
import sys
import numpy as np
from PIL import Image


def band(path, y0, y1, x0, x1):
    img = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)
    return img[y0:y1, x0:x1].mean(axis=0)  # (width, 3) column profile


def best_shift(a, b, max_shift=400):
    best = (1e30, 0)
    w = a.shape[0]
    for s in range(-max_shift, max_shift + 1):
        if s >= 0:
            pa, pb = a[:w - s], b[s:]
        else:
            pa, pb = a[-s:], b[:w + s]
        if len(pa) < w // 3:
            continue
        err = float(np.mean((pa - pb) ** 2))
        if err < best[0]:
            best = (err, s)
    return best[1], best[0]


a_path, b_path, y0, y1 = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
x0 = int(sys.argv[5]) if len(sys.argv) > 5 else 0
x1 = int(sys.argv[6]) if len(sys.argv) > 6 else None
a = band(a_path, y0, y1, x0, x1 if x1 else 10**9)
b = band(b_path, y0, y1, x0, x1 if x1 else 10**9)
s, err = best_shift(a, b)
print(f"rows {y0}-{y1}: shift {s:+d} px (match error {err:.1f})")

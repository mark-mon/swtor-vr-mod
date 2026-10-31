#!/usr/bin/env python3
"""Compare left/right eye dumps from XrBridgeHost (host_<n>_left.bmp / host_<n>_right.bmp).
Prints mean abs difference, estimated horizontal shift (phase correlation over the centre) and saves a side-by-side
preview + red/cyan anaglyph.   usage: eyecheck.py <dir> [outdir]"""
import sys, os, glob
import numpy as np
from PIL import Image


def shift(a, b):
    fa, fb = np.fft.fft2(a), np.fft.fft2(b)
    r = fa * np.conj(fb)
    r /= np.abs(r) + 1e-9
    c = np.abs(np.fft.ifft2(r))
    y, x = np.unravel_index(np.argmax(c), c.shape)
    if y > a.shape[0] // 2: y -= a.shape[0]
    if x > a.shape[1] // 2: x -= a.shape[1]
    return x, y, c.max()


def main():
    d = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else d
    for lp in sorted(glob.glob(os.path.join(d, "host_*_left.bmp"))):
        rp = lp.replace("_left", "_right")
        if not os.path.exists(rp):
            continue
        L = np.asarray(Image.open(lp).convert("L"), dtype=np.float32)
        R = np.asarray(Image.open(rp).convert("L"), dtype=np.float32)
        h, w = L.shape
        cy, cx = h // 2, w // 2
        a = L[cy - h // 4:cy + h // 4, cx - w // 4:cx + w // 4]
        b = R[cy - h // 4:cy + h // 4, cx - w // 4:cx + w // 4]
        dx, dy, peak = shift(a, b)
        name = os.path.basename(lp).replace("_left.bmp", "")
        print(f"{name}: {w}x{h} mean L {L.mean():.1f} R {R.mean():.1f}  mean|L-R| {np.abs(L - R).mean():.2f}  "
              f"shift dx {dx} dy {dy} (peak {peak:.3f})  identical={np.array_equal(L, R)}")
        li, ri = Image.open(lp).convert("RGB"), Image.open(rp).convert("RGB")
        sbs = Image.new("RGB", (w * 2 + 8, h), "white")
        sbs.paste(li, (0, 0)); sbs.paste(ri, (w + 8, 0))
        sbs.thumbnail((1400, 800)); sbs.save(os.path.join(out, name + "_sbs.png"))
        an = np.zeros((h, w, 3), np.uint8)
        an[..., 0] = L.astype(np.uint8); an[..., 1] = R.astype(np.uint8); an[..., 2] = R.astype(np.uint8)
        im = Image.fromarray(an); im.thumbnail((900, 900)); im.save(os.path.join(out, name + "_anaglyph.png"))


if __name__ == "__main__":
    main()

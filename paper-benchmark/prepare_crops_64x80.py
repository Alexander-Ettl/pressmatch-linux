#!/usr/bin/env python3
"""
Prepare 500 DPI 64x80 px cropped fingerprint dataset matching the legacy
Windows-DLL / OMGrant FT9201 crop geometry (5,120 px = 55.6% of FT9201 native 96x96).
Uses the exact same 11 public 500 DPI databases (206 fingers, 1,648 impressions)
and identical ELAN/PressMatch normalization for apples-to-apples geometry comparison.
"""
import math
import os
import struct
from pathlib import Path
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parent.parent
BASE_DIR = str(REPO_ROOT / "far-benchmark" / "data" / "extracted")
OUT_BIN = str(REPO_ROOT / "paper-benchmark" / "crops_64x80.bin")

W, H = 64, 80

DATASETS = [
    ("download_UareU_sample_DB",      "UareU",        0, 0, 1.0),
    ("download_CrossMatch_Sample_DB", "CrossMatch",   0, 0, 1.0),
    ("fvc2000_Downloads_DB1_B",       "FVC2000_DB1",  1, 0, 1.0),
    ("fvc2000_Downloads_DB2_B",       "FVC2000_DB2c", 1, 1, 1.0),
    ("fvc2000_Downloads_DB3_B",       "FVC2000_DB3",  1, 0, 1.0),
    ("fvc2002_Downloads_DB1_B",       "FVC2002_DB1",  2, 0, 1.0),
    ("fvc2002_Downloads_DB2_B",       "FVC2002_DB2",  2, 0, 500.0 / 569.0),
    ("fvc2002_Downloads_DB3_B",       "FVC2002_DB3c", 2, 1, 1.0),
    ("fvc2004_Downloads_DB1_B",       "FVC2004_DB1",  3, 0, 1.0),
    ("fvc2004_Downloads_DB2_B",       "FVC2004_DB2",  3, 0, 1.0),
    ("fvc2004_Downloads_DB3_B",       "FVC2004_DB3c", 3, 1, 1.0),
]


def find_foreground_centroid_and_box(gray_bytes, iw, ih):
    bs = 16
    gw, gh = iw // bs, ih // bs
    if gw < 2 or gh < 2:
        return iw // 2, ih // 2, 0, 0, iw, ih
    stds = []
    for by in range(gh):
        for bx in range(gw):
            s = s2 = 0
            x0, y0 = bx * bs, by * bs
            for y in range(y0, y0 + bs, 2):
                row = y * iw
                for x in range(x0, x0 + bs, 2):
                    v = gray_bytes[row + x]
                    s += v
                    s2 += v * v
            n = (bs // 2) * (bs // 2)
            mean = s / n
            stds.append((bx, by, math.sqrt(max(0.0, s2 / n - mean * mean))))
    max_std = max(t[2] for t in stds) if stds else 0.0
    thr = max(12.0, max_std * 0.30)
    wsum = cx = cy = 0.0
    min_x, min_y, max_x, max_y = iw, ih, 0, 0
    for bx, by, sd in stds:
        if sd >= thr:
            w = sd * sd
            wsum += w
            cx += (bx + 0.5) * bs * w
            cy += (by + 0.5) * bs * w
            min_x = min(min_x, bx * bs)
            min_y = min(min_y, by * bs)
            max_x = max(max_x, (bx + 1) * bs)
            max_y = max(max_y, (by + 1) * bs)
    if wsum <= 0.0:
        return iw // 2, ih // 2, 0, 0, iw, ih
    return int(round(cx / wsum)), int(round(cy / wsum)), min_x, min_y, max_x, max_y


def elan_normalize_crop(crop_bytes):
    n = len(crop_bytes)
    s = sorted(crop_bytes)
    lvl0, lvl1, lvl2, lvl3 = s[n // 100], s[(n * 3) // 10], s[(n * 65) // 100], s[(n * 99) // 100]
    d01, d12, d23 = max(lvl1 - lvl0, 1), max(lvl2 - lvl1, 1), max(lvl3 - lvl2, 1)
    out = bytearray(n)
    for i, px in enumerate(crop_bytes):
        if px <= lvl0:
            v = 0
        elif px < lvl1:
            v = (px - lvl0) * 99 // d01
        elif px < lvl2:
            v = 99 + ((px - lvl1) * 56 // d12)
        elif px >= lvl3:
            v = 255
        else:
            v = 155 + min((px - lvl2) * 100 // d23, 100)
        out[i] = v
    return bytes(out)


def crop_patch(gray_bytes, iw, ih, cx, cy):
    x0 = max(0, min(iw - W, int(round(cx - W / 2))))
    y0 = max(0, min(ih - H, int(round(cy - H / 2))))
    buf = bytearray(W * H)
    for r in range(H):
        src = (y0 + r) * iw + x0
        buf[r * W:(r + 1) * W] = gray_bytes[src:src + W]
    return buf


def patch_std(buf):
    n = len(buf)
    mean = sum(buf) / n
    return math.sqrt(sum((v - mean) * (v - mean) for v in buf) / n)


def main():
    os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)
    # Identical physical finger offsets as 96x96 (just cropped to 64x80 center window)
    ENROLL_OFFSETS = [
        [(-20, -20), (0, 0), (20, 20)],
        [(-20, 20), (4, -18), (22, 0)],
        [(-22, 0), (-4, 22), (18, -18)],
        [(-12, -12), (12, 12), (0, -22)],
    ]
    PROBE_OFFSETS = [
        [(0, 0), (-14, -14), (14, 14)],
        [(0, -10), (-16, 12), (16, -12)],
        [(6, 6), (-12, -16), (12, 16)],
        [(-6, 0), (14, -8), (-14, 10)],
    ]

    fingers_out = []
    for ds_idx, (folder, name, cohort, is_cap, scale) in enumerate(DATASETS):
        root_dir = os.path.join(BASE_DIR, folder)
        by_finger = {}
        for r, _, files in os.walk(root_dir):
            for f in files:
                if not f.lower().endswith((".tif", ".tiff", ".bmp", ".png")):
                    continue
                parts = os.path.splitext(f)[0].split("_")
                if len(parts) == 3:
                    fid, imp = f"{parts[0]}_{parts[1]}", int(parts[2])
                elif len(parts) == 2:
                    fid, imp = parts[0], int(parts[1])
                else:
                    continue
                by_finger.setdefault(fid, []).append((imp, os.path.join(r, f)))

        for fid in sorted(by_finger.keys()):
            imps = sorted(by_finger[fid], key=lambda t: t[0])
            if len(imps) < 8:
                continue
            loaded = []
            for _, path in imps[:8]:
                im = Image.open(path).convert("L")
                if abs(scale - 1.0) > 1e-3:
                    im = im.resize((max(W, int(round(im.width * scale))),
                                    max(H, int(round(im.height * scale)))),
                                   Image.Resampling.BICUBIC)
                iw, ih = im.size
                raw = bytes(255 - v for v in im.tobytes())
                loaded.append((raw, iw, ih, *find_foreground_centroid_and_box(raw, iw, ih)))

            enroll_crops = []
            for i in range(4):
                raw, iw, ih, cx, cy, bx0, by0, bx1, by1 = loaded[i]
                for dx, dy in ENROLL_OFFSETS[i]:
                    ccx = max(bx0 + W // 2 - 10, min(bx1 - W // 2 + 10, cx + dx))
                    ccy = max(by0 + H // 2 - 10, min(by1 - H // 2 + 10, cy + dy))
                    patch = crop_patch(raw, iw, ih, ccx, ccy)
                    if patch_std(patch) < 10.0:
                        patch = crop_patch(raw, iw, ih, cx, cy)
                    enroll_crops.append(elan_normalize_crop(patch))

            probe_crops = []
            for i in range(4):
                raw, iw, ih, cx, cy, bx0, by0, bx1, by1 = loaded[4 + i]
                for dx, dy in PROBE_OFFSETS[i]:
                    ccx = max(bx0 + W // 2 - 10, min(bx1 - W // 2 + 10, cx + dx))
                    ccy = max(by0 + H // 2 - 10, min(by1 - H // 2 + 10, cy + dy))
                    patch = crop_patch(raw, iw, ih, ccx, ccy)
                    if patch_std(patch) < 10.0:
                        patch = crop_patch(raw, iw, ih, cx, cy)
                    probe_crops.append(elan_normalize_crop(patch))

            fingers_out.append((ds_idx, cohort, is_cap, name, fid, enroll_crops, probe_crops))

    with open(OUT_BIN, "wb") as fp:
        fp.write(struct.pack("<8sIII", b"ELANFAR1", len(fingers_out), W, H))
        for ds_idx, cohort, is_cap, ds_name, fid, enr, prb in fingers_out:
            fp.write(struct.pack("<IIIII24s24s",
                                 ds_idx, cohort, is_cap, len(enr), len(prb),
                                 ds_name.encode("ascii")[:23],
                                 fid.encode("ascii")[:23]))
            for img in enr:
                fp.write(img)
            for img in prb:
                fp.write(img)

    print(f"Wrote {OUT_BIN}: {len(fingers_out)} fingers at {W}x{H} "
          f"({os.path.getsize(OUT_BIN) / (1024*1024):.2f} MB)")


if __name__ == "__main__":
    main()

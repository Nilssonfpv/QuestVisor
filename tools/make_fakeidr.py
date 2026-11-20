# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE.
"""Make a fake black H.264 IDR that matches a live keyframe-less stream's own SPS/PPS.

    python make_fakeidr.py <live_capture.h264> [out_dir]   ->  out_dir/black_<W>x<H>.h264

How: encode one black frame at the stream's size with h264_nvenc (CABAC, High, same 8x8-transform setting),
then keep its compressed slice data and rewrite only the slice header so it is valid under the live SPS/PPS;
the output is live SPS + live PPS + rewritten IDR. Fails if fields that shape the slice data differ.
Needs an ffmpeg.exe with h264_nvenc (NVIDIA GPU): $FFMPEG, else ../relay/third_party/ffmpeg/bin/ffmpeg.exe, else ffmpeg on PATH."""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from h264ps import nals, parse_sps, parse_pps, write_idr

FFMPEG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "relay", "third_party", "ffmpeg", "bin", "ffmpeg.exe")
FFMPEG = os.environ.get("FFMPEG") or (FFMPEG if os.path.isfile(FFMPEG) else "ffmpeg")
live = open(sys.argv[1], "rb").read()
out_dir = sys.argv[2] if len(sys.argv) > 2 else "."
os.makedirs(out_dir, exist_ok=True)
L = nals(live)
lsps = next(n for n in L if n[0] & 0x1F == 7); lpps = next(n for n in L if n[0] & 0x1F == 8)
ls = parse_sps(lsps); lp = parse_pps(lpps, ls)
w, h = ls["width"], ls["height"]
tmp = os.path.join(tempfile.mkdtemp(), "nv.h264")
subprocess.run([FFMPEG, "-v", "error", "-y", "-f", "lavfi", "-i", f"color=black:s={w}x{h}:r=30", "-frames:v", "1",
                "-c:v", "h264_nvenc", "-profile:v", "high", "-coder", "cabac", "-bf", "0", "-g", "30", "-rc", "constqp",
                "-qp", "30", "-pix_fmt", "yuv420p", "-f", "h264", tmp], check=True)
N = nals(open(tmp, "rb").read())
ns = parse_sps(next(n for n in N if n[0] & 0x1F == 7)); np_ = parse_pps(next(n for n in N if n[0] & 0x1F == 8), ns)
for k in ("chroma_format_idc", "bit_depth_luma", "bit_depth_chroma", "frame_mbs_only", "mbs_w", "map_units_h"):
    assert ls[k] == ns[k], f"SPS field {k} differs: live {ls[k]} vs nvenc {ns[k]}"
for k in ("cabac", "transform_8x8", "constrained_intra"):
    assert lp[k] == np_[k], f"PPS field {k} differs: live {lp[k]} vs nvenc {np_[k]}"
sc = b"\x00\x00\x00\x01"
blob = sc + lsps + sc + lpps + b"".join(sc + write_idr(n, ns, np_, ls, lp) for n in N if n[0] & 0x1F == 5)
path = os.path.join(out_dir, f"black_{w}x{h}.h264")
open(path, "wb").write(blob)
print(f"wrote {path} ({len(blob)} bytes)")

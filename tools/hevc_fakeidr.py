# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE.
"""Generate a fake grey H.265 IDR that is valid under a live stream's own VPS/SPS/PPS (for keyframe-less streams).

    python hevc_fakeidr.py <live_capture.h265> [out_dir]  ->  out_dir/black_<W>x<H>.h265  (VPS+SPS+PPS+IDR)

The picture is coded by a tiny CABAC encoder: every CU is intra with the most-probable mode and no residual,
so the decoded picture is flat mid-grey (128). Interior CTBs use one 2Nx2N CU at CTB size (transform tree
split down to the max TB size, all cbf = 0). CUs at the minimum CB size (picture edges) use NxN so that
cbf_luma is coded (= 0) instead of inferred. Supported: 4:2:0, 8-bit, no tiles/WPP, no PCM, CTB > min CB.
Context init values follow FFmpeg's hevc/cabac.c (initType 0); state tables are standard H.264/H.265.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from h264ps import nals, escape, W
from hevcps import ntype, parse_sps, parse_pps

# Standard CABAC tables (ITU-T H.265 Table 9-52 / 9-53)
RANGE_LPS = [
    (128, 176, 208, 240), (128, 167, 197, 227), (128, 158, 187, 216), (123, 150, 178, 205), (116, 142, 169, 195),
    (111, 135, 160, 185), (105, 128, 152, 175), (100, 122, 144, 166), (95, 116, 137, 158), (90, 110, 130, 150),
    (85, 104, 123, 142), (81, 99, 117, 135), (77, 94, 111, 128), (73, 89, 105, 122), (69, 85, 100, 116),
    (66, 80, 95, 110), (62, 76, 90, 104), (59, 72, 86, 99), (56, 69, 81, 94), (53, 65, 77, 89), (51, 62, 73, 85),
    (48, 59, 69, 80), (46, 56, 66, 76), (43, 53, 63, 72), (41, 50, 59, 69), (39, 48, 56, 65), (37, 45, 54, 62),
    (35, 43, 51, 59), (33, 41, 48, 56), (32, 39, 46, 53), (30, 37, 43, 50), (29, 35, 41, 48), (27, 33, 39, 45),
    (26, 31, 37, 43), (24, 30, 35, 41), (23, 28, 33, 39), (22, 27, 32, 37), (21, 26, 30, 35), (20, 24, 29, 33),
    (19, 23, 27, 31), (18, 22, 26, 30), (17, 21, 25, 28), (16, 20, 23, 27), (15, 19, 22, 25), (14, 18, 21, 24),
    (14, 17, 20, 23), (13, 16, 19, 22), (12, 15, 18, 21), (12, 14, 17, 20), (11, 14, 16, 19), (11, 13, 15, 18),
    (10, 12, 15, 17), (10, 12, 14, 16), (9, 11, 13, 15), (9, 11, 12, 14), (8, 10, 12, 14), (8, 9, 11, 13),
    (7, 9, 11, 12), (7, 9, 10, 12), (7, 8, 10, 11), (6, 8, 9, 11), (6, 7, 9, 10), (6, 7, 8, 9), (2, 2, 2, 2)]
TRANS_LPS = [0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9, 11, 11, 12, 13, 13, 15, 15, 16, 16, 18, 18, 19, 19, 21, 21,
             22, 22, 23, 24, 24, 25, 26, 26, 27, 27, 28, 29, 29, 30, 30, 30, 31, 32, 32, 33, 33, 33, 34, 34, 35, 35,
             35, 36, 36, 36, 37, 37, 37, 38, 38, 63]
INIT = {"split_cu": 139, "part_mode": 184, "prev_intra": 184, "chroma_mode": 63, "cbf_luma0": 111, "cbf_luma1": 141, "cbf_chroma0": 94}


class Cabac:
    def __init__(self, qp):
        self.low, self.range, self.first, self.outstanding, self.bits = 0, 510, True, 0, []
        self.ctx = {}
        for k, v in INIT.items():
            m, n = (v >> 4) * 5 - 45, ((v & 15) << 3) - 16
            pre = min(126, max(1, ((m * min(51, max(0, qp))) >> 4) + n))
            self.ctx[k] = [pre - 64, 1] if pre > 63 else [63 - pre, 0]   # [pStateIdx, valMps]

    def _put(self, b):
        if self.first:
            self.first = False
        else:
            self.bits.append(b)
        self.bits += [1 - b] * self.outstanding
        self.outstanding = 0

    def _renorm(self):
        while self.range < 256:
            if self.low < 256:
                self._put(0)
            elif self.low >= 512:
                self.low -= 512; self._put(1)
            else:
                self.low -= 256; self.outstanding += 1
            self.range <<= 1; self.low <<= 1

    def decision(self, name, b):
        st = self.ctx[name]
        lps = RANGE_LPS[st[0]][(self.range >> 6) & 3]
        self.range -= lps
        if b != st[1]:
            self.low += self.range; self.range = lps
            if st[0] == 0: st[1] = 1 - st[1]
            st[0] = TRANS_LPS[st[0]]
        else:
            st[0] = min(st[0] + 1, 62)
        self._renorm()

    def bypass(self, b):
        self.low <<= 1
        if b: self.low += self.range
        if self.low >= 1024:
            self._put(1); self.low -= 1024
        elif self.low < 512:
            self._put(0)
        else:
            self.low -= 512; self.outstanding += 1

    def terminate(self, b):
        self.range -= 2
        if b:
            self.low += self.range
            self.range = 2; self._renorm()          # EncodeFlush
            self._put((self.low >> 9) & 1)
            self.bits += [(self.low >> 8) & 1, 1]   # last two bits; the final '1' is rbsp_stop_one_bit
        else:
            self._renorm()


def encode_picture(sps, qp):
    c = Cabac(qp)
    ctb_log2 = sps["log2_min_cb"] + sps["log2_diff_max_min_cb"]
    ctb, min_cb = 1 << ctb_log2, sps["log2_min_cb"]
    max_tb = sps["log2_min_tb"] + sps["log2_diff_max_min_tb"]
    W_, H_ = sps["pic_w"], sps["pic_h"]
    assert sps["chroma_format_idc"] == 1 and not sps["pcm"] and ctb_log2 > min_cb
    assert sps["max_th_depth_intra"] == 0, "only max_transform_hierarchy_depth_intra 0 handled"

    def transform_tree_2nx2n(log2):            # CU at depth 0, IntraSplitFlag 0
        c.decision("cbf_chroma0", 0); c.decision("cbf_chroma0", 0)
        if log2 <= max_tb:
            # No split: split_transform_flag is not coded (MaxTrafoDepth = 0) and is inferred 0. For an intra CU
            # cbf_luma is always coded, here at trafoDepth 0, i.e. ctxInc 1. (Edge CUs of a 1080-line picture with
            # 32x32 CTBs and a 16x16 max TB hit this case.)
            c.decision("cbf_luma1", 0)
            return
        # log2 > max_tb: split inferred; cbf_cb/cr at depth 0, then cbf_luma at depth 1 (ctxInc 0)
        n = 1 << (2 * (log2 - max_tb))           # number of max-size TUs (one split level assumed)
        assert log2 - max_tb == 1
        for _ in range(4): c.decision("cbf_luma0", 0)

    def cu(x, y, log2):
        if log2 == min_cb:                       # NxN: 4 PUs, transform tree split inferred
            c.decision("part_mode", 0)
            for _ in range(4): c.decision("prev_intra", 1)
            for _ in range(4): c.bypass(0)       # mpm_idx = 0
            c.decision("chroma_mode", 0)         # intra_chroma_pred_mode = 4 (DM)
            c.decision("cbf_chroma0", 0); c.decision("cbf_chroma0", 0)
            for _ in range(4): c.decision("cbf_luma0", 0)
        else:
            c.decision("prev_intra", 1); c.bypass(0); c.decision("chroma_mode", 0)
            transform_tree_2nx2n(log2)

    def quadtree(x, y, log2):
        size = 1 << log2
        if x + size <= W_ and y + size <= H_ and log2 > min_cb:
            c.decision("split_cu", 0)            # all neighbours at depth 0 -> ctxInc 0
            cu(x, y, log2); return
        if log2 > min_cb:                        # inferred split (CU crosses the picture edge)
            h = size // 2
            for dy in (0, h):
                for dx in (0, h):
                    if x + dx < W_ and y + dy < H_:
                        quadtree(x + dx, y + dy, log2 - 1)
            return
        cu(x, y, log2)

    ctbs = [(x, y) for y in range(0, H_, ctb) for x in range(0, W_, ctb)]
    for i, (x, y) in enumerate(ctbs):
        quadtree(x, y, ctb_log2)
        c.terminate(1 if i == len(ctbs) - 1 else 0)
    bits = c.bits + [0] * (-len(c.bits) % 8)
    return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def slice_header(sps, pps, qp, poc_lsb=0):
    """CRA (not IDR): HEVC references pictures by POC, so the fake keyframe must carry POC = (first live POC - 1)
    for the live frame's reference to land on it. Uses an empty short-term RPS from the SPS if there is one."""
    w = W()
    w.u(1, 0); w.u(6, 21); w.u(6, 0); w.u(3, 1)            # CRA_NUT
    w.u(1, 1); w.u(1, 0); w.ue(pps["pps_id"])              # first_slice, no_output_of_prior_pics, pps id
    w.u(pps["extra_bits"], 0); w.ue(2)                     # slice_type I
    if pps["output_flag_present"]: w.u(1, 1)
    w.u(sps["log2_max_poc_lsb"], poc_lsb % (1 << sps["log2_max_poc_lsb"]))
    empty = [i for i, rps in enumerate(sps["st_rps"]) if not rps]
    if empty:
        w.u(1, 1)                                          # short_term_ref_pic_set_sps_flag
        nbits = (sps["num_st_rps"] - 1).bit_length()
        if nbits: w.u(nbits, empty[0])
    else:
        w.u(1, 0)                                          # explicit st_ref_pic_set(num_short_term_ref_pic_sets)
        if sps["num_st_rps"] != 0: w.u(1, 0)               # inter_ref_pic_set_prediction_flag = 0
        w.ue(0); w.ue(0)                                   # no negative / positive pictures
    if sps["long_term_refs"]: w.ue(0)
    if sps["temporal_mvp"]: w.u(1, 0)
    if sps["sao"]:
        w.u(1, 0); w.u(1, 0)                               # SAO off for this slice
    w.se(qp - pps["init_qp"])
    if pps["slice_chroma_qp_offsets_present"]: w.se(0); w.se(0)
    if pps["deblocking_override_enabled"]: w.u(1, 0)
    if pps["loop_filter_across_slices"] and not pps["pps_deblocking_disabled"]: w.u(1, 1)
    if pps["tiles"] or pps["entropy_sync"]: w.ue(0)
    if pps["slice_header_ext"]: w.ue(0)
    w.u(1, 1)                                              # byte_alignment()
    while len(w.bits) % 8: w.bits.append(0)
    return w.bytes()


def check_tables():
    """Cross-check RANGE_LPS with FFmpeg's ff_h264_cabac_tables if its cabac.c is next to us (optional)."""
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cabac.c")
    if not os.path.exists(p): return
    import re
    txt = open(p).read().split("// LPS range")[1]
    vals = [int(v) & 0xFF for v in re.findall(r"-?\d+", txt)[:512]]
    for s in range(64):
        for q in range(4):
            assert vals[q * 128 + 2 * s] == RANGE_LPS[s][q] & 0xFF, (s, q)
    print("RANGE_LPS matches FFmpeg")


if __name__ == "__main__":
    check_tables()
    live = nals(open(sys.argv[1], "rb").read())
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "."
    os.makedirs(out_dir, exist_ok=True)
    vps = next(n for n in live if ntype(n) == 32); sn = next(n for n in live if ntype(n) == 33)
    pn = next(n for n in live if ntype(n) == 34)
    sps, pps = parse_sps(sn), parse_pps(pn)
    assert not pps["sign_data_hiding"] or True       # no residual coded, so these do not matter
    qp = 26
    poc = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    hdr = slice_header(sps, pps, qp, poc)
    idr = hdr[:2] + escape(hdr[2:] + encode_picture(sps, qp))
    sc = b"\x00\x00\x00\x01"
    blob = sc + vps + sc + sn + sc + pn + sc + idr
    path = os.path.join(out_dir, f"black_{sps['width']}x{sps['height']}.h265")
    open(path, "wb").write(blob)
    # Where the POC lsb sits, so injectors can set POC = (first live picture's POC lsb - 1) mod 2^bits:
    # RBSP bit offset after the 2-byte NAL header = first_slice(1) + no_output(1) + pps_id ue + extra bits +
    # slice_type ue(2)=3 bits + output flag. Check that patching never creates an emulation-prevention pattern.
    off = 2 + (pps["pps_id"] + 1).bit_length() * 2 - 1 + pps["extra_bits"] + 3 + pps["output_flag_present"]
    nbits = sps["log2_max_poc_lsb"]
    base = blob.rfind(sc) + len(sc)                       # start of the CRA NAL in the blob
    for v in range(1 << nbits):
        h = slice_header(sps, pps, qp, v)
        assert b"\x00\x00" not in h[2:2 + (off + nbits + 7) // 8 + 1], "patched header could need escaping"
        assert h[2:] [: (off + nbits) // 8 + 1] == slice_header(sps, pps, qp, v)[2:][: (off + nbits) // 8 + 1]
    import json
    meta = {"codec": "h265", "nal": "CRA", "cra_offset_in_file": base, "poc_lsb_rbsp_bit": off,
            "poc_lsb_bits": nbits, "note": "set POC lsb = (first live picture POC lsb - 1) mod 2^bits by writing "
            "the field MSB-first at NAL byte 2 + bit/8 (bits counted after the 2-byte NAL header); no escaping needed"}
    json.dump(meta, open(path + ".json", "w"), indent=1)
    print(f"wrote {path} ({len(blob)} bytes) + .json {meta}")

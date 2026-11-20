# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE.
"""Minimal H.264 SPS/PPS/slice-header parsing and writing (enough to transplant an IDR slice between parameter sets)."""
import re


def nals(d):
    pos = [m.end() for m in re.finditer(b"\x00\x00\x01", d)]
    out = []
    for k, p in enumerate(pos):
        e = pos[k + 1] - 3 if k + 1 < len(pos) else len(d)
        while e > p and d[e - 1] == 0:
            e -= 1
        out.append(d[p:e])
    return out


def unescape(b):
    o, z = bytearray(), 0
    for x in b:
        if z >= 2 and x == 3:
            z = 0
            continue
        o.append(x)
        z = z + 1 if x == 0 else 0
    return bytes(o)


def escape(b):
    o, z = bytearray(), 0
    for x in b:
        if z >= 2 and x <= 3:
            o.append(3)
            z = 0
        o.append(x)
        z = z + 1 if x == 0 else 0
    return bytes(o)


class R:
    def __init__(s, b): s.b, s.p = b, 0
    def u(s, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((s.b[s.p >> 3] >> (7 - (s.p & 7))) & 1); s.p += 1
        return v
    def ue(s):
        z = 0
        while s.u(1) == 0: z += 1
        return (1 << z) - 1 + s.u(z)
    def se(s):
        k = s.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)


class W:
    def __init__(s): s.bits = []
    def u(s, n, v): s.bits += [(v >> (n - 1 - i)) & 1 for i in range(n)]
    def ue(s, v):
        v += 1; n = v.bit_length(); s.u(n - 1, 0); s.u(n, v)
    def se(s, v): s.ue(2 * v - 1 if v > 0 else -2 * v)
    def bytes(s):
        b = s.bits + [0] * (-len(s.bits) % 8)
        return bytes(int("".join(map(str, b[i:i + 8])), 2) for i in range(0, len(b), 8))


def scaling_list(r, size):
    last = nxt = 8
    for _ in range(size):
        if nxt != 0:
            nxt = (last + r.se() + 256) % 256
        last = nxt if nxt != 0 else last


def parse_sps(nal):
    r = R(unescape(nal[1:])); s = {}
    s["profile_idc"] = r.u(8); s["constraints"] = r.u(8); s["level_idc"] = r.u(8); s["sps_id"] = r.ue()
    s["chroma_format_idc"] = 1; s["bit_depth_luma"] = 8; s["bit_depth_chroma"] = 8
    if s["profile_idc"] in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        s["chroma_format_idc"] = r.ue()
        if s["chroma_format_idc"] == 3: s["separate_colour_plane"] = r.u(1)
        s["bit_depth_luma"] = r.ue() + 8; s["bit_depth_chroma"] = r.ue() + 8
        s["qpprime_y_zero_bypass"] = r.u(1); s["seq_scaling_matrix_present"] = r.u(1)
        if s["seq_scaling_matrix_present"]:
            for i in range(8 if s["chroma_format_idc"] != 3 else 12):
                if r.u(1): scaling_list(r, 16 if i < 6 else 64)
    s["log2_max_frame_num"] = r.ue() + 4
    s["poc_type"] = r.ue()
    if s["poc_type"] == 0:
        s["log2_max_poc_lsb"] = r.ue() + 4
    elif s["poc_type"] == 1:
        s["delta_pic_order_always_zero"] = r.u(1); r.se(); r.se()
        for _ in range(r.ue()): r.se()
    s["max_num_ref_frames"] = r.ue(); s["gaps"] = r.u(1)
    s["mbs_w"] = r.ue() + 1; s["map_units_h"] = r.ue() + 1
    s["frame_mbs_only"] = r.u(1)
    if not s["frame_mbs_only"]: s["mbaff"] = r.u(1)
    s["direct_8x8"] = r.u(1)
    s["crop"] = (0, 0, 0, 0)
    if r.u(1): s["crop"] = (r.ue(), r.ue(), r.ue(), r.ue())
    s["vui"] = r.u(1)
    s["width"] = s["mbs_w"] * 16 - 2 * (s["crop"][0] + s["crop"][1])
    s["height"] = s["map_units_h"] * 16 * (2 - s["frame_mbs_only"]) - 2 * (s["crop"][2] + s["crop"][3])
    return s


def parse_pps(nal, sps):
    r = R(unescape(nal[1:])); p = {}
    p["pps_id"] = r.ue(); p["sps_id"] = r.ue(); p["cabac"] = r.u(1); p["bottom_field_poc"] = r.u(1)
    p["slice_groups"] = r.ue() + 1
    if p["slice_groups"] > 1: raise ValueError("slice groups not supported")
    p["num_ref_l0"] = r.ue() + 1; p["num_ref_l1"] = r.ue() + 1
    p["weighted_pred"] = r.u(1); p["weighted_bipred"] = r.u(2)
    p["pic_init_qp"] = 26 + r.se(); p["pic_init_qs"] = 26 + r.se(); p["chroma_qp_offset"] = r.se()
    p["deblocking_ctrl"] = r.u(1); p["constrained_intra"] = r.u(1); p["redundant_pic_cnt"] = r.u(1)
    p["transform_8x8"] = 0; p["second_chroma_qp_offset"] = p["chroma_qp_offset"]
    if len(r.b) * 8 - r.p > 8:   # more_rbsp_data (approximation good enough for these PPSs)
        p["transform_8x8"] = r.u(1); p["pic_scaling_matrix_present"] = r.u(1)
        if p["pic_scaling_matrix_present"]:
            for i in range(6 + (2 if sps["chroma_format_idc"] != 3 else 6) * p["transform_8x8"]):
                if r.u(1): scaling_list(r, 16 if i < 6 else 64)
        p["second_chroma_qp_offset"] = r.se()
    return p


def parse_idr_slice_header(nal, sps, pps):
    """Returns (fields, bit position where slice_data begins (before cabac alignment))."""
    rb = unescape(nal); r = R(rb[1:]); f = {}
    f["nal_ref_idc"] = (nal[0] >> 5) & 3
    f["first_mb"] = r.ue(); f["slice_type"] = r.ue(); f["pps_id"] = r.ue()
    if sps.get("separate_colour_plane"): r.u(2)
    f["frame_num"] = r.u(sps["log2_max_frame_num"])
    if not sps["frame_mbs_only"]:
        if r.u(1): r.u(1)
    f["idr_pic_id"] = r.ue()
    if sps["poc_type"] == 0:
        f["poc_lsb"] = r.u(sps["log2_max_poc_lsb"])
        if pps["bottom_field_poc"]: r.se()
    elif sps["poc_type"] == 1 and not sps.get("delta_pic_order_always_zero"):
        r.se()
        if pps["bottom_field_poc"]: r.se()
    if pps["redundant_pic_cnt"]: r.ue()
    # I slice: no ref list stuff. dec_ref_pic_marking for IDR:
    f["no_output_of_prior_pics"] = r.u(1); f["long_term_ref"] = r.u(1)
    if pps["cabac"] and f["slice_type"] % 5 not in (2, 4): r.ue()
    f["slice_qp"] = pps["pic_init_qp"] + r.se()
    if pps["deblocking_ctrl"]:
        f["disable_deblock"] = r.ue()
        if f["disable_deblock"] != 1:
            f["alpha"] = r.se(); f["beta"] = r.se()
    return f, r.p + 8, rb


def write_idr(src_nal, src_sps, src_pps, dst_sps, dst_pps):
    """Re-emit an IDR I-slice (CABAC) so it is valid under dst SPS/PPS; slice_data bytes are kept verbatim."""
    f, pos, rb = parse_idr_slice_header(src_nal, src_sps, src_pps)
    assert src_pps["cabac"] and dst_pps["cabac"], "CABAC required on both sides"
    w = W()
    w.u(1, 0); w.u(2, f["nal_ref_idc"] or 3); w.u(5, 5)
    w.ue(f["first_mb"]); w.ue(f["slice_type"]); w.ue(dst_pps["pps_id"])
    w.u(dst_sps["log2_max_frame_num"], 0)
    if not dst_sps["frame_mbs_only"]: w.u(1, 0)
    w.ue(0)                                            # idr_pic_id
    if dst_sps["poc_type"] == 0:
        w.u(dst_sps["log2_max_poc_lsb"], 0)
        if dst_pps["bottom_field_poc"]: w.se(0)
    elif dst_sps["poc_type"] == 1 and not dst_sps.get("delta_pic_order_always_zero"):
        w.se(0)
        if dst_pps["bottom_field_poc"]: w.se(0)
    if dst_pps["redundant_pic_cnt"]: w.ue(0)
    w.u(1, 0); w.u(1, 0)                               # no_output_of_prior_pics, long_term_reference
    w.se(f["slice_qp"] - dst_pps["pic_init_qp"])       # keep SliceQPY identical (CABAC init depends on it)
    if dst_pps["deblocking_ctrl"]:
        w.ue(f.get("disable_deblock", 0))
        if f.get("disable_deblock", 0) != 1:
            w.se(f.get("alpha", 0)); w.se(f.get("beta", 0))
    # cabac_alignment_one_bit(s), then the original slice data bytes (already byte aligned in the source)
    while len(w.bits) % 8: w.bits.append(1)
    src_pos = pos
    while src_pos % 8: src_pos += 1
    return escape(w.bytes() + rb[src_pos // 8:])

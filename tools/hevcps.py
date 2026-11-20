# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE.
"""Minimal H.265 SPS/PPS/IDR-slice-header parsing and writing (enough to transplant an IDR slice between
parameter sets, as tools/make_fakeidr.py does for H.264)."""
from h264ps import nals, unescape, escape, R, W   # same Annex-B / bit helpers


def ntype(n):
    return (n[0] >> 1) & 0x3F


def ptl(r, max_sub):
    r.u(2); r.u(1); prof = r.u(5); r.u(32); r.u(4); r.u(43); r.u(1); lvl = r.u(8)
    sp, sl = [], []
    for _ in range(max_sub):
        sp.append(r.u(1)); sl.append(r.u(1))
    if max_sub > 0:
        for _ in range(max_sub, 8): r.u(2)
    for i in range(max_sub):
        if sp[i]: r.u(88)
        if sl[i]: r.u(8)
    return prof, lvl


def parse_sps(nal):
    r = R(unescape(nal[2:])); s = {}
    s["vps_id"] = r.u(4); msl = r.u(3); r.u(1)
    s["profile"], s["level"] = ptl(r, msl)
    s["sps_id"] = r.ue(); s["chroma_format_idc"] = r.ue()
    if s["chroma_format_idc"] == 3: s["separate_colour_plane"] = r.u(1)
    s["pic_w"] = r.ue(); s["pic_h"] = r.ue()
    s["conf"] = (0, 0, 0, 0)
    if r.u(1): s["conf"] = (r.ue(), r.ue(), r.ue(), r.ue())
    s["bit_depth_luma"] = r.ue() + 8; s["bit_depth_chroma"] = r.ue() + 8
    s["log2_max_poc_lsb"] = r.ue() + 4
    sub = r.u(1)
    for _ in range(0 if sub else msl, msl + 1): r.ue(); r.ue(); r.ue()
    s["log2_min_cb"] = r.ue() + 3; s["log2_diff_max_min_cb"] = r.ue()
    s["log2_min_tb"] = r.ue() + 2; s["log2_diff_max_min_tb"] = r.ue()
    s["max_th_depth_inter"] = r.ue(); s["max_th_depth_intra"] = r.ue()
    s["scaling_list_enabled"] = r.u(1)
    if s["scaling_list_enabled"]:
        s["sps_scaling_list_data"] = r.u(1)
        if s["sps_scaling_list_data"]: raise ValueError("SPS scaling list data not supported")
    s["amp"] = r.u(1); s["sao"] = r.u(1); s["pcm"] = r.u(1)
    if s["pcm"]:
        s["pcm_bits"] = (r.u(4), r.u(4)); s["pcm_sizes"] = (r.ue(), r.ue()); s["pcm_loop_filter_disabled"] = r.u(1)
    s["num_st_rps"] = r.ue()
    s["st_rps"] = []
    for i in range(s["num_st_rps"]):
        s["st_rps"].append(st_ref_pic_set(r, i, s["st_rps"]))
    s["long_term_refs"] = r.u(1)
    if s["long_term_refs"]:
        s["num_lt_sps"] = r.ue()
        for _ in range(s["num_lt_sps"]): r.u(s["log2_max_poc_lsb"]); r.u(1)
    s["temporal_mvp"] = r.u(1); s["strong_intra_smoothing"] = r.u(1)
    s["width"] = s["pic_w"] - 2 * (s["conf"][0] + s["conf"][1])     # 4:2:0 SubWidthC = 2
    s["height"] = s["pic_h"] - 2 * (s["conf"][2] + s["conf"][3])
    return s


def st_ref_pic_set(r, idx, sets, num_sets=None):
    """Parse st_ref_pic_set(idx); returns number of pictures (negative, positive) as a list of deltas."""
    num_sets = len(sets) if num_sets is None else num_sets
    inter = r.u(1) if idx != 0 else 0
    if inter:
        delta_idx = (r.ue() + 1) if idx == num_sets else 1
        ref = sets[idx - delta_idx]
        sign = r.u(1); abs_d = r.ue() + 1
        d_rps = (1 - 2 * sign) * abs_d
        out = []
        for d in ref + [0]:
            used = r.u(1)
            use_delta = r.u(1) if not used else 1
            if used or use_delta:
                nd = d + d_rps
                if nd != 0: out.append(nd)
        return sorted(out)
    neg, pos = r.ue(), r.ue(); out = []; acc = 0
    for _ in range(neg):
        acc -= r.ue() + 1; r.u(1); out.append(acc)
    acc = 0
    for _ in range(pos):
        acc += r.ue() + 1; r.u(1); out.append(acc)
    return sorted(out)


def first_slice_poc_lsb(nal, sps, pps):
    """POC lsb of a non-IDR slice segment (first segment of a picture)."""
    r = R(unescape(nal)[2:]); t = ntype(nal)
    first = r.u(1)
    if 16 <= t <= 23: r.u(1)
    r.ue()
    if not first: return None
    r.u(pps["extra_bits"]); r.ue()
    if pps["output_flag_present"]: r.u(1)
    if sps.get("separate_colour_plane"): r.u(2)
    return r.u(sps["log2_max_poc_lsb"])


def parse_pps(nal):
    r = R(unescape(nal[2:])); p = {}
    p["pps_id"] = r.ue(); p["sps_id"] = r.ue(); p["dependent_slices"] = r.u(1); p["output_flag_present"] = r.u(1)
    p["extra_bits"] = r.u(3); p["sign_data_hiding"] = r.u(1); p["cabac_init_present"] = r.u(1)
    r.ue(); r.ue(); p["init_qp"] = 26 + r.se(); p["constrained_intra"] = r.u(1); p["transform_skip"] = r.u(1)
    p["cu_qp_delta"] = r.u(1); p["diff_cu_qp_delta_depth"] = r.ue() if p["cu_qp_delta"] else 0
    p["cb_qp_offset"] = r.se(); p["cr_qp_offset"] = r.se(); p["slice_chroma_qp_offsets_present"] = r.u(1)
    p["weighted_pred"] = r.u(1); p["weighted_bipred"] = r.u(1); p["transquant_bypass"] = r.u(1)
    p["tiles"] = r.u(1); p["entropy_sync"] = r.u(1)
    if p["tiles"]: raise ValueError("tiles not supported")
    p["loop_filter_across_slices"] = r.u(1); p["deblocking_ctrl"] = r.u(1)
    p["deblocking_override_enabled"] = 0; p["pps_deblocking_disabled"] = 0
    if p["deblocking_ctrl"]:
        p["deblocking_override_enabled"] = r.u(1); p["pps_deblocking_disabled"] = r.u(1)
        if not p["pps_deblocking_disabled"]: r.se(); r.se()
    p["pps_scaling_list_data"] = r.u(1)
    if p["pps_scaling_list_data"]: raise ValueError("PPS scaling list data not supported")
    p["lists_modification"] = r.u(1); p["log2_parallel_merge"] = r.ue() + 2
    p["slice_header_ext"] = r.u(1); p["pps_ext"] = r.u(1)
    return p


def ctb_count(s):
    ctb = 1 << (s["log2_min_cb"] + s["log2_diff_max_min_cb"])
    return ((s["pic_w"] + ctb - 1) // ctb) * ((s["pic_h"] + ctb - 1) // ctb)


def parse_idr_header(nal, sps, pps):
    rb = unescape(nal); r = R(rb[2:]); f = {"type": ntype(nal)}
    f["first_slice"] = r.u(1)
    assert f["type"] in (19, 20), "not an IDR"
    f["no_output_of_prior_pics"] = r.u(1); f["pps_id"] = r.ue()
    if not f["first_slice"]:
        raise ValueError("only single-slice / first slice segment supported")
    r.u(pps["extra_bits"]); f["slice_type"] = r.ue()
    if pps["output_flag_present"]: f["pic_output"] = r.u(1)
    if sps.get("separate_colour_plane"): r.u(2)
    f["sao_luma"] = f["sao_chroma"] = 0
    if sps["sao"]:
        f["sao_luma"] = r.u(1)
        if sps["chroma_format_idc"]: f["sao_chroma"] = r.u(1)
    f["slice_qp"] = pps["init_qp"] + r.se()
    if pps["slice_chroma_qp_offsets_present"]: f["cb"] = r.se(); f["cr"] = r.se()
    f["deblock_override"] = 0; f["deblock_disabled"] = pps["pps_deblocking_disabled"]
    if pps["deblocking_override_enabled"]:
        f["deblock_override"] = r.u(1)
    if f["deblock_override"]:
        f["deblock_disabled"] = r.u(1)
        if not f["deblock_disabled"]: f["beta"] = r.se(); f["tc"] = r.se()
    if pps["loop_filter_across_slices"] and (f["sao_luma"] or f["sao_chroma"] or not f["deblock_disabled"]):
        f["lf_across"] = r.u(1)
    if pps["tiles"] or pps["entropy_sync"]:
        n = r.ue(); f["entry_points"] = n
        if n: raise ValueError("entry points not supported")
    if pps["slice_header_ext"]:
        for _ in range(r.ue()): r.u(8)
    # byte_alignment(): one '1' bit then zeros
    assert r.u(1) == 1
    while r.p % 8: r.u(1)
    return f, r.p // 8 + 2, rb


def write_idr(src_nal, src_sps, src_pps, dst_sps, dst_pps):
    f, data_off, rb = parse_idr_header(src_nal, src_sps, src_pps)
    w = W()
    w.u(1, 0); w.u(6, f["type"]); w.u(6, 0); w.u(3, 1)     # NAL header: type, layer 0, tid 1
    w.u(1, 1); w.u(1, 0); w.ue(dst_pps["pps_id"])
    w.u(dst_pps["extra_bits"], 0); w.ue(f["slice_type"])
    if dst_pps["output_flag_present"]: w.u(1, 1)
    if dst_sps["sao"]:
        w.u(1, f["sao_luma"])
        if dst_sps["chroma_format_idc"]: w.u(1, f["sao_chroma"])
    elif f["sao_luma"] or f["sao_chroma"]:
        raise ValueError("source slice uses SAO but the target SPS disables it")
    w.se(f["slice_qp"] - dst_pps["init_qp"])
    if dst_pps["slice_chroma_qp_offsets_present"]: w.se(f.get("cb", 0)); w.se(f.get("cr", 0))
    want_disabled = f["deblock_disabled"]
    override = 0
    if dst_pps["deblocking_override_enabled"]:
        override = int(want_disabled != dst_pps["pps_deblocking_disabled"] or "beta" in f)
        w.u(1, override)
    if override:
        w.u(1, want_disabled)
        if not want_disabled: w.se(f.get("beta", 0)); w.se(f.get("tc", 0))
    eff_disabled = want_disabled if override else dst_pps["pps_deblocking_disabled"]
    if dst_pps["loop_filter_across_slices"] and (f["sao_luma"] or f["sao_chroma"] or not eff_disabled):
        w.u(1, f.get("lf_across", 1))
    if dst_pps["tiles"] or dst_pps["entropy_sync"]: w.ue(0)
    if dst_pps["slice_header_ext"]: w.ue(0)
    w.u(1, 1)
    while len(w.bits) % 8: w.bits.append(0)
    hdr = w.bytes()
    return hdr[:2] + escape(hdr[2:] + rb[data_off:])

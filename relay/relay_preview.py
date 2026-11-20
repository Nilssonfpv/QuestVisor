"""Video window for the QuestVisor relay: shows the goggles' live video on the PC.

Runs in its own process (started by simple_forwarder.py), so decoding and drawing can never hold up the stream
to the headset: the relay hands this process the same bytes it sends a headset, through a pipe, and resyncs it
like a slow headset if it falls behind.

Decoding: FFmpeg through PyAV, Direct3D 11 hardware decoding when available, software otherwise.

Joining mid-stream: DJI streams have (almost) no keyframes, only P-frames with intra refresh. A GPU decoder that
starts without a reference picture uses uninitialised memory as the reference (dark, blue blotches). So, like
the Quest app, the window starts the decoder on a fake keyframe of the stream's size
(fakeidr/black_<W>x<H>.h264|.h265, the same files the app uses; despite the name they decode to neutral grey; for
H.265 the CRA's POC is patched to "first live picture - 1" so the live P-frames reference it). The size is
learned by decoding the first frames with a throwaway decoder; if that decoder sees a real keyframe it is used
as it is. Measured on armed.h265 (2026-11-20): the intra refresh never repaints static areas, so those keep a
grey wash until the goggles send a real keyframe (arming, mode switch); the headset sees the same. A size
without a fake keyframe is shown as it comes.

Window: pygame-ce (SDL2), resizable, keeps the aspect ratio. Keys: F or F11 = full screen, Esc = leave full
screen, Q = close the window (the relay keeps running).
"""
import collections, json, os, sys, threading, time

TITLE = "QuestVisor Relay - live video"
MAX_QUEUED = 8 * 1024 * 1024           # decoder backlog (bytes) before it skips ahead
CODEC_NAME = {"hevc": "H.265", "h264": "H.264"}


# ---------------------------------------------------------------------------------------------------------------
# Annex-B helpers

class NalSplitter:
    """Feeds stream bytes, returns complete NAL units (without start code). A NAL is complete when the next
    start code arrives; the goggles send an AUD right after each frame, so a frame completes without delay."""

    def __init__(self):
        self.buf = b""

    def feed(self, data):
        buf = self.buf + data
        out, i = [], buf.find(b"\x00\x00\x01")
        if i < 0:
            self.buf = buf[-3:]
            return out
        while True:
            j = buf.find(b"\x00\x00\x01", i + 3)
            if j < 0:
                break
            nal = buf[i + 3:j]
            if nal.endswith(b"\x00"):                   # 4-byte start code of the next NAL
                nal = nal.rstrip(b"\x00")
            if nal:
                out.append(nal)
            i = j
        self.buf = buf[i:]
        return out


def rbsp(nal, skip):
    """NAL payload without emulation-prevention bytes, header (skip bytes) removed."""
    out, zeros = bytearray(), 0
    for b in nal[skip:skip + 64]:                       # slice headers / PPS starts fit easily
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


class Bits:
    def __init__(self, data):
        self.d, self.p = data, 0

    def u(self, n):
        v = 0
        for _ in range(n):
            if self.p >= len(self.d) * 8:
                raise ValueError("out of bits")
            v = (v << 1) | ((self.d[self.p >> 3] >> (7 - (self.p & 7))) & 1)
            self.p += 1
        return v

    def ue(self):
        z = 0
        while self.u(1) == 0:
            z += 1
            if z > 31:
                raise ValueError("bad ue")
        return (1 << z) - 1 + self.u(z)


def hevc_pps_flags(pps):
    """(num_extra_slice_header_bits, output_flag_present) from an H.265 PPS NAL."""
    b = Bits(rbsp(pps, 2))
    b.ue(); b.ue()                                      # pps id, sps id
    b.u(1)                                              # dependent_slice_segments_enabled_flag
    out_flag = b.u(1)
    return b.u(3), bool(out_flag)


def hevc_slice_poc_lsb(nal, extra_bits, out_flag, poc_bits):
    """POC lsb of an H.265 first slice segment, or None."""
    t = (nal[0] >> 1) & 0x3F
    if t > 31:
        return None
    b = Bits(rbsp(nal, 2))
    if not b.u(1):                                      # first_slice_segment_in_pic_flag
        return None
    if 16 <= t <= 23:
        b.u(1)                                          # no_output_of_prior_pics_flag
    b.ue()                                              # pps id
    b.u(extra_bits)
    b.ue()                                              # slice_type
    if out_flag:
        b.u(1)
    if 19 <= t <= 20:                                   # IDR: no POC
        return None
    return b.u(poc_bits)


def set_bits(buf, pos, n, value):
    for i in range(n):
        bit = (value >> (n - 1 - i)) & 1
        byte, off = (pos + i) >> 3, 7 - ((pos + i) & 7)
        buf[byte] = (buf[byte] | (1 << off)) if bit else (buf[byte] & ~(1 << off))


def fakeidr_dirs():
    here = os.path.dirname(os.path.abspath(__file__))
    dirs = [os.path.join(getattr(sys, "_MEIPASS", here), "fakeidr"),                    # inside the .exe
            os.path.join(here, "fakeidr"),                                               # release relay/ folder
            os.path.join(here, "..", "questvisor", "app", "src", "main", "assets", "fakeidr")]   # main repo
    return [d for d in dirs if os.path.isdir(d)]


def load_fake(codec, w, h):
    """(blob bytearray, sidecar dict or None) for black_<w>x<h>.<ext>, or None."""
    name = "black_%dx%d.%s" % (w, h, "h265" if codec == "hevc" else "h264")
    for d in fakeidr_dirs():
        p = os.path.join(d, name)
        if os.path.isfile(p):
            side = None
            if os.path.isfile(p + ".json"):
                with open(p + ".json") as f:
                    side = json.load(f)
            with open(p, "rb") as f:
                return bytearray(f.read()), side
    return None


# ---------------------------------------------------------------------------------------------------------------
# Decoding

def make_decoder(codec):
    """(CodecContext, 'd3d11va' or 'software')."""
    import av
    try:
        from av.codec.hwaccel import HWAccel
        ctx = av.CodecContext.create(codec, "r", hwaccel=HWAccel("d3d11va", allow_software_fallback=True))
        hw = "d3d11va"
    except Exception:                                   # noqa: BLE001 - no GPU decoder: use software
        ctx = av.CodecContext.create(codec, "r")
        ctx.thread_type = "SLICE"                       # no frame threading: it adds frames of delay
        hw = "software"
    ctx.options = {"flags": "+low_delay+output_corrupt", "flags2": "+showall"}
    return ctx, hw


class VideoDecoder:
    """Takes NAL units, returns decoded frames; handles codec switches and the fake-keyframe start."""

    def __init__(self, log):
        self.log = log
        self.codec = None
        self.params = {}            # nal type -> NAL bytes (with start code)
        self.au = []                # NALs (with start codes) of the access unit being collected
        self.ctx = None
        self.state = "idle"         # idle -> probe (learn size) -> inject (wait for a slice) -> live
        self.size = None
        self.pps_flags = (0, False)

    def _codec_of(self, h):
        if self.codec != "hevc" and h == 0x40:          # H.265 VPS
            return "hevc"
        if self.codec != "h264" and h in (0x27, 0x47, 0x67):   # H.264 SPS
            return "h264"
        return self.codec

    def _types(self):
        return ((32, 33, 34), 35) if self.codec == "hevc" else ((7, 8), 9)

    def _ntype(self, nal):
        return ((nal[0] >> 1) & 0x3F) if self.codec == "hevc" else (nal[0] & 0x1F)

    def feed(self, nals):
        frames = []
        for nal in nals:
            c = self._codec_of(nal[0])
            if c != self.codec:
                self.codec, self.params, self.au = c, {}, []
                self.ctx, hw = make_decoder(c)
                self.state, self.size = "probe", None
                self.log("%s stream, %s decoding" % (CODEC_NAME[c], hw))
            if self.codec is None:
                continue
            t = self._ntype(nal)
            params, aud = self._types()
            if t == aud and self.au:
                frames += self._au_done(self.au)
                self.au = []
            if t in params:
                self.params[t] = b"\x00\x00\x00\x01" + nal
                if self.codec == "hevc" and t == 34:
                    try:
                        self.pps_flags = hevc_pps_flags(nal)
                    except ValueError:
                        pass
            self.au.append(b"\x00\x00\x00\x01" + nal)
        return frames

    def _has_keyframe(self, au):
        for n in au:
            t = self._ntype(n[4:])
            if (self.codec == "hevc" and 16 <= t <= 23) or (self.codec == "h264" and t == 5):
                return True
        return False

    def _decode(self, data):
        import av
        try:
            return list(self.ctx.decode(av.Packet(data)))
        except Exception:                               # noqa: BLE001 - corrupt input: keep going
            return []

    def _au_done(self, au):
        data = b"".join(au)
        if self.state == "inject":
            return self._inject(au, data)
        frames = self._decode(data)
        if self.state == "probe" and self._has_keyframe(au):
            self.state = "live"                         # a real keyframe: the decoder is clean already
            self.log("started on a keyframe from the goggles")
        if self.state == "probe" and frames:
            f = frames[-1]
            self.size = (f.width, f.height)
            if load_fake(self.codec, f.width, f.height):
                self.state = "inject"
                return []                               # don't show the corrupt probe pictures
            self.log("no black keyframe for %dx%d %s: showing the stream as it comes (colours may be off "
                     "until the next keyframe)" % (f.width, f.height, CODEC_NAME[self.codec]))
            self.state = "live"
        if self.state == "probe":
            return []
        if frames and (frames[-1].width, frames[-1].height) != self.size:
            self.size = (frames[-1].width, frames[-1].height)
        return frames

    def _inject(self, au, data):
        fake = load_fake(self.codec, *self.size)
        if fake is None:
            self.state = "live"
            return self._decode(data)
        blob, side = fake
        if self.codec == "hevc" and side and side.get("nal") == "CRA":
            poc = None
            for n in au:
                nal = n[4:]
                if ((nal[0] >> 1) & 0x3F) <= 31:
                    try:
                        poc = hevc_slice_poc_lsb(nal, self.pps_flags[0], self.pps_flags[1],
                                                 int(side.get("poc_lsb_bits", 8)))
                    except ValueError:
                        poc = None
                    break
            if poc is None:
                return []                               # no usable slice in this AU: wait for the next
            bits = int(side.get("poc_lsb_bits", 8))
            set_bits(blob, (int(side["cra_offset_in_file"]) + 2) * 8 + int(side["poc_lsb_rbsp_bit"]), bits,
                     (poc - 1) % (1 << bits))
        self.ctx, _ = make_decoder(self.codec)
        self._decode(bytes(blob))                       # the black keyframe (its own parameter sets)
        self.state = "live"
        self.log("started on a black %dx%d keyframe" % self.size)
        return self._decode(b"".join(self.params[t] for t in sorted(self.params)) + data)


# ---------------------------------------------------------------------------------------------------------------
# Window

def run(conn):
    """Process entry point. conn: multiprocessing Connection the relay writes stream bytes to."""
    os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
    import pygame

    def log(msg):
        print("video window: %s" % msg, flush=True)

    q, qbytes, lock, eof = collections.deque(), [0], threading.Lock(), [False]

    def reader():
        try:
            while True:
                b = conn.recv_bytes()
                with lock:
                    q.append(b)
                    qbytes[0] += len(b)
        except (EOFError, OSError):
            eof[0] = True

    threading.Thread(target=reader, daemon=True).start()

    pygame.init()
    flags = pygame.RESIZABLE
    screen = pygame.display.set_mode((960, 540), flags)
    pygame.display.set_caption(TITLE)
    font = pygame.font.Font(None, 28)
    fullscreen, windowed_size = False, (960, 540)

    splitter, dec = NalSplitter(), VideoDecoder(log)
    frame, new_frame = None, False
    last_frame_t, shown, t_fps, fps = 0.0, 0, time.monotonic(), 0.0
    rx_bytes, mbps = 0, 0.0
    clock = pygame.time.Clock()
    running = True
    while running and not eof[0]:
        for ev in pygame.event.get():
            if ev.type == pygame.QUIT:
                running = False
            elif ev.type == pygame.KEYDOWN:
                if ev.key == pygame.K_q:
                    running = False
                elif ev.key in (pygame.K_f, pygame.K_F11) or (ev.key == pygame.K_ESCAPE and fullscreen):
                    fullscreen = not fullscreen
                    if fullscreen:
                        windowed_size = screen.get_size()
                        screen = pygame.display.set_mode((0, 0), pygame.FULLSCREEN)
                    else:
                        screen = pygame.display.set_mode(windowed_size, flags)
                    new_frame = frame is not None

        with lock:
            chunks = list(q)
            q.clear()
            behind = qbytes[0] > MAX_QUEUED
            qbytes[0] = 0
        if behind:
            log("decoder fell behind, skipping ahead")
            chunks = chunks[-1:]
        for data in chunks:
            rx_bytes += len(data)
            out = dec.feed(splitter.feed(data))
            if out:
                frame, new_frame = out[-1], True
                last_frame_t = time.monotonic()
                shown += len(out)

        if new_frame and frame is not None:
            new_frame = False
            sw, sh = screen.get_size()
            fw, fh = frame.width, frame.height
            scale = min(sw / fw, sh / fh)
            w, h = max(2, int(fw * scale)) & ~1, max(2, int(fh * scale)) & ~1
            try:
                rgb = frame.reformat(width=w, height=h, format="rgb24")
                plane = rgb.planes[0]
                img = pygame.image.frombuffer(plane, (w, h), "RGB", plane.line_size)
                screen.fill((0, 0, 0))
                screen.blit(img, ((sw - w) // 2, (sh - h) // 2))
            except Exception as e:                      # noqa: BLE001
                log("draw failed: %s" % e)
            pygame.display.set_caption("%s - %dx%d %s %.0f fps %.1f Mbit/s" % (
                TITLE, fw, fh, CODEC_NAME.get(dec.codec, "?"), fps, mbps))
            pygame.display.flip()
        elif time.monotonic() - last_frame_t > 1.0:
            screen.fill((0, 0, 0))
            msg = "waiting for video from the goggles ..." if frame is None else "no video"
            txt = font.render(msg, True, (200, 200, 200))
            screen.blit(txt, txt.get_rect(center=(screen.get_width() // 2, screen.get_height() // 2)))
            pygame.display.flip()
            last_frame_t = time.monotonic() - 0.5       # redraw twice a second while waiting

        now = time.monotonic()
        if now - t_fps >= 1.0:
            fps, mbps = shown / (now - t_fps), rx_bytes * 8 / (now - t_fps) / 1e6
            shown, rx_bytes, t_fps = 0, 0, now
        if not chunks:
            clock.tick(500)                             # ~2 ms idle wait

    pygame.quit()
    log("closed (the relay keeps running; close the console window or press Ctrl+C to stop it)")

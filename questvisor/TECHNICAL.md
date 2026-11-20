# QuestVisor technical notes

For developers. User documentation is in the [README](../README.md).

## Pipeline

1. **Goggles → PC.** DJI Goggles 3 enumerate on USB as a composite device (`VID_2CA3 PID_0020`); interface
   `MI_00` is RNDIS ("Remote NDIS based Internet Sharing Device"). Windows gets `192.168.60.1/24`; the goggles
   are `192.168.60.2` and serve an Annex-B H.265 or H.264 elementary stream on TCP `9100`, **one client at a
   time**.
2. **Relay** (`relay/simple_forwarder.py`).
   * Zero-hold pass-through: bytes are forwarded as soon as `recv()` returns. The stream is scanned
     incrementally (2-byte carry between chunks) only to cache parameter sets and find AUDs. An earlier version
     released a NAL only when the next start code arrived, which held each frame-closing AUD back by one frame
     interval (~33 ms at 30 fps); removing that cut the app's "decode" figure from 40.6 ms to 12.7 ms.
   * Late joiners get the cached VPS/SPS/PPS (or SPS/PPS) first and start at the next AUD.
   * One sender thread and queue per client; a client whose queue is older than `--max-backlog-ms` (150) or
     larger than `--max-backlog-kb` (1024) is dropped back to "waiting" and resynced at the next AUD.
   * Codec switches are followed on a parameter-set header only the other codec can produce (H.264 SPS byte
     `0x27/0x47/0x67` is odd, i.e. `nuh_layer_id` ≥ 32 in H.265; H.265 VPS `40 01` is NAL type 0 in H.264).
   * Before the first slice of each frame it inserts a `user_data_unregistered` SEI with UUID
     `QRLY-TS-v1\0\0\0\0\0\x01`, a big-endian 64-bit PC arrival time (µs, `perf_counter`) and a 32-bit counter.
     A UDP clock server on 9200 answers an 8-byte request with the request plus the PC time (BE64), so the app
     can map PC time to its own clock. The app never hands the SEI to the decoder.
   * Windows: runs at HIGH priority class.
3. **App** (`questvisor/`).
   * Kotlin + Meta Spatial SDK 0.15 for the scene, panels and input; a native library (`libquestvisor.so`) for
     the socket reader, Annex-B parser and `AMediaCodec` pipeline. Decoding is always the Qualcomm low-latency
     hardware decoders (`c2.qti.hevc.decoder.low_latency`, `c2.qti.avc.decoder.low_latency`). The FFmpeg
     software decoder (`sw_decoder.cpp`) is still compiled and linked but can no longer be selected.
   * The video panel is a compositor layer (`VideoSurfacePanelRegistration`, flat or cylinder) for the lowest
     latency; occlusion mode switches to a mesh panel (`ReadableVideoSurfacePanelRegistration`) plus
     `EnvironmentDepthMode.OCCLUSION`, and hands the running decoder the new surface without a restart.
   * A new SPS with different dimensions restarts the decoder at the next PPS; a codec change resets it.
   * Fake keyframes: the goggles send almost no IRAP pictures (one IDR at arming or a mode switch, then P-slices
     with intra refresh only). `assets/fakeidr/black_<W>x<H>.h265|.h264` (+ `.json` sidecar for H.265) are
     injected before the first live frame so the decoder can start mid-stream.
   * Display refresh rate: the decoded fps, rounded to 10 and stable for 3 readings, picks the lowest multiple
     among 72/80/90/100/120 Hz that is ≥ 90 Hz (then 120, then 90) via `Scene.requestExactDisplayRate()`.

## Adding a fake keyframe for a new stream size

Record a few seconds of the new mode (for example with the relay's output saved to a file), then:

```powershell
python tools\hevc_fakeidr.py <capture.h265> questvisor\app\src\main\assets\fakeidr   # H.265
python tools\make_fakeidr.py <capture.h264> questvisor\app\src\main\assets\fakeidr   # H.264
```

`hevc_fakeidr.py` builds an all-grey/black IDR from the capture's own VPS/SPS/PPS. Check offline that FFmpeg
decodes the fake followed by the live capture before shipping it.

## Launch extras (`am start -n com.xerx.questvisor/.PlayerActivity ...`)

| Extra | Type | Meaning |
|---|---|---|
| `host`, `port` | string, int | relay address (saved) |
| `source` | `network` \| `usb` | intake path |
| `autoconnect` | bool | connect as soon as the video surface exists |
| `stats` | bool | stats column |
| `occlusion` | bool | mesh panel + depth occlusion (saved) |
| `screenwidth`, `curve`, `distance` | float | width (m), curvature 0–1, distance (m) (saved) |
| `fit`, `fov` | float | preset: image diagonal 0 / 100 / 155°; lens FOV for the 155 preset (saved) |
| `headlock` | bool | screen follows the head (saved) |
| `hwexp` | string | decoder experiments: `keyflag`, `recsei`, `dropcorrupt`, `nopicorder` |
| `selftest` | path | decode a file on the device, logs `SELFTEST PASS/FAIL` |
| `decoder` | ignored | always hardware |
| `softedges`, `depthz`, `softdepth` | | experimental soft occlusion shader, off by default, does not work yet |

## Logs

`adb logcat -s QuestVisor:*`. Once a second while running:

```
frames src=Network decoded=… rendered=… dropped=… | avg decode … ms, avg displayed … ms
latency 1s: arrive->queued X ms, queued->decoded Y ms (n=), decoded->displayed Z ms (n=), relay->displayed W ms (n=, clock ok, rtt R ms)
```

`relay->displayed` is the PC-arrival → frame-rendered-callback latency quoted in the README.

## Direct USB (goggles plugged into the Quest)

`source=usb` implements RNDIS, ARP, IPv4 and TCP in userspace over `USBDEVFS_*` ioctls (`usb_rndis.cpp`,
`tinynet.cpp`), so no root or kernel `usb0` would be needed. It does not run today: with the goggles attached the
Quest stays in USB device (sink) role (`current_mode=ufp`) because the goggles present a power-sourcing host, and
Horizon OS does not allow forcing the role. A **powered USB-C hub** between them should make the Quest the host;
untested.

## Measurements (Quest 3, Wi-Fi, relay on the PC)

| Run | Stream | arrive→queued | queued→decoded | decoded→displayed | relay→displayed |
|---|---|---|---|---|---|
| Avata 2 disarmed, 38 s | H.265 1832×1032 30 fps ~21 Mbit/s | 4.7 ms | 7.9 ms | 5.9 ms | 23.2 ms (median 22.5, max 35.1) |
| Avata 2 armed, 60 s | H.265 1920×1080 60 fps ~63 Mbit/s | ~7.3 ms | ~7.5 ms | ~5.5 ms | ~23 ms, 3442 rendered / 3489 decoded, 0 dropped |
| O4 Pro normal | H.264 1920×1080 60 fps ~63 Mbit/s | | 6.8 ms | 6.8 ms | ~28 ms |
| O4 Pro race | H.265 1440×1080 100 fps ~34 Mbit/s | 2.5 ms | 5.0 ms | 5.1 ms | ~23 ms; 21.5–22.2 ms with the display at 100 Hz |
| Race replay, occlusion on/off | H.265 1440×1080 100 fps | | | | 23.9 / 23.3 ms (mesh mode excludes ~1 display frame) |

## Release build

`questvisor/app/build.gradle.kts` reads signing data from `keystore.properties` in `questvisor/` (git-ignored) or
from the file named by `QUESTVISOR_KEYSTORE_PROPERTIES`; see `keystore.properties.example`. Without it,
`:app:preReleaseBuild` fails with instructions. Minify is off: a trial R8 build succeeded and kept `NativePlayer`
and its JNI methods (`proguard-rules.pro`), but `PlayerActivity` reads some Spatial SDK members by reflection and
a minified build has not been tested on a headset.

## Spatial SDK conventions that bit us

* `Quaternion(w, x, y, z)`: the constructor takes **w first**. Identity is `Quaternion(1f, 0f, 0f, 0f)`;
  `Quaternion(0f, 0f, 0f, 1f)` is a 180° roll about z (this turned the screen upside down on every recenter
  until 2026-11-20). `Quaternion(a, b, c)` with three floats is Euler angles, not a vector.
* `Pose.forward()` is +z in entity space; `Scene.getViewerPose()` returns all zeros (identity) for the first
  frames after start, so placement runs again 300 ms later.
* `Grabbable` only grabs the entity whose mesh was hit, and rewrites that entity's transform every frame while
  held: extra rotation (wrist tilt) must go on a child entity.

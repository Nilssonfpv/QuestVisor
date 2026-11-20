# QuestVisor — DJI FPV feed on Meta Quest 3

A Meta Quest 3 (Horizon OS, Android 14) app that receives the live H.265/H.264 FPV stream from
**DJI Goggles 3** and shows it on a grabbable VR panel, decoded with the device's hardware decoder.
Built with the **Meta Spatial SDK 0.15.0** UI layer (a Compose control panel plus a media panel).

Module: `questvisor/` · package `com.xerx.questvisor` · arm64-v8a only · `minSdk/targetSdk 34`

> Built and tested with the goggles' video link in **16:9** mode only; **4:3 is currently untested**.

---

## How the video reaches the headset

Two intake paths exist in the code today:

| Path | Status | What it looks like |
|---|---|---|
| **Network** (`source=network`) | **works** | Goggles → USB → PC (RNDIS, `192.168.60.2:9100`) → `relay/simple_forwarder.py` on `0.0.0.0:9201` → Quest over Wi-Fi |
| **Goggles USB** (`source=usb`) | implemented, **blocked by hardware roles** | Goggles plugged straight into the Quest's USB-C port; the app speaks RNDIS + TCP/IP itself, in userspace |

The network path is the one to use right now. Direct USB is described in
[Direct USB from the goggles](#direct-usb-from-the-goggles) because it needs a powered USB-C hub
before any of that code can run.

Measured on the working path (2026-11-19, zero-hold forwarder): drone disarmed, H.265 1832×1032 30 fps
~21 Mbit/s; armed or flying, **H.265 1920×1080 60 fps ~63 Mbit/s** (P-slices only after one IDR at arming).
`dropped=0` in both modes, average decode **12.7 ms** (first byte → decoded), **~23 ms** PC arrival → on screen.
D-Log M switches the stream to **H.264** 1832×1032 30 fps (~15 Mbit/s); the app and forwarder follow codec and
resolution changes mid-stream (`stream codec changed …` / `stream resolution changed …` in logcat).

With an **O4 Pro** air unit: normal mode is H.264 1920×1080 60 fps (~63 Mbit/s, ~28 ms PC → screen); race mode is
**H.265 1440×1080 100 fps** (~34 Mbit/s, ~23 ms PC → screen). The display refresh rate follows the stream
(60 fps → 120 Hz, 100 fps → 100 Hz, 30 fps → 90 Hz), so every frame is shown for the same number of refreshes. After joining a stream with no keyframes, static areas can keep small blocky remnants
until something moves through them.

---

## Build

Prerequisites: JDK 17+ (developed with `C:\Program Files\Eclipse Adoptium\jdk-21.0.13.9-hotspot`),
Android SDK with `platforms;android-34`, `build-tools;34.0.0`, `ndk;26.1.10909125`, `cmake;3.22.1`,
plus the Gradle wrapper (Gradle 8.14.3, AGP 8.11.1, Kotlin 2.2.0). `questvisor/local.properties`
must point at the SDK (`sdk.dir=…`).

```powershell
$env:JAVA_HOME='C:\Program Files\Eclipse Adoptium\jdk-21.0.13.9-hotspot'
Set-Location questvisor
.\gradlew.bat :app:assembleDebug
```

Output: `questvisor/app/build/outputs/apk/debug/app-debug.apk` (~114 MB — the Spatial SDK native
libraries dominate it). A Kotlin-only change rebuilds in ~10 s; a C++ change re-runs CMake and
relinks `libquestvisor.so`. To confirm a native rebuild really happened, check the timestamp of
`questvisor/app/.cxx/Debug/<hash>/arm64-v8a/CMakeFiles/questvisor.dir/<file>.cpp.o`.

Install and start:

```powershell
$adb = '<sdk>\platform-tools\adb.exe'
$dev = '<quest-ip>:<port>'           # wireless adb (adb mdns services); the port changes every session
& $adb connect $dev
& $adb -s $dev install -r -g 'questvisor\app\build\outputs\apk\debug\app-debug.apk'

# keep the headset awake, otherwise it sleeps before a surface is ever created
& $adb -s $dev shell "am broadcast -a com.oculus.vrpowermanager.prox_close; svc power stayon true"

& $adb -s $dev shell ("am start -n com.xerx.questvisor/.PlayerActivity " +
    "--es host <PC-IP> --ei port 9201 --es source network " +
    "--ez autoconnect true --ez stats true --es decoder hw")
```

The app also appears in the Quest launcher; starting it from the launcher uses the last settings
saved by the control panel.

---

## Using the app

### In-headset control panel

The scene opens in passthrough with two panels 1.4–1.5 m away: a **video panel** (1.6 × 0.9 m quad,
media surface at 1832×1032) and a **control panel** (1.1 × 0.45 m). Both are grabbable and rotate
about the vertical axis. They are placed in front of your head when the scene starts, and again
whenever you recenter (hold the Meta button).

Control panel widgets, top to bottom:

* **Network / Goggles USB** — which intake path to use (the selected one shows a `•`).
* `Host: … Port: … Status: …` — current target and state (`Disconnected`, `Connecting`,
  `Receiving`, `Decoding`, `Running`).
* `Relay found: <PC> (<address>)` / `Looking for the relay on the network...` — relay discovery
  (`RelayDiscovery.kt`). The relay broadcasts `QVISOR1 tcp=9201 clock=9200 name=<pc>` on UDP **9202** once a
  second; the app connects to it by itself while nothing is playing, and switches to it if it is stuck
  connecting to another address for more than 4 s. Without an announcement it uses the last address that
  worked (after 3 s). After **Disconnect** it stays disconnected until **Connect**.
* Decoding is always hardware: the Qualcomm low-latency decoders (`c2.qti.hevc.decoder.low_latency`,
  `c2.qti.avc.decoder.low_latency`). The FFmpeg software decoder was removed on 2026-11-20 (the APK has no
  third-party codec libraries any more; 93 MB instead of 114 MB).
* **Replay buffer** — `Keep: off / 1 / 2 / 5 / 10 min` (default 5), **−30 s**, **−10 s**, **Pause/Play**,
  **Live**. The app records the received stream byte for byte (no frames dropped: every DJI frame is a
  reference frame, so a thinned stream would not decode) into 2-second segments in
  `<external files>/replay/seg_<ms>.qvr`, written on its own thread (`cpp/replay_recorder.h`). Replay feeds
  the segments back through the same parser and decoder at the recorded pace (`Source::Replay`,
  `replayLoop()`); recording pauses while replaying so the footage is not pruned. Seeks land on segment
  starts (2 s steps).
* **keyflag / recsei** — vendor decoder hints: mark input buffers as key frames, and ask for
  recovery SEI handling. Applied as `hwexp` tokens.
* **occlusion** — hands and objects in front of the video panel hide it (Meta Depth API). Draws the
  video in the scene instead of as a compositor layer, which probably adds about one display frame of
  latency; edges are hard and stair-stepped (the Spatial SDK has no soft occlusion; a soft-edge shader was
  tried and does not work yet). Asks for the spatial-data permission the first time.
* **Size − / Size + / Curve / 100° / 155° lens** — screen width (×1.25 per step), curvature (0 % flat to
  100 % wrapped around your head), and two presets that size and curve the screen so the image's
  diagonal spans 100° (all of it inside the Quest's view) or 155° (the camera lens, natural angles).
  Changing them reshapes the screen in place; only a recenter moves it in front of you. The screen also
  follows the stream's aspect ratio (4:3 in race mode). Grab anywhere on the screen to move it.
* **head lock** — the screen follows the headset and stays straight in front of you, like the goggles;
  limited to 100° while on.
* **Cog** (just outside the screen's lower-right corner) — opens or closes the settings panel, which hangs
  under the screen. The cog appears while you point at the screen, while the settings are open, and
  whenever no video is coming in.
* **Moving and sizing the screen** — grab it anywhere (controller grip or hand pinch) to move it; while
  holding, turn your wrist left/right to yaw it and up/down to tilt it. Grab with both hands or both
  controllers and pull apart / push together to scale it; the size is kept.
* **Connect / Disconnect** — starts the selected path (network dials `host:port`; USB goes through
  the Android USB permission flow).
* **Reset defaults** — host back to empty (discovery), port 9201.
* **Show stats** — turns on a right-hand monospace column fed by the native `statusText()`: bytes,
  NAL/access-unit counters, decode and display latency, dropped frames, decoder name, source.

All of these persist in `SharedPreferences` (`questvisor`), so a restart keeps them.

### Launch options (intent extras)

| Extra | Type | Meaning |
|---|---|---|
| `host` | string | relay address (IPv4); normally not needed, discovery finds the relay |
| `port` | int | stream port (default `9201`) |
| `source` | `network` \| `usb` | intake path |
| `decoder` | ignored | decoding is always hardware (`hw`); kept so old launch lines still work |
| `hwexp` | string, comma separated | `keyflag`, `recsei` |
| `autoconnect` | bool | connect as soon as the video surface exists |
| `stats` | bool | enable the stats column |
| `occlusion` | bool | video panel drawn in the scene so real objects can hide it (saved) |
| `screenwidth` / `curve` / `distance` | float | screen width (m), curvature 0–1, distance (m) (saved) |
| `fit` / `fov` | float | screen preset: image diagonal in degrees (0 off, 100, 155); lens diagonal FOV for the 155 preset (saved). `--ez lens155` still works |
| `headlock` | bool | screen follows the headset (saved) |
| `selftest` | path | decode a file on the device instead of a live stream; reports `SELFTEST PASS/FAIL …` in logcat |
| `replaybuffer` | int | replay buffer length in seconds (0 = off) (saved) |
| `replayback` | int | start replay this many seconds back (test hook for the −30 s / −10 s buttons) |
| `replaypause` | bool | pause / resume the replay |
| `live` | bool | back to live video |

Goggles defaults live in `res/values/strings.xml`: `goggles_host 192.168.60.2`, `goggles_port 9100`.

### Feeding the stream from a PC

```powershell
python relay\simple_forwarder.py
```

It connects to the goggles at `192.168.60.2:9100`, listens on `0.0.0.0:9201` for headsets, caches the
latest parameter sets (H.265 VPS/SPS/PPS or H.264 SPS/PPS) and replays them to each new client, then
switches each client on at the next AUD boundary. That matters: the goggles only emit parameter sets
at connection start in some states, so a headset joining mid-stream would otherwise never configure
a decoder.

Bytes are forwarded the instant they arrive (no NAL is held back waiting for the next start code — the
old version delayed every frame by one frame interval that way). Each headset has its own send queue and
is resynced at the next AUD if it falls more than 150 ms behind. The forwarder also answers the app's
UDP clock on port **9200** and inserts a timestamp SEI before each frame, which makes the
`Relay->displayed` stat the real PC-arrival → on-screen latency (`--no-sei` to disable; allow UDP 9200
through Windows Firewall). Every second the app logs
`latency 1s: arrive->queued …, queued->decoded …, decoded->displayed …, relay->displayed …`.

Requirements on the PC side: the goggles' USB composite device (`VID_2CA3 PID_0020`) must enumerate,
with `MI_00` giving Windows the *"Remote NDIS based Internet Sharing Device"* adapter — that is the
interface holding `192.168.60.1/24`. Check with `Get-NetIPAddress` (the adapter name has been
**Ethernet 4**). The goggles serve **one client at a time**, so only one forwarder — or one direct
`frame_timing.py` capture — may hold the connection.

### Watching what it does

```powershell
& $adb -s $dev shell "logcat -c"
& $adb -s $dev shell "am start -n com.xerx.questvisor/.PlayerActivity --es source network --ez autoconnect true"
& $adb -s $dev shell "logcat -d -s QuestVisor:*"
```

Roughly once a second while running:

```
rx 2721279 B/s, nals 7702, aus 854, codec video/hevc, params yes, decoder running
frames src=Network decoded=854 rendered=0 dropped=0 | avg decode 40.6 ms (n=854), avg displayed 0.0 ms (n=0), avg sw 0.0 ms (n=0)
```

Startup also logs the chosen decoders, the fake IDR injection, `configure succeeded variant=… plan=…`,
`output format changed: 1832x1032`, and every `hwexp` value applied.

### Reading the stats

| Field | Meaning |
|---|---|
| `rx B/s`, `nals`, `aus` | wire counters: bytes/sec, NAL units, access units queued |
| `params yes` | decoder has VPS/SPS/PPS, so it can configure |
| `decoded` | output frames dequeued |
| `rendered`, `avg displayed (n=…)` | frames reported by `AMediaCodec_setOnFrameRenderedCallback`; `displayed` = arrival → on-screen |
| `avg decode (n=…)` | dequeue-side decode cost per frame |
| `avg sw (n=…)` | always 0 (the software decoder was removed; the field is kept for log parsers) |
| `dropped` | frames thrown away: no input buffer, buffer too small, or queue overflow |

---

## Source layout

```
questvisor/app/src/main/java/com/xerx/questvisor/
  PlayerActivity.kt    Spatial SDK scene, panels, intent extras, USB permission handling
  ControlPanel.kt      Compose control panel + stats column
  UsbGoggles.kt        Android USB host side: device match, interface/endpoint choice, fd handoff
  NativePlayer.kt      JNI declarations (startNetwork / startUsb / startReplay / setSurface / statusText …)
  RelayDiscovery.kt    listens for the relay's UDP 9202 announcements
  DecoderPicker.kt     picks the c2.qti low-latency decoders
questvisor/app/src/main/cpp/
  native_player.cpp    socket reader, Annex-B parser, MediaCodec pipeline, fake IDR, replay playback, stats
  replay_recorder.h    replay buffer recorder (segment files, writer thread, pruning)
  tinynet.cpp          minimal ARP/IPv4/TCP stack used over RNDIS
  usb_rndis.cpp        RNDIS host implementation over USBDEVFS_CONTROL/BULK
relay/
  simple_forwarder.py  the relay (QuestVisor-Relay.exe is built from it by build_relay_exe.ps1)
  relay_preview.py     the relay's PC video window
tools/
  hevc_fakeidr.py, make_fakeidr.py, hevcps.py, h264ps.py   parameter-set / fake-IDR helpers
```

---

## Direct USB from the goggles

`source=usb` implements the whole intake in userspace: Kotlin claims the RNDIS interfaces and hands
the file descriptor to native code, which drives `USBDEVFS_CONTROL`/`USBDEVFS_BULK`, runs the RNDIS
init/QUERY/SET handshake and then ARP/IPv4/TCP against `192.168.60.2:9100`, so the app does not need
any kernel `usb0` interface or root.

**It cannot connect today because of USB Type-C roles, not code.** With the goggles attached, the
Quest reports `current_mode=ufp`, `power_role=sink`, `data_role=device`, `kernel_state=CONFIGURED`
("charging this device via USB") and an empty `/sys/bus/usb/devices`; xHCI is torn down. The goggles
present a power-sourcing host on their plug, so the headset yields to peripheral role. Software
forcing is unavailable on this firmware (`setprop sys.usb.typec.*` → `Failed to set property`,
`can_change_data_role=false`). The same headset does host correctly: a USB-C flash drive produces
`current_mode=dfp`, `power_role=source`, `data_role=host`, `num_connects=1` and visible device nodes.

Passive USB-C cables, USB-A OTG dongles and PD charge splitters were all tried with the same result
(the goggles back-feed VBUS). The fix is a **powered USB-C hub**: its upstream side enumerates as a
device, making the Quest the host, and its own supply drives VBUS downstream so the goggles fall back
to being an RNDIS device, exactly as they do on the PC. Powered matters — the goggles draw around 10 W.

If, after that, the Horizon kernel claims the interface itself (`ip link` shows `usb0`), the app will
report `USB: claim failed`; Horizon's Ethernet stack pins `usb0` to a static `10.86.13.37/29`, which
cannot reach `192.168.60.0/24`, and the work would move to Android's Ethernet APIs instead.

---

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| No `QuestVisor` log lines at all after `am start` | Headset asleep or a "Finding position in room / Continue without tracking" dialog is holding focus. Wear the headset, dismiss the dialog, then `prox_close` + `svc power stayon true` and relaunch. |
| `rx 0 B/s … codec unknown, params no, decoder not-running` | Nothing is arriving: goggles not attached to the PC (no `192.168.60.x` adapter) or the forwarder lost it (`goggles: timed out`). |
| `aus` climbs but `decoded=0`, no `fake IDR selected` | No fake IDR for this codec + resolution. The stream has no keyframes after the one at arming, so add `assets/fakeidr/black_<W>x<H>.h265` (+ `.json`) with `python tools\hevc_fakeidr.py <capture.h265> <out_dir>` from a capture of that mode. |
| App connects but never configures a decoder | Parameter sets missing — use the forwarder (it caches and replays them) rather than a raw TCP pipe. |
| Panels not visible | Hold the Meta button to recenter; the app moves both panels in front of you (logcat: `onRecenter`, `recenter: head … -> panels along …`). |
| `adb: device … not found` | Wireless adb port is ephemeral; `adb disconnect` then `adb connect <ip>:<port>`. |
| `rendered=0` / `avg displayed n=0` while video is clearly visible | The NDK frame-rendered callback is 4 parameters (`codec, userdata, mediaTimeUs, systemNano`) — a 3-parameter callback silently never counts. Fixed in the current build; if it still reads 0, register before `AMediaCodec_start()` and suspect the media panel. |
| `sdkmanager` installs nothing, mentioning a missing package | It aborts the whole transaction. Drop the unresolvable package (`ninja;1.10.2`); the SDK's CMake bundles ninja. |

---

## Current limitations

* No audio: the goggles' stream carries video only in this pipeline; nothing is decoded or played.
* A resolution switch (arming / disarming while the app runs) restarts the decoder at the new SPS/PPS
  (`stream resolution changed … decoder restarted …` in logcat); the picture returns within a frame or two.
* Direct-USB intake is untested against real goggles hardware (see the hub requirement above).
* Only arm64-v8a; only 1832×1032 goggles geometry has been exercised.
* No layout saving, one stream per app instance.
* The RNDIS notification (interrupt) endpoint is detected and logged but not armed; only add it if
  logs show `RNDIS control read failed` or `RNDIS unexpected response`.

Related notes: [`TECHNICAL.md`](TECHNICAL.md) for the pipeline, fake keyframes and the release build, and the
[main README](../README.md) for setup and troubleshooting.

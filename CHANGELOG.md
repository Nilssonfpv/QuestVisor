# Changelog

## 0.1.0 (first public release)

Quest app `com.xerx.questvisor`, versionName 0.1.0, versionCode 1. Relay `QuestVisor-Relay.exe` 0.1.0.

### App (Meta Quest 3)
* Live DJI Goggles 3 video on a screen in passthrough, decoded with the Quest's hardware low-latency decoders
  (H.265 and H.264).
* About 23 ms from PC arrival to on screen (Wi-Fi, measured with the relay's timestamps); 0 dropped frames in
  the measured runs.
* Follows the goggles' format switches mid-stream: arming (1832×1032 30 fps ↔ 1920×1080 60 fps), D-Log M
  (H.265 ↔ H.264), O4 Pro normal and race mode (1440×1080 100 fps).
* Joins a running stream without keyframes, using built-in black fake keyframes for 1832×1032, 1920×1080 and
  1440×1080 (H.265 and H.264).
* Display refresh rate follows the stream: 30 fps → 90 Hz, 60 fps → 120 Hz, 100 fps → 100 Hz.
* Screen: grab to move, wrist to turn and tilt, two hands to scale; size steps, curvature, 100° and 155° lens
  presets, head lock, follows the stream's aspect ratio.
* Optional occlusion: hands and real objects in front of the screen hide it (hard edges).
* Settings behind a cog next to the screen; stats column with bitrate, latency split and dropped frames.
* Finds the relay on the local network by itself (UDP 9202 announcements) and connects; follows the PC to a
  new address. Manual address with an adb launch option as a fallback.
* Replay buffer: keeps the last 1, 2, 5 or 10 minutes (setting; default 5) of the received video, every frame,
  on the headset. Replay from 30 or 10 seconds back, pause, back to live.
* Hardware decoding only; no third-party codec libraries in the APK.
* App icon.

### Relay (Windows PC)
* Passes the goggles' stream through the instant it arrives (no frame held back), replays parameter sets to
  late-joining headsets, skips a slow headset ahead instead of letting it fall behind, several headsets at once.
* Timestamp SEI per frame and a UDP clock on port 9200, for the app's PC → screen latency figure.
* Packaged as a single `QuestVisor-Relay.exe`; double-click to run with the defaults. Clear status lines
  (waiting for goggles, goggles connected, Quest connected/disconnected, Mbit/s and fps), prints the PC's
  address, `--version`.
* Announces itself once a second on UDP 9202 so the app finds it (`--discovery-port`, `0` = off).

### Known limitations
* **The 0.1.0 APK is debug-signed.** A later release signed with a proper release key will not install over
  it; uninstall 0.1.0 first (`adb uninstall com.xerx.questvisor`). Saved settings, including the PC address,
  are lost with the uninstall.
* Discovery needs UDP broadcasts to reach the Quest; where they don't (some routers between Ethernet and Wi-Fi,
  guest networks), the address has to be set once with adb. There is no on-screen address field.
* Replay can't save a clip to a video file yet, and seeks in 2-second steps.
* Goggles plugged straight into the Quest (Goggles USB) does not work yet: with the goggles attached the Quest
  stays in USB device mode. A powered USB-C hub may fix it; untested.
* No audio.
* The headset must be worn (proximity sensor) for the video to start.
* After joining an O4 Pro stream, still areas can keep blocky remnants until something moves through them.
* If the hardware decoder stops producing frames after corrupt input there is no automatic restart; reconnect.
* Panel rebuilds (size, curve, presets, occlusion) drop frames for well under a second.

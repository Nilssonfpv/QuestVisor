// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE (GPL-3.0 with an additional permission
// to combine with the Meta Spatial SDK).
package com.xerx.questvisor

import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Settings
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.ui.Alignment
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.verticalScroll
import androidx.compose.ui.unit.sp

@Composable
fun ControlPanel(context: Context) {
    val prefs = remember {
        context.getSharedPreferences("questvisor", Context.MODE_PRIVATE)
    }

    val h265Decoder = remember { DecoderPicker.pickDecoder("video/hevc") }
    val h264Decoder = remember { DecoderPicker.pickDecoder("video/avc") }

    var host by remember {
        mutableStateOf(
            prefs.getString("host", context.getString(R.string.default_host))
                ?: context.getString(R.string.default_host)
        )
    }
    var portText by remember {
        mutableStateOf(
            prefs.getString("port", context.getString(R.string.default_port))
                ?: context.getString(R.string.default_port)
        )
    }

    var source by remember {
        mutableStateOf(prefs.getString("source", "network") ?: "network")
    }
    var showStats by remember {
        mutableStateOf(prefs.getBoolean("showStats", false))
    }

    // Always the hardware (Qualcomm low-latency) decoder; the auto/hw/sw choice was removed.
    val decoderMode = "hw"

    var keyflag by remember {
        mutableStateOf(hwexpContains(prefs.getString("hwexp", ""), "keyflag"))
    }
    var recsei by remember {
        mutableStateOf(hwexpContains(prefs.getString("hwexp", ""), "recsei"))
    }

    var occlusion by remember {
        mutableStateOf(prefs.getBoolean("occlusion", false))
    }
    var headLock by remember { mutableStateOf(prefs.getBoolean("headLocked", false)) }
    var screenWidth by remember { mutableStateOf(prefs.getFloat("screenWidth", 1.6f)) }
    var screenCurve by remember { mutableStateOf(prefs.getFloat("screenCurve", 0f)) }
    var fitDeg by remember {
        mutableStateOf(prefs.getFloat("fitDeg", if (prefs.getBoolean("lensMatched", false)) 155f else 0f))
    }

    val hwexpString = remember(keyflag, recsei) {
        listOfNotNull(
            if (keyflag) "keyflag" else null,
            if (recsei) "recsei" else null,
        ).joinToString(",")
    }

    var connected by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("Idle") }
    var statusText by remember { mutableStateOf("") }
    var relayText by remember { mutableStateOf("") }
    var replayText by remember { mutableStateOf("") }
    var replayState by remember { mutableStateOf(0L) }
    var replaySeconds by remember { mutableStateOf(prefs.getInt("replaySeconds", 300)) }

    LaunchedEffect(Unit) {
        while (true) {
            val state = NativePlayer.uiState()
            status = when (state) {
                0 -> "Disconnected"
                1 -> "Connecting"
                2 -> "Receiving"
                3 -> "Decoding"
                else -> "Running"
            }

            val activity = PlayerActivity.current
            connected = state != 0
            if (activity != null && activity.currentHost.isNotBlank()) {
                host = activity.currentHost
                portText = activity.currentPort.toString()
            }
            relayText = RelayDiscovery.current()?.let { "Relay found: ${it.name} (${it.host})" }
                ?: "Looking for the relay on the network..."

            val info = try { NativePlayer.replayInfo() } catch (_: Throwable) { LongArray(4) }
            replayState = if (activity?.replaying == true) maxOf(info[0], 1L) else 0L
            replayText = when {
                replayState == 3L -> "Replay: end of the recording. Press Live."
                replayState != 0L -> "Replay: %s behind the end of the recording%s".format(
                    formatSpan(info[3] - info[1]), if (replayState == 2L) ", paused" else "")
                replaySeconds == 0 -> "Replay buffer off"
                info[3] > 0L -> "Replay buffer: %s recorded".format(formatSpan(info[3] - info[2]))
                else -> "Replay buffer: nothing recorded yet"
            }

            statusText = try {
                NativePlayer.statusText()
            } catch (_: Throwable) {
                ""
            }

            delay(250L)
        }
    }

    val panelModifier = Modifier
        .fillMaxSize()
        .background(Color(0xB3000000), RoundedCornerShape(16.dp))
        .padding(16.dp)

    SpatialTheme(darkTheme = true) {
      androidx.compose.foundation.layout.Row(
          modifier = panelModifier,
          horizontalArrangement = Arrangement.spacedBy(24.dp),
      ) {
        Column(
            modifier = Modifier.weight(1f).fillMaxHeight()
                .verticalScroll(androidx.compose.foundation.rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = {
                        source = "network"
                        prefs.edit().putString("source", source).apply()
                    },
                ) {
                    Text(if (source == "network") "Network •" else "Network", color = Color.White)
                }

                Button(
                    onClick = {
                        source = "usb"
                        prefs.edit().putString("source", source).apply()
                    },
                ) {
                    Text(if (source == "usb") "Goggles USB •" else "Goggles USB", color = Color.White)
                }
            }

            Text("Host: $host   Port: $portText   Status: $status", color = Color.White)
            if (source == "network") Text(relayText, color = Color.White)

            // Replay buffer: the last minutes of received video, for watching a crash again.
            Text(replayText, color = Color.White)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = {
                        val steps = listOf(0, 60, 120, 300, 600)
                        replaySeconds = steps[(steps.indexOf(replaySeconds) + 1) % steps.size]
                        PlayerActivity.current?.applyReplayBuffer(replaySeconds)
                    },
                ) {
                    Text(if (replaySeconds == 0) "Keep: off" else "Keep: ${replaySeconds / 60} min", color = Color.White)
                }
                Button(
                    onClick = { PlayerActivity.current?.replayBack(30) },
                    enabled = replaySeconds > 0,
                ) {
                    Text("−30 s", color = Color.White)
                }
                Button(
                    onClick = { PlayerActivity.current?.replayBack(10) },
                    enabled = replaySeconds > 0,
                ) {
                    Text("−10 s", color = Color.White)
                }
                Button(
                    onClick = { PlayerActivity.current?.setReplayPaused(replayState == 1L) },
                    enabled = replayState == 1L || replayState == 2L,
                ) {
                    Text(if (replayState == 2L) "Play" else "Pause", color = Color.White)
                }
                Button(
                    onClick = { PlayerActivity.current?.goLive() },
                    enabled = replayState != 0L,
                ) {
                    Text("Live", color = Color.White)
                }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = {
                        keyflag = !keyflag
                        prefs.edit().putString("hwexp", hwexpStringFor(keyflag, recsei)).apply()
                    },
                ) {
                    Text(if (keyflag) "keyflag •" else "keyflag", color = Color.White)
                }
                Button(
                    onClick = {
                        recsei = !recsei
                        prefs.edit().putString("hwexp", hwexpStringFor(keyflag, recsei)).apply()
                    },
                ) {
                    Text(if (recsei) "recsei •" else "recsei", color = Color.White)
                }
                // Hands and objects in front of the video panel hide it (Depth API). Costs some latency; see stats.
                Button(
                    onClick = {
                        occlusion = !occlusion
                        PlayerActivity.current?.setOcclusion(occlusion)
                            ?: prefs.edit().putBoolean("occlusion", occlusion).apply()
                    },
                ) {
                    Text(if (occlusion) "occlusion •" else "occlusion", color = Color.White)
                }
                // Screen follows the headset (always straight ahead, like the goggles) vs. stays in the room.
                Button(
                    onClick = {
                        headLock = !headLock
                        PlayerActivity.current?.setHeadLock(headLock)
                    },
                ) {
                    Text(if (headLock) "head lock •" else "head lock", color = Color.White)
                }
            }

            // Screen geometry: width (m), curve (0 = flat, 100 % = wrapped around the head) and two fit presets that
            // size and curve the screen so the image's diagonal spans 100 deg (whole image inside the Quest's view)
            // or 155 deg (the camera lens, natural angles). A preset overrides size and curve while on.
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = {
                        // Steps the visible size (includes any two-handed scale).
                        PlayerActivity.current?.stepScreenSize(1f / 1.25f)
                    },
                    enabled = fitDeg == 0f,
                ) {
                    Text("Size −", color = Color.White)
                }
                Button(
                    onClick = {
                        PlayerActivity.current?.stepScreenSize(1.25f)
                    },
                    enabled = fitDeg == 0f,
                ) {
                    Text("Size +", color = Color.White)
                }
                Button(
                    onClick = {
                        screenCurve = if (screenCurve >= 0.99f) 0f else ((screenCurve * 4f).toInt() + 1) / 4f
                        PlayerActivity.current?.setScreen(curve = screenCurve)
                    },
                    enabled = fitDeg == 0f,
                ) {
                    Text("Curve ${(screenCurve * 100).toInt()} %", color = Color.White)
                }
                Button(
                    onClick = {
                        fitDeg = if (fitDeg == 100f) 0f else 100f
                        PlayerActivity.current?.setScreen(fit = fitDeg)
                    },
                ) {
                    Text(if (fitDeg == 100f) "100° •" else "100°", color = Color.White)
                }
                Button(
                    onClick = {
                        fitDeg = if (fitDeg == 155f) 0f else 155f
                        PlayerActivity.current?.setScreen(fit = fitDeg)
                    },
                ) {
                    Text(if (fitDeg == 155f) "155° lens •" else "155° lens", color = Color.White)
                }
            }
            Text(
                when {
                    fitDeg == 155f && headLock -> "Screen: 155° lens, limited to 100° while head-locked"
                    fitDeg > 0f -> "Screen: fitted to %.0f°".format(fitDeg)
                    else -> "Screen: %.1f m wide, curve %d %%".format(screenWidth, (screenCurve * 100).toInt())
                },
                color = Color.White,
            )

            Button(
                modifier = Modifier.width(220.dp),
                onClick = {
                    val activity = PlayerActivity.current
                    if (connected) {
                        activity?.stopNetwork() ?: NativePlayer.stop()
                        activity?.stopUsb()
                        connected = false
                        status = "Disconnected"
                    } else {
                        if (source == "network") {
                            // Prefer the relay discovery hears; fall back to the last address used.
                            val relay = RelayDiscovery.current()
                            val target = relay?.host ?: host.trim()
                            val port = relay?.port ?: portText.trim().toIntOrNull()
                            if (target.isBlank() || port == null || port <= 0 || port > 65535) {
                                status = "No relay found: start QuestVisor-Relay on the PC"
                            } else if (activity != null) {
                                activity.startNetwork(target, port)
                                connected = true
                                status = "Connecting"
                            }
                        } else {
                            prefs.edit().putString("source", "usb").apply()
                            activity?.connectGoggles(h265Decoder, h264Decoder, decoderMode, hwexpString)
                            connected = true
                            status = "Connecting USB"
                        }
                    }
                },
            ) {
                Text(if (connected) "Disconnect" else "Connect", color = Color.White)
            }

            TextButton(
                onClick = {
                    host = context.getString(R.string.default_host)
                    portText = context.getString(R.string.default_port)
                },
            ) {
                Text("Reset defaults", color = Color.White)
            }

            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                SpatialSwitch(
                    checked = showStats,
                    onCheckedChange = {
                        showStats = it
                        prefs.edit().putBoolean("showStats", it).apply()
                    },
                )
                Text("Show stats", color = Color.White)
            }

        }

        // Stats in their own right-hand column (they were clipped below the controls).
        if (showStats && statusText.isNotBlank()) {
            Column(
                modifier = Modifier.weight(1f).fillMaxHeight()
                    .verticalScroll(androidx.compose.foundation.rememberScrollState()),
            ) {
                Text(
                    text = statusText,
                    color = Color.White,
                    fontSize = 13.sp,
                    lineHeight = 17.sp,
                    fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace,
                )
            }
        }
      }
    }
}


private fun formatSpan(ms: Long): String {
    val total = (ms.coerceAtLeast(0L) / 1000L)
    return "%d:%02d".format(total / 60, total % 60)
}

private fun hwexpContains(hwexp: String?, token: String): Boolean {
    return (hwexp ?: "").split(",").any { it.trim().lowercase() == token }
}

// Small panel under the video screen: shows / hides the settings panel (which then appears in front of the user).
@Composable
fun SettingsToggleButton() {
    var shown by remember { mutableStateOf(PlayerActivity.current?.settingsVisible ?: true) }
    // The app also closes the settings by itself (while scaling the screen), so follow its state.
    LaunchedEffect(Unit) {
        while (true) {
            PlayerActivity.current?.settingsVisible?.let { if (it != shown) shown = it }
            delay(200L)
        }
    }
    // A small round cog in the screen's lower-right corner; brighter while the settings are open.
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(if (shown) 0xE6FFFFFF else 0x99000000), androidx.compose.foundation.shape.CircleShape)
            .clickable {
                shown = !shown
                PlayerActivity.current?.setSettingsVisible(shown)
            },
        contentAlignment = Alignment.Center,
    ) {
        androidx.compose.material3.Icon(
            imageVector = Icons.Filled.Settings,
            contentDescription = if (shown) "Hide settings" else "Settings",
            tint = if (shown) Color.Black else Color.White,
            modifier = Modifier.fillMaxSize().padding(10.dp),
        )
    }
}

private fun hwexpStringFor(keyflag: Boolean, recsei: Boolean): String {
    return listOfNotNull(
        if (keyflag) "keyflag" else null,
        if (recsei) "recsei" else null,
    ).joinToString(",")
}

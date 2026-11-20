// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE (GPL-3.0 with an additional permission
// to combine with the Meta Spatial SDK).
package com.xerx.questvisor

import android.util.Log
import android.view.Surface

class NativePlayer private constructor() {
    companion object {
        init {
            tryLoad("questvisor")
        }

        private fun tryLoad(name: String) {
            try {
                System.loadLibrary(name)
            } catch (t: Throwable) {
                Log.e("QuestVisor", "load $name failed", t)
            }
        }

        @JvmStatic
        external fun setSurface(surface: Surface?)

        @JvmStatic
        external fun addFakeIdr(
            codec: String,
            width: Int,
            height: Int,
            data: ByteArray,
        )

        @JvmStatic
        external fun addFakeIdrWithSidecar(
            codec: String,
            width: Int,
            height: Int,
            data: ByteArray,
            sidecarJson: String?,
        )

        @JvmStatic
        external fun startNetwork(
            host: String,
            port: Int,
            decoderNameH265: String,
            decoderNameH264: String,
            decoderMode: String,
            hwexp: String,
        )

        @JvmStatic
        external fun startUsb(
            fd: Int,
            ctrlIf: Int,
            epIn: Int,
            epOut: Int,
            decoderNameH265: String,
            decoderNameH264: String,
            decoderMode: String,
            hwexp: String,
        )

        @JvmStatic
        external fun startSelftest(
            path: String,
            decoderNameH265: String,
            decoderNameH264: String,
            decoderMode: String,
            hwexp: String,
        )

        @JvmStatic
        external fun stop()

        /** Replay buffer: keep the last [seconds] of received video in [dir]; 0 = off (deletes it). */
        @JvmStatic
        external fun setReplayBuffer(dir: String, seconds: Int)

        /** Plays the replay buffer from wall-clock [startMs] (stops live video; recording pauses meanwhile). */
        @JvmStatic
        external fun startReplay(
            startMs: Long,
            decoderNameH265: String,
            decoderNameH264: String,
            decoderMode: String,
            hwexp: String,
        )

        @JvmStatic
        external fun setReplayPaused(paused: Boolean)

        /** {state 0 live/none, 1 playing, 2 paused, 3 end; position ms; buffer start ms; buffer end ms}. */
        @JvmStatic
        external fun replayInfo(): LongArray

        @JvmStatic
        external fun stats(): LongArray

        @JvmStatic
        external fun uiState(): Int

        @JvmStatic
        external fun statusText(): String

        @JvmStatic
        external fun decoderName(): String
    }
}

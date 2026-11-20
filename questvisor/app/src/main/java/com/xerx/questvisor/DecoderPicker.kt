// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE (GPL-3.0 with an additional permission
// to combine with the Meta Spatial SDK).
package com.xerx.questvisor

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.os.Build
import android.util.Log

object DecoderPicker {
    private const val TAG = "QuestVisor"

    private data class Candidate(
        val name: String,
        val mime: String,
        val hardware: Boolean,
        val lowLatency: Boolean,
        val c2Qti: Boolean,
        val supportsStream: Boolean,
        val maxWidth: Int,
        val maxHeight: Int,
        val maxFrameRate: Double,
        val maxInstances: Int,
    )

    private fun supportedTypes(info: MediaCodecInfo): List<String> {
        return try {
            val value = info.javaClass.getMethod("getSupportedTypes").invoke(info)
            val out = mutableListOf<String>()
            if (value is Array<*>) {
                for (item in value) {
                    if (item is String) {
                        out.add(item)
                    }
                }
            }
            out
        } catch (t: Throwable) {
            emptyList()
        }
    }

    private fun videoCapabilities(caps: MediaCodecInfo.CodecCapabilities): Any? {
        return try {
            caps.javaClass.getMethod("getVideoCapabilities").invoke(caps)
        } catch (t: Throwable) {
            null
        }
    }

    private fun intProp(obj: Any?, name: String): Int {
        if (obj == null) {
            return -1
        }

        return try {
            val value = obj.javaClass.getMethod(name).invoke(obj)
            (value as? Number)?.toInt() ?: -1
        } catch (t: Throwable) {
            -1
        }
    }

    private fun floatProp(obj: Any?, name: String): Float {
        if (obj == null) {
            return -1f
        }

        return try {
            val value = obj.javaClass.getMethod(name).invoke(obj)
            (value as? Number)?.toFloat() ?: -1f
        } catch (t: Throwable) {
            -1f
        }
    }

    private fun areSizeAndRateSupported(obj: Any?, width: Int, height: Int, frameRate: Float): Boolean {
        if (obj == null) {
            return false
        }

        return try {
            val method = obj.javaClass.getMethod(
                "areSizeAndRateSupported",
                java.lang.Integer.TYPE,
                java.lang.Integer.TYPE,
                java.lang.Float.TYPE,
            )
            method.invoke(obj, width, height, frameRate) as? Boolean ?: false
        } catch (t: Throwable) {
            false
        }
    }

    private fun maxInstances(info: MediaCodecInfo): Int {
        return try {
            val value = info.javaClass.getMethod("getMaxSupportedInstances").invoke(info)
            (value as? Number)?.toInt() ?: -1
        } catch (t: Throwable) {
            -1
        }
    }

    private fun logVendorParameters(name: String) {
        if (name.isBlank()) {
            return
        }

        try {
            val codec = MediaCodec.createByCodecName(name)
            try {
                val names = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    try {
                        codec.supportedVendorParameters?.toList() ?: emptyList<String>()
                    } catch (t: Throwable) {
                        Log.w(TAG, "supportedVendorParameters failed for $name", t)
                        emptyList<String>()
                    }
                } else {
                    emptyList<String>()
                }

                Log.i(TAG, "vendor params $name: ${names.joinToString(", ")}")
            } finally {
                try {
                    codec.release()
                } catch (_: Throwable) {
                }
            }
        } catch (t: Throwable) {
            Log.w(TAG, "vendor params probe failed for $name", t)
        }
    }

    fun pickDecoder(mime: String): String {
        val list = MediaCodecList(MediaCodecList.REGULAR_CODECS)
        val candidates = mutableListOf<Candidate>()
        val codecInfos = list.getCodecInfos() ?: emptyArray<MediaCodecInfo>()

        for (info in codecInfos) {
            val types = supportedTypes(info)
            if (!types.any { it.equals(mime, ignoreCase = true) }) {
                continue
            }

            val name = info.getName() ?: ""
            var lowLatency = false
            var maxWidth = -1
            var maxHeight = -1
            var maxFrameRate = -1.0
            var supportsStream = false
            var maxInstances = -1

            try {
                val caps = info.getCapabilitiesForType(mime)
                if (caps != null) {
                    lowLatency = try {
                        caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
                    } catch (t: Throwable) {
                        false
                    }

                    val video = videoCapabilities(caps)
                    if (video != null) {
                        maxWidth = intProp(video, "getMaxWidth")
                        maxHeight = intProp(video, "getMaxHeight")
                        maxFrameRate = floatProp(video, "getMaxFrameRate").toDouble()
                        supportsStream = areSizeAndRateSupported(video, 1832, 1032, 60f)
                    }
                }
            } catch (t: Throwable) {
                Log.w(TAG, "getCapabilitiesForType failed for $name", t)
            }

            maxInstances = maxInstances(info)

            val hardware = try {
                !info.isSoftwareOnly()
            } catch (t: Throwable) {
                true
            }

            val candidate = Candidate(
                name = name,
                mime = mime,
                hardware = hardware,
                lowLatency = lowLatency,
                c2Qti = name.startsWith("c2.qti."),
                supportsStream = supportsStream,
                maxWidth = maxWidth,
                maxHeight = maxHeight,
                maxFrameRate = maxFrameRate,
                maxInstances = maxInstances,
            )

            Log.i(
                TAG,
                "decoder candidate name=$name mime=$mime hardware=${candidate.hardware} " +
                    "lowLatency=${candidate.lowLatency} c2Qti=${candidate.c2Qti} " +
                    "maxSize=${candidate.maxWidth}x${candidate.maxHeight} maxRate=${candidate.maxFrameRate} " +
                    "maxInstances=${candidate.maxInstances}",
            )

            candidates.add(candidate)
        }

        val hardwareCandidates = candidates.filter { it.hardware }
        val selectedPool = if (hardwareCandidates.isNotEmpty()) hardwareCandidates else candidates

        if (selectedPool.isEmpty()) {
            Log.w(TAG, "no decoder candidate for $mime")
            return ""
        }

        val chosen = selectedPool.sortedWith(
            compareByDescending<Candidate> { it.lowLatency }
                .thenByDescending { it.c2Qti }
                .thenByDescending { it.supportsStream }
                .thenByDescending { it.hardware }
                .thenBy { it.name },
        ).first()

        Log.i(TAG, "chosen decoder for $mime: ${chosen.name}")
        logVendorParameters(chosen.name)
        return chosen.name
    }
}

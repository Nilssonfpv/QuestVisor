// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE (GPL-3.0 with an additional permission
// to combine with the Meta Spatial SDK).
package com.xerx.questvisor

import android.content.Context
import android.net.wifi.WifiManager
import android.os.SystemClock
import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.SocketTimeoutException

/**
 * Finds the QuestVisor relay on the local network. The relay broadcasts
 * "QVISOR1 tcp=<port> clock=<port> name=<pc>" once a second on UDP 9202 (simple_forwarder.py, discovery_beacon).
 */
object RelayDiscovery {
    const val PORT = 9202
    private const val TAG = "QuestVisor"

    data class Relay(val host: String, val port: Int, val name: String, val seenAtMs: Long)

    @Volatile var latest: Relay? = null
        private set

    @Volatile private var thread: Thread? = null
    private var lock: WifiManager.MulticastLock? = null

    /** The relay heard within [maxAgeMs], or null. */
    fun current(maxAgeMs: Long = 5000): Relay? =
        latest?.takeIf { SystemClock.elapsedRealtime() - it.seenAtMs <= maxAgeMs }

    fun start(context: Context) {
        if (thread != null) return
        try {
            // Some Wi-Fi drivers drop broadcast packets for apps that don't hold this lock.
            val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            lock = wifi.createMulticastLock("questvisor-discovery").apply {
                setReferenceCounted(false)
                acquire()
            }
        } catch (t: Throwable) {
            Log.w(TAG, "discovery: multicast lock unavailable: $t")
        }
        val t = Thread({ run() }, "relay-discovery")
        t.isDaemon = true
        thread = t
        t.start()
    }

    fun stop() {
        thread?.interrupt()
        thread = null
        try { lock?.release() } catch (_: Throwable) {}
        lock = null
    }

    private fun run() {
        val socket = try {
            DatagramSocket(null).apply {
                reuseAddress = true
                broadcast = true
                soTimeout = 1000
                bind(InetSocketAddress(PORT))
            }
        } catch (t: Throwable) {
            Log.w(TAG, "discovery: cannot listen on UDP $PORT: $t")
            thread = null
            return
        }
        Log.i(TAG, "discovery: listening on UDP $PORT")
        val buf = ByteArray(256)
        socket.use {
            while (!Thread.currentThread().isInterrupted) {
                val packet = DatagramPacket(buf, buf.size)
                try {
                    socket.receive(packet)
                } catch (_: SocketTimeoutException) {
                    continue
                } catch (t: Throwable) {
                    if (Thread.currentThread().isInterrupted) break
                    Log.w(TAG, "discovery: receive failed: $t")
                    Thread.sleep(1000)
                    continue
                }
                val text = String(packet.data, packet.offset, packet.length, Charsets.UTF_8)
                val relay = parse(text, packet.address.hostAddress ?: continue) ?: continue
                val previous = latest
                latest = relay
                if (previous == null || previous.host != relay.host || previous.port != relay.port) {
                    Log.i(TAG, "discovery: relay '${relay.name}' at ${relay.host}:${relay.port}")
                }
            }
        }
    }

    private fun parse(text: String, host: String): Relay? {
        val parts = text.trim().split(' ')
        if (parts.firstOrNull() != "QVISOR1") return null
        val fields = parts.drop(1).mapNotNull {
            val i = it.indexOf('=')
            if (i > 0) it.substring(0, i) to it.substring(i + 1) else null
        }.toMap()
        val port = fields["tcp"]?.toIntOrNull()?.takeIf { it in 1..65535 } ?: return null
        if (host.contains(':')) return null          // the native player takes IPv4 literals only
        return Relay(host, port, fields["name"] ?: host, SystemClock.elapsedRealtime())
    }
}

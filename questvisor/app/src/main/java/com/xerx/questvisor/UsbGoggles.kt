// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE (GPL-3.0 with an additional permission
// to combine with the Meta Spatial SDK).
package com.xerx.questvisor

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.util.Log
import java.net.InetSocketAddress
import java.net.Socket

class UsbGoggles {
    companion object {
        const val TAG = "QuestVisor"
        const val ACTION_USB_PERMISSION = "com.xerx.questvisor.USB_PERMISSION"

        // The Goggles 3 enumerate as this composite device (MI_00 = RNDIS, MI_02 = mass storage,
        // MI_03..MI_07 = vendor), so class-only matching would also accept an unrelated gadget.
        const val DJI_VENDOR_ID = 0x2CA3
        const val DJI_PRODUCT_ID = 0x0020

        @JvmStatic
        fun tryNetwork(host: String, port: Int, timeoutMs: Int): Boolean {
            return try {
                Socket().use { socket ->
                    socket.connect(InetSocketAddress(host, port), timeoutMs)
                    socket.close()
                }
                true
            } catch (_: Throwable) {
                false
            }
        }
    }

    private var receiver: BroadcastReceiver? = null
    private var pendingActivity: Activity? = null
    private var pendingDevice: UsbDevice? = null
    private var pendingH265 = ""
    private var pendingH264 = ""
    private var pendingDecoderMode = "auto"
    private var pendingHwExp = ""

    private var usbManager: UsbManager? = null
    private var connection: UsbDeviceConnection? = null
    private var claimedInterfaces: List<UsbInterface> = emptyList()
    private var hasOpen = false

    @Volatile
    var status: String = "USB idle"

    // The panel only shows the native player's status, so every Java-side stage must reach logcat
    // too - otherwise a failure before the native thread starts is invisible.
    private fun setUsbStatus(text: String, warn: Boolean = false) {
        status = text
        if (warn) {
            Log.w(TAG, text)
        } else {
            Log.i(TAG, text)
        }
    }

    private fun interfaceLabel(intf: UsbInterface): String {
        val endpoints = (0 until intf.endpointCount).joinToString(",") { j ->
            val ep = intf.getEndpoint(j)
            val type = when (ep.type) {
                UsbConstants.USB_ENDPOINT_XFER_BULK -> "bulk"
                UsbConstants.USB_ENDPOINT_XFER_INT -> "int"
                UsbConstants.USB_ENDPOINT_XFER_ISOC -> "isoc"
                else -> "ep${ep.type}"
            }
            "0x%02x:$type:%s".format(ep.address, if (ep.direction == UsbConstants.USB_DIR_IN) "in" else "out")
        }
        return "if${intf.id}(cls=${intf.interfaceClass} sub=${intf.interfaceSubclass} " +
            "proto=${intf.interfaceProtocol} alt=${intf.alternateSetting} eps=$endpoints)"
    }

    private fun isRndisControl(intf: UsbInterface): Boolean {
        return intf.interfaceClass == 0xE0 ||
            (intf.interfaceClass == 0x02 && intf.interfaceSubclass == 0x02)
    }

    private fun isRndisData(intf: UsbInterface): Boolean {
        return intf.interfaceClass == 0x0A
    }

    fun register(activity: Activity) {
        if (receiver != null) {
            return
        }

        usbManager = activity.getSystemService(Context.USB_SERVICE) as? UsbManager

        receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                if (intent.action != ACTION_USB_PERMISSION) {
                    return
                }

                val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                val device = intent.extras?.getParcelable(
                    UsbManager.EXTRA_DEVICE,
                    UsbDevice::class.java,
                )

                if (!granted) {
                    setUsbStatus("USB: permission denied", warn = true)
                    return
                }

                if (device == null || pendingDevice == null || device != pendingDevice) {
                    setUsbStatus("USB: permission granted for wrong device", warn = true)
                    return
                }

                val activity = pendingActivity ?: (context as? Activity) ?: return
                open(activity, device)
            }
        }

        try {
            activity.registerReceiver(
                receiver,
                IntentFilter(ACTION_USB_PERMISSION),
                Context.RECEIVER_NOT_EXPORTED,
            )
        } catch (t: Throwable) {
            activity.registerReceiver(receiver, IntentFilter(ACTION_USB_PERMISSION))
        }
    }

    fun unregister() {
        try {
            val activity = pendingActivity
            val recv = receiver
            if (activity != null && recv != null) {
                activity.unregisterReceiver(recv)
            }
        } catch (_: Throwable) {
        }

        receiver = null
        pendingActivity = null
    }

    fun requestConnection(
        activity: Activity,
        h265Decoder: String,
        h264Decoder: String,
        decoderMode: String,
        hwexp: String,
    ) {
        if (usbManager == null) {
            usbManager = activity.getSystemService(Context.USB_SERVICE) as? UsbManager
        }

        if (hasOpen) {
            setUsbStatus("USB: already open")
            return
        }

        pendingActivity = activity
        pendingH265 = h265Decoder
        pendingH264 = h264Decoder
        pendingDecoderMode = decoderMode
        pendingHwExp = hwexp

        val device = findGogglesDevice()
        if (device == null) {
            setUsbStatus("USB: device not found", warn = true)
            return
        }

        pendingDevice = device

        val manager = usbManager ?: run {
            setUsbStatus("USB: manager missing", warn = true)
            return
        }
        if (!manager.hasPermission(device)) {
            val intent = Intent(ACTION_USB_PERMISSION).setPackage(activity.packageName)
            intent.putExtra(UsbManager.EXTRA_DEVICE, device)

            val pendingIntent = PendingIntent.getBroadcast(
                activity,
                0,
                intent,
                PendingIntent.FLAG_MUTABLE,
            )

            manager.requestPermission(device, pendingIntent)
            setUsbStatus("USB: permission requested")
            return
        }

        open(activity, device)
    }

    fun stop() {
        try {
            connection?.let { conn ->
                for (intf in claimedInterfaces) {
                    try {
                        conn.releaseInterface(intf)
                    } catch (_: Throwable) {
                    }
                }
            }
        } catch (_: Throwable) {
        }

        try {
            connection?.close()
        } catch (_: Throwable) {
        }

        connection = null
        claimedInterfaces = emptyList()
        hasOpen = false
        setUsbStatus("USB stopped")
    }

    private fun findGogglesDevice(): UsbDevice? {
        val manager = usbManager ?: return null
        val devices = manager.deviceList.values

        if (devices.isEmpty()) {
            setUsbStatus("USB: no devices on the port")
            return null
        }

        var fallback: UsbDevice? = null

        for (device in devices) {
            var hasCtrl = false
            var hasData = false
            var hasBulkIn = false
            var hasBulkOut = false

            val ifaces = (0 until device.interfaceCount).map { device.getInterface(it) }

            for (intf in ifaces) {
                if (isRndisControl(intf)) {
                    hasCtrl = true
                }
                if (isRndisData(intf)) {
                    hasData = true
                }

                for (j in 0 until intf.endpointCount) {
                    val ep = intf.getEndpoint(j)
                    if (ep.type == UsbConstants.USB_ENDPOINT_XFER_BULK) {
                        if (ep.direction == UsbConstants.USB_DIR_IN) {
                            hasBulkIn = true
                        } else {
                            hasBulkOut = true
                        }
                    }
                }
            }

            val matches = (hasCtrl || hasData) && hasBulkIn && hasBulkOut

            // Log what is actually on the port, so "not found" says more than "not found".
            setUsbStatus(
                "USB: dev %04x:%04x serial=%s match=%s %s".format(
                    device.vendorId,
                    device.productId,
                    device.serialNumber ?: "-",
                    matches,
                    ifaces.joinToString(" ") { interfaceLabel(it) },
                )
            )

            if (!matches) {
                continue
            }

            if (device.vendorId == DJI_VENDOR_ID && device.productId == DJI_PRODUCT_ID) {
                return device
            }
            if (fallback == null) {
                fallback = device
            }
        }

        return fallback
    }

    private fun open(activity: Activity, device: UsbDevice) {
        val manager = usbManager ?: run {
            setUsbStatus("USB: manager missing", warn = true)
            return
        }

        val conn = manager.openDevice(device)
        if (conn == null) {
            setUsbStatus("USB: open failed", warn = true)
            return
        }

        connection = conn
        hasOpen = true

        var ctrlIf = -1
        var ctrlIntf: UsbInterface? = null
        var dataIf = -1
        var dataIntf: UsbInterface? = null
        var epIn = -1
        var epOut = -1
        var epNotify = -1
        val claimed = mutableListOf<UsbInterface>()

        for (i in 0 until device.interfaceCount) {
            val intf = device.getInterface(i)
            val isCtrl = isRndisControl(intf)
            val isData = isRndisData(intf)

            if (!isCtrl && !isData) {
                // The mass storage and vendor interfaces of this composite device have bulk
                // endpoints too; touching them would hijack the RNDIS pipes.
                continue
            }

            if (!conn.claimInterface(intf, true)) {
                claimedInterfaces = claimed
                setUsbStatus("USB: claim failed for ${interfaceLabel(intf)}", warn = true)
                stop()
                return
            }

            claimed.add(intf)

            if (isCtrl) {
                ctrlIf = intf.id
                ctrlIntf = intf
            }
            if (isData) {
                dataIf = intf.id
                dataIntf = intf

                // A CDC/RNDIS data interface sits at alternate setting 0 with no bandwidth, so its
                // bulk endpoints are unusable until the active setting is switched on.
                val activated = try {
                    conn.setInterface(intf)
                } catch (_: Throwable) {
                    false
                }
                setUsbStatus("USB: data interface active=$activated ${interfaceLabel(intf)}")
            }

            // Endpoints come from the interface that owns them: the data interface has the bulk
            // pipes, the control interface has the RNDIS notification endpoint.
            for (j in 0 until intf.endpointCount) {
                val ep = intf.getEndpoint(j)
                if (ep.type == UsbConstants.USB_ENDPOINT_XFER_BULK) {
                    if (ep.direction == UsbConstants.USB_DIR_IN && epIn < 0) {
                        epIn = ep.address
                    } else if (ep.direction == UsbConstants.USB_DIR_OUT && epOut < 0) {
                        epOut = ep.address
                    }
                } else if (ep.type == UsbConstants.USB_ENDPOINT_XFER_INT &&
                    ep.direction == UsbConstants.USB_DIR_IN && epNotify < 0
                ) {
                    epNotify = ep.address
                }
            }
        }

        claimedInterfaces = claimed

        if (ctrlIf < 0) {
            setUsbStatus("USB: no RNDIS control interface on %04x:%04x".format(device.vendorId, device.productId), warn = true)
            stop()
            return
        }

        if (epIn < 0 || epOut < 0) {
            setUsbStatus(
                "USB: no bulk endpoints (data=${dataIntf?.let { interfaceLabel(it) } ?: "none"} " +
                    "ctrl=${ctrlIntf?.let { interfaceLabel(it) } ?: "none"})",
                warn = true,
            )
            stop()
            return
        }

        setUsbStatus(
            "USB: opening RNDIS fd=${conn.fileDescriptor} ctrl=$ctrlIf data=$dataIf " +
                "epIn=0x%02x epOut=0x%02x notify=%s".format(
                    epIn,
                    epOut,
                    if (epNotify < 0) "none" else "0x%02x".format(epNotify),
                )
        )

        NativePlayer.startUsb(
            conn.fileDescriptor,
            ctrlIf,
            epIn,
            epOut,
            pendingH265,
            pendingH264,
            pendingDecoderMode,
            pendingHwExp,
        )
    }
}

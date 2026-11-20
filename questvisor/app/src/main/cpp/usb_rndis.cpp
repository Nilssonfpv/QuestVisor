// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE (GPL-3.0 with an additional permission
// to combine with the Meta Spatial SDK).
#include "usb_rndis.h"

#include <android/log.h>
#include <algorithm>
#include <cstdio>
#include <errno.h>
#include <cstring>
#include <chrono>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/usbdevice_fs.h>

#define LOG_TAG "QuestVisor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
    constexpr uint32_t RNDIS_INITIALIZE_MSG = 0x00000002;
    constexpr uint32_t RNDIS_INITIALIZE_CMPLT = 0x80000002;
    constexpr uint32_t RNDIS_QUERY_MSG = 0x40010101;
    constexpr uint32_t RNDIS_QUERY_CMPLT = 0xC0010101;
    constexpr uint32_t RNDIS_SET_MSG = 0x00010101;
    constexpr uint32_t RNDIS_SET_CMPLT = 0x80010101;
    constexpr uint32_t RNDIS_PACKET_MSG = 0x00000001;

    constexpr uint32_t OID_802_3_PERMANENT_ADDRESS = 0x01010101;
    constexpr uint32_t OID_GEN_CURRENT_PACKET_FILTER = 0x0001010E;
}

UsbRndis::UsbRndis(int fd, int ctrlIf, int epIn, int epOut)
    : fd_(fd), ctrlIf_(ctrlIf), epIn_(epIn), epOut_(epOut) {}

UsbRndis::~UsbRndis() {}

uint32_t UsbRndis::get32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
        (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) |
        (static_cast<uint32_t>(p[3]) << 24);
}

void UsbRndis::put32le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

bool UsbRndis::controlSend(const uint8_t* data, uint32_t len) {
    struct usbdevfs_ctrltransfer ctrl {};
    ctrl.bRequestType = 0x21;
    ctrl.bRequest = 0x00;
    ctrl.wValue = 0;
    ctrl.wIndex = static_cast<uint16_t>(ctrlIf_);
    ctrl.wLength = static_cast<uint16_t>(len);
    ctrl.timeout = 1000;
    ctrl.data = const_cast<uint8_t*>(data);

    const int rc = ioctl(fd_, USBDEVFS_CONTROL, &ctrl);
    if (rc < 0) {
        LOGW("USBDEVFS_CONTROL send failed: %s", std::strerror(errno));
        status_ = std::string("RNDIS control send failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool UsbRndis::controlRead(std::vector<uint8_t>& resp) {
    resp.assign(4096, 0);

    struct usbdevfs_ctrltransfer ctrl {};
    ctrl.bRequestType = 0xa1;
    ctrl.bRequest = 0x01;
    ctrl.wValue = 0;
    ctrl.wIndex = static_cast<uint16_t>(ctrlIf_);
    ctrl.wLength = 4096;
    ctrl.timeout = 1000;
    ctrl.data = resp.data();

    const int rc = ioctl(fd_, USBDEVFS_CONTROL, &ctrl);
    if (rc < 0) {
        LOGW("USBDEVFS_CONTROL read failed: %s", std::strerror(errno));
        status_ = std::string("RNDIS control read failed: ") + std::strerror(errno);
        return false;
    }

    size_t len = resp.size();
    if (rc > 0 && static_cast<size_t>(rc) < len) {
        len = static_cast<size_t>(rc);
    }

    if (len >= 8) {
        const uint32_t msgLen = get32le(resp.data() + 4);
        if (msgLen > 0 && msgLen <= len) {
            len = msgLen;
        }
    }

    resp.resize(len);
    return true;
}

bool UsbRndis::sendEncapsulated(uint8_t* data, uint32_t len, uint32_t expectedType, std::vector<uint8_t>& resp) {
    if (!controlSend(data, len)) {
        return false;
    }
    if (!controlRead(resp)) {
        return false;
    }
    if (resp.size() < 8) {
        status_ = "RNDIS short response";
        return false;
    }

    const uint32_t type = get32le(resp.data());
    if (type != expectedType) {
        LOGW("unexpected RNDIS response type 0x%08x, wanted 0x%08x", type, expectedType);
        status_ = "RNDIS unexpected response";
        return false;
    }
    return true;
}

bool UsbRndis::initialize() {
    uint8_t initMsg[28] {};
    put32le(initMsg + 0, RNDIS_INITIALIZE_MSG);
    put32le(initMsg + 4, 28);
    put32le(initMsg + 8, requestId_++);
    put32le(initMsg + 12, 0x01000000);
    put32le(initMsg + 16, 0x01000000);
    put32le(initMsg + 20, 16384);
    put32le(initMsg + 24, 0x00000001);

    std::vector<uint8_t> resp;
    if (!sendEncapsulated(initMsg, sizeof(initMsg), RNDIS_INITIALIZE_CMPLT, resp)) {
        return false;
    }

    if (resp.size() >= 28) {
        const uint32_t status = get32le(resp.data() + 12);
        if (status != 0) {
            LOGW("RNDIS initialize status=%u", status);
            status_ = "RNDIS initialize failed";
            return false;
        }

        maxTransferSize_ = get32le(resp.data() + 24);
        if (maxTransferSize_ < 512 || maxTransferSize_ > 65536) {
            maxTransferSize_ = 16384;
        }

        packetAlignment_ = 1;  // RNDIS over Ethernet is byte-aligned; the INIT reply has no alignment field
        if (packetAlignment_ == 0) {
            packetAlignment_ = 1;
        }
    }

    LOGI("RNDIS init ok maxTransferSize=%u alignment=%u", maxTransferSize_, packetAlignment_);
    return true;
}

bool UsbRndis::queryPermanentAddress() {
    uint8_t query[24] {};
    put32le(query + 0, RNDIS_QUERY_MSG);
    put32le(query + 4, 24);
    put32le(query + 8, requestId_++);
    put32le(query + 12, OID_802_3_PERMANENT_ADDRESS);
    put32le(query + 16, 0);
    put32le(query + 20, 24);

    std::vector<uint8_t> resp;
    if (!sendEncapsulated(query, sizeof(query), RNDIS_QUERY_CMPLT, resp)) {
        return false;
    }

    if (resp.size() < 32) {
        LOGW("RNDIS query response too short: %zu", resp.size());
        return false;
    }

    uint32_t infoLen = get32le(resp.data() + 24);
    uint32_t infoOff = get32le(resp.data() + 28);
    if (infoOff < 24) {
        infoOff = 24;
    }
    if (static_cast<size_t>(infoOff + infoLen) > resp.size()) {
        infoOff = 24;
    }
    if (static_cast<size_t>(infoOff + 6) <= resp.size() && infoLen >= 6) {
        std::memcpy(peerMac_, resp.data() + infoOff, 6);
        LOGI("RNDIS permanent MAC %s", peerMacHex().c_str());
        return true;
    }

    LOGW("RNDIS permanent address not found infoOff=%u infoLen=%u len=%zu",
         infoOff, infoLen, resp.size());
    return false;
}

bool UsbRndis::setPacketFilter() {
    uint8_t setMsg[28] {};
    put32le(setMsg + 0, RNDIS_SET_MSG);
    put32le(setMsg + 4, 28);
    put32le(setMsg + 8, requestId_++);
    put32le(setMsg + 12, OID_GEN_CURRENT_PACKET_FILTER);
    put32le(setMsg + 16, 4);
    put32le(setMsg + 20, 24);
    put32le(setMsg + 24, 0x0000000B);

    std::vector<uint8_t> resp;
    if (!sendEncapsulated(setMsg, sizeof(setMsg), RNDIS_SET_CMPLT, resp)) {
        return false;
    }

    if (resp.size() >= 16) {
        const uint32_t status = get32le(resp.data() + 12);
        if (status != 0) {
            LOGW("RNDIS set filter status=%u", status);
            status_ = "RNDIS set filter failed";
            return false;
        }
    }

    LOGI("RNDIS packet filter set");
    return true;
}

bool UsbRndis::init() {
    if (fd_ < 0) {
        status_ = "USB fd invalid";
        return false;
    }

    if (!initialize()) {
        initialized_ = false;
        return false;
    }

    if (!queryPermanentAddress()) {
        status_ = "RNDIS ok (MAC unknown)";
        LOGW("could not query permanent MAC, continuing");
    } else {
        status_ = "RNDIS ok " + peerMacHex();
    }

    if (!setPacketFilter()) {
        initialized_ = false;
        return false;
    }

    initialized_ = true;
    return true;
}

std::string UsbRndis::peerMacHex() const {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                  peerMac_[0], peerMac_[1], peerMac_[2], peerMac_[3], peerMac_[4], peerMac_[5]);
    return std::string(buf);
}

std::string UsbRndis::statusString() const {
    return status_;
}

int UsbRndis::readFrames(int timeoutMs, const std::function<void(const uint8_t*, size_t, int64_t)>& onFrame) {
    if (!initialized_) {
        return -1;
    }

    std::vector<uint8_t> rx(std::max<uint32_t>(maxTransferSize_, 4096));

    struct usbdevfs_bulktransfer bulk {};
    bulk.ep = static_cast<unsigned>(epIn_);
    bulk.len = static_cast<unsigned>(rx.size());
    bulk.timeout = static_cast<unsigned>(timeoutMs);
    bulk.data = rx.data();

    const int rc = ioctl(fd_, USBDEVFS_BULK, &bulk);
    if (rc < 0) {
        if (errno == ETIMEDOUT || errno == ENODATA || errno == ECONNRESET) {
            return 0;
        }
        status_ = std::string("USB bulk IN error: ") + std::strerror(errno);
        LOGW("USB bulk IN error: %s", std::strerror(errno));
        return -1;
    }

    size_t len = (rc > 0) ? static_cast<size_t>(rc) : 0;
    if (len == 0) {
        return 0;
    }

    if (len > rx.size()) {
        len = rx.size();
    }

    const int64_t t = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();

    int frames = 0;
    size_t offset = 0;

    while (offset + 36 <= len) {
        const uint32_t msgType = get32le(rx.data() + offset + 0);
        if (msgType != RNDIS_PACKET_MSG) {
            break;
        }

        uint32_t msgLen = get32le(rx.data() + offset + 4);
        uint32_t dataOff = get32le(rx.data() + offset + 8);
        uint32_t dataLen = get32le(rx.data() + offset + 12);

        if (dataOff < 36) {
            dataOff = 36;
        }

        if (static_cast<size_t>(offset + dataOff + dataLen) > len) {
            break;
        }

        if (dataLen > 0) {
            onFrame(rx.data() + offset + dataOff, dataLen, t);
            frames++;
        }

        size_t step = msgLen;
        if (step < dataOff + dataLen) {
            step = dataOff + dataLen;
        }
        if (packetAlignment_ > 1) {
            step = ((step + packetAlignment_ - 1) / packetAlignment_) * packetAlignment_;
        }
        if (step == 0) {
            step = 44;
        }

        offset += step;
    }

    return frames;
}

bool UsbRndis::sendFrame(const uint8_t* frame, size_t len) {
    if (!initialized_ || frame == nullptr || len == 0) {
        return false;
    }

    const size_t total = 44 + len;
    std::vector<uint8_t> out(total, 0);

    put32le(out.data() + 0, RNDIS_PACKET_MSG);
    put32le(out.data() + 4, static_cast<uint32_t>(total));
    put32le(out.data() + 8, 36);
    put32le(out.data() + 12, static_cast<uint32_t>(len));
    put32le(out.data() + 16, sequence_++);

    std::memcpy(out.data() + 36, frame, len);

    struct usbdevfs_bulktransfer bulk {};
    bulk.ep = static_cast<unsigned>(epOut_);
    bulk.len = static_cast<unsigned>(out.size());
    bulk.timeout = 500;
    bulk.data = out.data();

    const int rc = ioctl(fd_, USBDEVFS_BULK, &bulk);
    if (rc < 0) {
        status_ = std::string("USB bulk OUT error: ") + std::strerror(errno);
        LOGW("USB bulk OUT error: %s", std::strerror(errno));
        return false;
    }

    return true;
}

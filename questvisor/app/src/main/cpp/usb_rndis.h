// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE (GPL-3.0 with an additional permission
// to combine with the Meta Spatial SDK).
#ifndef USB_RNDIS_H
#define USB_RNDIS_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class UsbRndis {
public:
    UsbRndis(int fd, int ctrlIf, int epIn, int epOut);
    ~UsbRndis();

    bool init();
    int readFrames(int timeoutMs, const std::function<void(const uint8_t*, size_t, int64_t)>& onFrame);
    bool sendFrame(const uint8_t* frame, size_t len);

    std::string peerMacHex() const;
    std::string statusString() const;
    bool initialized() const { return initialized_; }

private:
    int fd_;
    int ctrlIf_;
    int epIn_;
    int epOut_;

    bool initialized_ = false;
    uint8_t peerMac_[6] {};
    uint32_t requestId_ = 1;
    uint32_t sequence_ = 1;
    uint32_t maxTransferSize_ = 16384;
    uint32_t packetAlignment_ = 4;
    std::string status_;

    bool controlSend(const uint8_t* data, uint32_t len);
    bool controlRead(std::vector<uint8_t>& resp);
    bool sendEncapsulated(uint8_t* data, uint32_t len, uint32_t expectedType, std::vector<uint8_t>& resp);

    bool initialize();
    bool queryPermanentAddress();
    bool setPacketFilter();

    static uint32_t get32le(const uint8_t* p);
    static void put32le(uint8_t* p, uint32_t v);
};

#endif

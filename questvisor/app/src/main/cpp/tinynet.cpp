// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE (GPL-3.0 with an additional permission
// to combine with the Meta Spatial SDK).
#include "tinynet.h"

#include <android/log.h>
#include <chrono>
#include <cstring>
#include <vector>

#define LOG_TAG "QuestVisor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {
    constexpr uint16_t kTcpMss = 1460;
    constexpr uint16_t kTcpWindow = 65535;

    uint16_t get16(const uint8_t* p) {
        return static_cast<uint16_t>((p[0] << 8) | p[1]);
    }

    uint32_t get32(const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) << 24) |
            (static_cast<uint32_t>(p[1]) << 16) |
            (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
    }

    void put16(uint8_t* p, uint16_t v) {
        p[0] = static_cast<uint8_t>((v >> 8) & 0xff);
        p[1] = static_cast<uint8_t>(v & 0xff);
    }

    void put32(uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>((v >> 24) & 0xff);
        p[1] = static_cast<uint8_t>((v >> 16) & 0xff);
        p[2] = static_cast<uint8_t>((v >> 8) & 0xff);
        p[3] = static_cast<uint8_t>(v & 0xff);
    }

    uint16_t checksum(uint32_t initialSum, const uint8_t* data, size_t len) {
        uint32_t sum = initialSum;
        size_t i = 0;

        for (; i + 1 < len; i += 2) {
            sum += (static_cast<uint32_t>(data[i]) << 8) | data[i + 1];
        }

        if (i < len) {
            sum += static_cast<uint32_t>(data[i]) << 8;
        }

        while (sum >> 16) {
            sum = (sum & 0xffff) + (sum >> 16);
        }

        return static_cast<uint16_t>(~sum & 0xffff);
    }

    uint16_t ipChecksum(const uint8_t* ipHeader) {
        return checksum(0, ipHeader, 20);
    }

    uint16_t tcpChecksum(const uint8_t* tcpHeader, size_t tcpLen,
                        const uint8_t* srcIp, const uint8_t* dstIp) {
        uint8_t pseudo[12];
        std::memcpy(pseudo + 0, srcIp, 4);
        std::memcpy(pseudo + 4, dstIp, 4);
        pseudo[8] = 0;
        pseudo[9] = 6;
        put16(pseudo + 10, static_cast<uint16_t>(tcpLen));

        uint32_t sum = 0;
        for (size_t i = 0; i + 1 < sizeof(pseudo); i += 2) {
            sum += (static_cast<uint32_t>(pseudo[i]) << 8) | pseudo[i + 1];
        }

        for (size_t i = 0; i + 1 < tcpLen; i += 2) {
            sum += (static_cast<uint32_t>(tcpHeader[i]) << 8) | tcpHeader[i + 1];
        }

        if (tcpLen & 1) {
            sum += static_cast<uint32_t>(tcpHeader[tcpLen - 1]) << 8;
        }

        while (sum >> 16) {
            sum = (sum & 0xffff) + (sum >> 16);
        }

        return static_cast<uint16_t>(~sum & 0xffff);
    }
}

TinyNet::TinyNet(Sender sender, Payload payload)
    : sender_(std::move(sender)), payload_(std::move(payload)) {
    ourMac_[0] = 0x02;
    ourMac_[1] = 0x51;
    ourMac_[2] = 0x52;
    ourMac_[3] = 0x4C;
    ourMac_[4] = 0x59;
    ourMac_[5] = 0x01;

    ourIp_[0] = 192;
    ourIp_[1] = 168;
    ourIp_[2] = 60;
    ourIp_[3] = 1;

    peerIp_[0] = 192;
    peerIp_[1] = 168;
    peerIp_[2] = 60;
    peerIp_[3] = 2;

    std::memset(peerMac_, 0, sizeof(peerMac_));
    rng_.seed(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
}

bool TinyNet::connected() const {
    return state_ == ESTABLISHED;
}

bool TinyNet::streaming() const {
    return streaming_;
}

std::string TinyNet::statusString(int64_t nowUs) const {
    if (state_ == CLOSED && nowUs < reconnectAtUs_) {
        return "TCP closed, reconnecting";
    }
    if (!arpResolved_) {
        return "ARP resolving";
    }
    if (state_ == ARP_WAIT) {
        return "ARP ok, TCP connecting";
    }
    if (state_ == SYN_SENT) {
        return "ARP ok, TCP connecting";
    }
    if (streaming_) {
        return "TCP connected, streaming";
    }
    return "TCP connected";
}

void TinyNet::resetConnection(int64_t nowUs) {
    std::uniform_int_distribution<uint16_t> portDist(40000, 60000);
    std::uniform_int_distribution<uint32_t> isnDist(1, 0xffffffffu);

    localPort_ = portDist(rng_);
    localISN_ = isnDist(rng_);
    localSeq_ = localISN_;
    peerSeq_ = 0;
    pendingAcks_ = 0;
    lastSynUs_ = nowUs;
}

bool TinyNet::sendArp(int oper, const uint8_t* targetMac, const uint8_t* targetIp) {
    static const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t frame[42] {};

    std::memcpy(frame + 0, targetMac ? targetMac : kBroadcast, 6);
    std::memcpy(frame + 6, ourMac_, 6);
    frame[12] = 0x08;
    frame[13] = 0x06;

    uint8_t* arp = frame + 14;
    put16(arp + 0, 1);
    put16(arp + 2, 0x0800);
    arp[4] = 6;
    arp[5] = 4;
    put16(arp + 6, static_cast<uint16_t>(oper));

    std::memcpy(arp + 8, ourMac_, 6);
    std::memcpy(arp + 14, ourIp_, 4);

    if (targetMac != nullptr) {
        std::memcpy(arp + 18, targetMac, 6);
    }
    std::memcpy(arp + 24, targetIp, 4);

    return sender_(frame, sizeof(frame));
}

bool TinyNet::sendArpReplyFor(const uint8_t* req) {
    const uint8_t* arp = req + 14;
    const uint8_t* senderMac = arp + 8;
    const uint8_t* senderIp = arp + 14;
    const uint8_t* targetIp = arp + 24;

    if (std::memcmp(targetIp, ourIp_, 4) != 0) {
        return true;
    }

    if (std::memcmp(senderIp, peerIp_, 4) == 0) {
        std::memcpy(peerMac_, senderMac, 6);
        arpResolved_ = true;
    }

    static const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t* dstMac = arpResolved_ ? peerMac_ : kBroadcast;

    uint8_t frame[42] {};
    std::memcpy(frame + 0, dstMac, 6);
    std::memcpy(frame + 6, ourMac_, 6);
    frame[12] = 0x08;
    frame[13] = 0x06;

    uint8_t* out = frame + 14;
    put16(out + 0, 1);
    put16(out + 2, 0x0800);
    out[4] = 6;
    out[5] = 4;
    put16(out + 6, 2);

    std::memcpy(out + 8, ourMac_, 6);
    std::memcpy(out + 14, ourIp_, 4);
    std::memcpy(out + 18, senderMac, 6);
    std::memcpy(out + 24, senderIp, 4);

    return sender_(frame, sizeof(frame));
}

bool TinyNet::sendTcp(uint8_t flags, uint32_t seq, uint32_t ack,
                      const uint8_t* payload, size_t payloadLen) {
    static const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    uint8_t options[8] {};
    options[0] = 2;
    options[1] = 4;
    put16(options + 2, kTcpMss);
    options[4] = 3;
    options[5] = 3;
    options[6] = 0;
    options[7] = 1;

    size_t optionLen = (flags & 0x02) ? sizeof(options) : 0;
    const size_t tcpLen = 20 + optionLen + payloadLen;
    const uint8_t dataOffsetWords = static_cast<uint8_t>(20 + optionLen) / 4;

    const size_t frameLen = 14 + 20 + tcpLen;
    std::vector<uint8_t> frame(frameLen, 0);

    uint8_t* eth = frame.data();
    std::memcpy(eth + 0, arpResolved_ ? peerMac_ : kBroadcast, 6);
    std::memcpy(eth + 6, ourMac_, 6);
    eth[12] = 0x08;
    eth[13] = 0x00;

    uint8_t* ip = eth + 14;
    ip[0] = 0x45;
    put16(ip + 2, static_cast<uint16_t>(20 + tcpLen));
    put16(ip + 4, 0);
    ip[6] = 0;
    ip[7] = 0;
    ip[8] = 64;
    ip[9] = 6;
    std::memcpy(ip + 12, ourIp_, 4);
    std::memcpy(ip + 16, peerIp_, 4);
    put16(ip + 10, ipChecksum(ip));

    uint8_t* tcp = ip + 20;
    put16(tcp + 0, localPort_);
    put16(tcp + 2, 9100);
    put32(tcp + 4, seq);
    put32(tcp + 8, ack);
    tcp[12] = static_cast<uint8_t>(dataOffsetWords << 4);
    tcp[13] = flags;
    put16(tcp + 14, kTcpWindow);
    put16(tcp + 16, 0);

    if (optionLen) {
        std::memcpy(tcp + 20, options, optionLen);
    }

    if (payload != nullptr && payloadLen) {
        std::memcpy(tcp + 20 + optionLen, payload, payloadLen);
    }

    put16(tcp + 16, tcpChecksum(tcp, tcpLen, ip + 12, ip + 16));
    return sender_(eth, frameLen);
}

void TinyNet::handleFrame(const uint8_t* eth, size_t len, int64_t nowUs) {
    if (eth == nullptr || len < 14) {
        return;
    }

    const uint16_t ethertype = get16(eth + 12);

    if (ethertype == 0x0806 && len >= 42) {
        const uint8_t* arp = eth + 14;
        const int oper = get16(arp + 6);

        if (oper == 1) {
            sendArpReplyFor(eth);
        } else if (oper == 2) {
            const uint8_t* senderMac = arp + 8;
            const uint8_t* senderIp = arp + 14;
            if (std::memcmp(senderIp, peerIp_, 4) == 0) {
                if (!arpResolved_ || std::memcmp(peerMac_, senderMac, 6) != 0) {
                    std::memcpy(peerMac_, senderMac, 6);
                    arpResolved_ = true;
                    if (state_ != ESTABLISHED) {
                        state_ = ARP_WAIT;
                    }
                    LOGI("USB ARP resolved peer MAC %02x:%02x:%02x:%02x:%02x:%02x",
                         peerMac_[0], peerMac_[1], peerMac_[2],
                         peerMac_[3], peerMac_[4], peerMac_[5]);
                }
            }
        }

        return;
    }

    if (ethertype != 0x0800 || len < 14 + 20) {
        return;
    }

    const uint8_t* ip = eth + 14;
    const size_t ipHdrLen = static_cast<size_t>((ip[0] & 0x0f) * 4);
    if (ipHdrLen < 20 || len < 14 + ipHdrLen) {
        return;
    }

    if (ip[9] != 6) {
        return;
    }

    const uint8_t* dstIp = ip + 16;
    if (std::memcmp(dstIp, ourIp_, 4) != 0) {
        return;
    }

    const uint16_t ipTotal = get16(ip + 2);
    if (ipTotal < ipHdrLen || 14 + ipTotal > len) {
        return;
    }

    const uint8_t* tcp = ip + ipHdrLen;
    const size_t tcpLen = static_cast<size_t>(ipTotal - ipHdrLen);
    if (tcpLen < 20) {
        return;
    }

    const size_t tcpHdrLen = static_cast<size_t>((tcp[12] >> 4) * 4);
    if (tcpHdrLen < 20 || tcpHdrLen > tcpLen) {
        return;
    }

    const uint32_t seq = get32(tcp + 4);
    const uint32_t ack = get32(tcp + 8);
    const uint8_t flags = tcp[13];
    const size_t payloadLen = tcpLen - tcpHdrLen;
    const uint8_t* payload = tcp + tcpHdrLen;

    if ((flags & 0x04) != 0) {
        state_ = CLOSED;
        reconnectAtUs_ = nowUs + 500000;
        streaming_ = false;
        LOGW("USB TCP RST");
        return;
    }

    if (state_ == SYN_SENT && (flags & 0x02) && (flags & 0x10)) {
        if (ack == localISN_ + 1) {
            peerSeq_ = seq + 1;
            localSeq_ = localISN_ + 1;
            state_ = ESTABLISHED;
            lastAckUs_ = nowUs;
            LOGI("USB TCP connected to peer:9100 local_port=%u", localPort_);
            sendTcp(0x10, localSeq_, peerSeq_, nullptr, 0);
        }
        return;
    }

    if (state_ != ESTABLISHED) {
        if (payloadLen > 0 || seq != peerSeq_) {
            sendTcp(0x10, localSeq_, peerSeq_, nullptr, 0);
        }
        return;
    }

    if (seq == peerSeq_) {
        if (payloadLen > 0) {
            if (payload_ != nullptr) {
                payload_(payload, payloadLen, nowUs);
            }
            rxBytes_ += payloadLen;
            if (rxBytes_ > 1024) {
                streaming_ = true;
            }
            peerSeq_ += payloadLen;
            pendingAcks_++;
        }

        if ((flags & 0x01) != 0) {
            peerSeq_ += (payloadLen == 0 ? 1 : 0);
            sendTcp(0x10, localSeq_, peerSeq_, nullptr, 0);
            state_ = CLOSED;
            streaming_ = false;
            reconnectAtUs_ = nowUs + 500000;
            LOGW("USB TCP FIN");
            return;
        }

        if (pendingAcks_ > 0) {
            if (pendingAcks_ >= 2 || (nowUs - lastAckUs_) >= 5000) {
                sendTcp(0x10, localSeq_, peerSeq_, nullptr, 0);
                pendingAcks_ = 0;
                lastAckUs_ = nowUs;
            }
        }
    } else {
        sendTcp(0x10, localSeq_, peerSeq_, nullptr, 0);
    }
}

void TinyNet::tick(int64_t nowUs) {
    if (state_ == CLOSED) {
        if (nowUs >= reconnectAtUs_) {
            resetConnection(nowUs);
            state_ = ARP_WAIT;
            arpResolved_ = false;
            lastArpUs_ = 0;
        }
        return;
    }

    if (!arpResolved_) {
        if (lastArpUs_ == 0 || (nowUs - lastArpUs_) >= 200000) {
            lastArpUs_ = nowUs;
            static uint8_t zeroMac[6] = {0, 0, 0, 0, 0, 0};
            sendArp(1, zeroMac, peerIp_);
        }
        return;
    }

    if (state_ == ARP_WAIT) {
        state_ = SYN_SENT;
        resetConnection(nowUs);
    }

    if (state_ == SYN_SENT && (nowUs - lastSynUs_) >= 1000000) {
        lastSynUs_ = nowUs;
        sendTcp(0x02, localISN_, 0, nullptr, 0);
    }

    if (state_ == ESTABLISHED && pendingAcks_ > 0 && (nowUs - lastAckUs_) >= 5000) {
        sendTcp(0x10, localSeq_, peerSeq_, nullptr, 0);
        pendingAcks_ = 0;
        lastAckUs_ = nowUs;
    }
}

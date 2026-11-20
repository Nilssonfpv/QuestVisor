// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE (GPL-3.0 with an additional permission
// to combine with the Meta Spatial SDK).
#ifndef TINENET_H
#define TINENET_H

#include <cstdint>
#include <functional>
#include <random>
#include <string>

class TinyNet {
public:
    using Sender = std::function<bool(const uint8_t*, size_t)>;
    using Payload = std::function<void(const uint8_t*, size_t, int64_t)>;

    TinyNet(Sender sender, Payload payload);

    void handleFrame(const uint8_t* eth, size_t len, int64_t nowUs);
    void tick(int64_t nowUs);

    bool connected() const;
    bool streaming() const;
    std::string statusString(int64_t nowUs) const;

private:
    enum State {
        ARP_WAIT,
        SYN_SENT,
        ESTABLISHED,
        CLOSED,
    };

    Sender sender_;
    Payload payload_;

    State state_ = ARP_WAIT;
    bool arpResolved_ = false;
    bool streaming_ = false;

    uint8_t ourMac_[6];
    uint8_t peerMac_[6];
    uint8_t ourIp_[4];
    uint8_t peerIp_[4];

    uint16_t localPort_ = 0;
    uint32_t localISN_ = 0;
    uint32_t localSeq_ = 0;
    uint32_t peerSeq_ = 0;

    int64_t lastArpUs_ = 0;
    int64_t lastSynUs_ = 0;
    int64_t lastAckUs_ = 0;
    int64_t reconnectAtUs_ = 0;

    uint64_t rxBytes_ = 0;
    int pendingAcks_ = 0;

    std::mt19937_64 rng_;

    void resetConnection(int64_t nowUs);
    bool sendArp(int oper, const uint8_t* targetMac, const uint8_t* targetIp);
    bool sendTcp(uint8_t flags, uint32_t seq, uint32_t ack, const uint8_t* payload, size_t payloadLen);
    bool sendArpReplyFor(const uint8_t* req);
};

#endif

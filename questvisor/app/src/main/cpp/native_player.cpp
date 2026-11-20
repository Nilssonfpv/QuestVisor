// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE (GPL-3.0 with an additional permission
// to combine with the Meta Spatial SDK).
#include <jni.h>

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include "tinynet.h"
#include "usb_rndis.h"
#include "replay_recorder.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <ctime>
#include <cctype>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#define LOG_TAG "QuestVisor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Replay buffer recorder; lives across decoder instances (one per connection).
static replay::Recorder g_recorder;

namespace {
    constexpr size_t kNotFound = static_cast<size_t>(-1);
    constexpr int kDefaultWidth = 1832;
    constexpr int kDefaultHeight = 1032;
    constexpr int kMaxWidth = 1920;
    constexpr int kMaxHeight = 1088;
    constexpr int kMaxInputSize = 8 * 1024 * 1024;
    constexpr int kRcvBufSize = 256 * 1024;
    constexpr int kCodecTypeH265 = 1;
    constexpr int kCodecTypeH264 = 2;
    constexpr int kRelayClockPort = 9200;
    constexpr uint32_t kBufferFlagKeyFrame = 1u;
    constexpr uint32_t kBufferFlagEndOfStream = 4u;

    // Nice value of Android's THREAD_PRIORITY_VIDEO. App processes inherit RLIMIT_NICE 40 from init, so this
    // normally succeeds; the result is logged either way.
    void raiseThreadPriority(const char* what) {
        const pid_t tid = gettid();
        for (int nice : {-10, -4}) {
            if (setpriority(PRIO_PROCESS, tid, nice) == 0) {
                __android_log_print(ANDROID_LOG_INFO, "QuestVisor", "thread %s: nice %d", what, nice);
                return;
            }
        }
        __android_log_print(ANDROID_LOG_WARN, "QuestVisor", "thread %s: setpriority failed: %s", what,
                            std::strerror(errno));
    }

    int64_t nowUs() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }

    int64_t monoUs() {
        timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000000LL +
            static_cast<int64_t>(ts.tv_nsec / 1000LL);
    }

    uint64_t getBE64(const uint8_t* p) {
        return (static_cast<uint64_t>(p[0]) << 56) |
            (static_cast<uint64_t>(p[1]) << 48) |
            (static_cast<uint64_t>(p[2]) << 40) |
            (static_cast<uint64_t>(p[3]) << 32) |
            (static_cast<uint64_t>(p[4]) << 24) |
            (static_cast<uint64_t>(p[5]) << 16) |
            (static_cast<uint64_t>(p[6]) << 8) |
            static_cast<uint64_t>(p[7]);
    }

    uint32_t getBE32(const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) << 24) |
            (static_cast<uint32_t>(p[1]) << 16) |
            (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
    }

    void putBE64(uint8_t* p, int64_t v) {
        uint64_t x = static_cast<uint64_t>(v);
        p[0] = static_cast<uint8_t>((x >> 56) & 0xff);
        p[1] = static_cast<uint8_t>((x >> 48) & 0xff);
        p[2] = static_cast<uint8_t>((x >> 40) & 0xff);
        p[3] = static_cast<uint8_t>((x >> 32) & 0xff);
        p[4] = static_cast<uint8_t>((x >> 24) & 0xff);
        p[5] = static_cast<uint8_t>((x >> 16) & 0xff);
        p[6] = static_cast<uint8_t>((x >> 8) & 0xff);
        p[7] = static_cast<uint8_t>(x & 0xff);
    }

    void releaseInputBufferSafely(AMediaCodec* codec, size_t idx) {
        if (codec == nullptr) {
            return;
        }

        (void)AMediaCodec_queueInputBuffer(codec, idx, 0, 0, 0, 0);
    }

    bool endsWith(const std::string& value, const std::string& suffix) {
        return value.size() >= suffix.size() &&
            value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::string toLowerAscii(std::string value) {
        for (char& c : value) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return value;
    }

    std::string normalizeDecoderMode(const std::string& mode) {
        const std::string l = toLowerAscii(mode);
        if (l == "sw") {
            return "sw";
        }
        if (l == "hw") {
            return "hw";
        }
        return "auto";
    }

    struct FakeIdrSidecar {
        bool valid = false;
        std::string nal;
        int craOffsetInFile = -1;
        int pocLsbRbspBit = -1;
        int pocLsbBits = 8;
    };

    struct FakeIdr {
        std::string name;
        int codec = 0;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> blob;
        FakeIdrSidecar sidecar;
    };

    std::mutex g_fakeIdrMutex;
    std::vector<FakeIdr> g_fakeIdrs;

    std::vector<uint8_t> applyEmulationPrevention(const std::vector<uint8_t>& input) {
        std::vector<uint8_t> out;
        out.reserve(input.size() + input.size() / 3u + 8u);

        for (uint8_t b : input) {
            if (out.size() >= 2 &&
                out[out.size() - 1] == 0x00 &&
                out[out.size() - 2] == 0x00 &&
                b <= 0x03) {
                out.push_back(0x03);
            }
            out.push_back(b);
        }

        return out;
    }

    std::vector<uint8_t> removeEmulationPrevention(const uint8_t* p, size_t n) {
        std::vector<uint8_t> out;
        out.reserve(n);

        if (p == nullptr) {
            return out;
        }

        for (size_t i = 0; i < n; ) {
            if (i + 2 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 3) {
                out.push_back(0);
                out.push_back(0);
                i += 3;
            } else {
                out.push_back(p[i]);
                i++;
            }
        }

        return out;
    }

    std::vector<uint8_t> makeRbsp(const uint8_t* nal, size_t size, size_t headerLen) {
        if (nal == nullptr || size <= headerLen) {
            return {};
        }
        return removeEmulationPrevention(nal + headerLen, size - headerLen);
    }

    size_t findStartCodeIn(const uint8_t* p, size_t len, size_t searchFrom, size_t* outLen) {
        if (p == nullptr || len < 3 || searchFrom + 2 >= len) {
            return kNotFound;
        }

        for (size_t i = searchFrom; i + 2 < len; ++i) {
            if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
                size_t q = i;
                while (q > searchFrom && p[q - 1] == 0) {
                    --q;
                }

                if (outLen != nullptr) {
                    if (q + 3 < len &&
                        p[q] == 0 &&
                        p[q + 1] == 0 &&
                        p[q + 2] == 0 &&
                        p[q + 3] == 1) {
                        *outLen = 4;
                    } else {
                        *outLen = 3;
                    }
                }

                return q;
            }
        }

        return kNotFound;
    }

    struct BitReader {
        const std::vector<uint8_t>& bytes;
        size_t pos = 0;

        explicit BitReader(const std::vector<uint8_t>& b) : bytes(b) {}

        bool readBit(uint32_t* out) {
            if (out == nullptr) {
                return false;
            }
            if (pos >= bytes.size() * 8u) {
                return false;
            }

            size_t byteIdx = pos / 8u;
            int bitIdx = 7 - static_cast<int>(pos % 8u);
            *out = (bytes[byteIdx] >> bitIdx) & 1u;
            ++pos;
            return true;
        }

        bool readBits(size_t count, uint32_t* out) {
            if (out == nullptr) {
                return false;
            }
            if (count == 0) {
                *out = 0;
                return true;
            }
            if (count > 32 || pos + count > bytes.size() * 8u) {
                return false;
            }

            uint32_t v = 0;
            for (size_t i = 0; i < count; ++i) {
                uint32_t b = 0;
                if (!readBit(&b)) {
                    return false;
                }
                v = (v << 1) | b;
            }

            *out = v;
            return true;
        }

        bool skipBits(size_t count) {
            if (count == 0) {
                return true;
            }
            if (pos + count > bytes.size() * 8u) {
                return false;
            }
            pos += count;
            return true;
        }

        bool readUE(uint32_t* out) {
            if (out == nullptr) {
                return false;
            }

            uint32_t leading = 0;
            while (true) {
                uint32_t b = 0;
                if (!readBit(&b)) {
                    return false;
                }
                if (b != 0) {
                    break;
                }
                ++leading;
                if (leading > 30) {
                    return false;
                }
            }

            if (leading == 0) {
                *out = 0;
                return true;
            }

            uint32_t info = 0;
            if (!readBits(leading, &info)) {
                return false;
            }

            *out = ((1u << leading) - 1u) + info;
            return true;
        }

        bool readSE(int* out) {
            uint32_t code = 0;
            if (!readUE(&code) || out == nullptr) {
                return false;
            }

            long long delta = 0;
            if ((code & 1u) != 0) {
                delta = static_cast<long long>((code + 1u) / 2u);
            } else {
                delta = -static_cast<long long>(code / 2u);
            }

            if (delta > 2147483647LL) {
                delta = 2147483647LL;
            }
            if (delta < -2147483648LL) {
                delta = -2147483648LL;
            }

            *out = static_cast<int>(delta);
            return true;
        }
    };

    bool skipH264ScalingList(BitReader& br, size_t size) {
        int lastScale = 8;
        int nextScale = 8;

        for (size_t j = 0; j < size; ++j) {
            if (nextScale != lastScale) {
                int delta = 0;
                if (!br.readSE(&delta)) {
                    return false;
                }
                nextScale = (lastScale + delta) & 0xff;
            }
            lastScale = (nextScale == 0) ? lastScale : nextScale;
        }

        return true;
    }

    bool parseH264SpsDimensions(const uint8_t* nal, size_t size, int& outWidth, int& outHeight) {
        if (nal == nullptr || size < 2 || ((nal[0] & 0x1f) != 7)) {
            return false;
        }

        const std::vector<uint8_t> rbsp = makeRbsp(nal, size, 1);
        if (rbsp.empty()) {
            return false;
        }

        BitReader br(rbsp);
        uint32_t v = 0;

        if (!br.readBits(8, &v)) {
            return false;
        }
        uint32_t profileIdc = v;

        if (!br.skipBits(8)) {
            return false;
        }

        if (!br.readBits(8, &v)) {
            return false;
        }

        if (!br.readUE(&v)) {
            return false;
        }

        uint32_t chromaFormatIdc = 1;
        bool frameMbsOnly = true;
        uint32_t log2MaxPocLsbMinus4 = 0;
        (void)log2MaxPocLsbMinus4;

        bool highProfile =
            profileIdc == 100 ||
            profileIdc == 110 ||
            profileIdc == 122 ||
            profileIdc == 244 ||
            profileIdc == 44 ||
            profileIdc == 83 ||
            profileIdc == 86 ||
            profileIdc == 118 ||
            profileIdc == 128 ||
            profileIdc == 138 ||
            profileIdc == 139 ||
            profileIdc == 134 ||
            profileIdc == 135 ||
            profileIdc == 136;

        if (highProfile) {
            if (!br.readUE(&v)) {
                return false;
            }
            chromaFormatIdc = v;

            if (chromaFormatIdc == 3) {
                if (!br.skipBits(1)) {
                    return false;
                }
            }

            if (!br.readUE(&v) || !br.readUE(&v)) {
                return false;
            }

            if (!br.skipBits(1)) {
                return false;
            }

            uint32_t scalingMatrixPresent = 0;
            if (!br.readBit(&scalingMatrixPresent)) {
                return false;
            }

            if (scalingMatrixPresent != 0) {
                size_t loopCount = (profileIdc == 244) ? 6 : 8;
                for (size_t i = 0; i < loopCount; ++i) {
                    uint32_t present = 0;
                    if (!br.readBit(&present)) {
                        return false;
                    }
                    if (present != 0 && !skipH264ScalingList(br, 16)) {
                        return false;
                    }
                }
            }
        }

        if (!br.readUE(&v)) {
            return false;
        }

        uint32_t picOrderCntType = 0;
        if (!br.readUE(&picOrderCntType)) {
            return false;
        }

        if (picOrderCntType == 0) {
            if (!br.readUE(&log2MaxPocLsbMinus4)) {
                return false;
            }
        } else if (picOrderCntType == 1) {
            uint32_t deltaZero = 0;
            if (!br.readBit(&deltaZero)) {
                return false;
            }
            if (deltaZero == 0) {
                int tmp = 0;
                if (!br.readSE(&tmp) || !br.readSE(&tmp)) {
                    return false;
                }
                uint32_t numRef = 0;
                if (!br.readUE(&numRef)) {
                    return false;
                }
                for (uint32_t i = 0; i < numRef && i < 256; ++i) {
                    if (!br.readSE(&tmp)) {
                        return false;
                    }
                }
            }
        }

        if (!br.readUE(&v)) {
            return false;
        }

        if (!br.skipBits(1)) {
            return false;
        }

        uint32_t widthInMbsMinus1 = 0;
        uint32_t heightInMapUnitsMinus1 = 0;
        if (!br.readUE(&widthInMbsMinus1)) {
            return false;
        }
        if (!br.readUE(&heightInMapUnitsMinus1)) {
            return false;
        }

        uint32_t frameMbsOnlyFlag = 1;
        if (!br.readBit(&frameMbsOnlyFlag)) {
            return false;
        }
        frameMbsOnly = (frameMbsOnlyFlag != 0);

        if (!frameMbsOnly) {
            if (!br.skipBits(1)) {
                return false;
            }
        }

        if (!br.skipBits(1)) {
            return false;
        }

        uint32_t cropFlag = 0;
        if (!br.readBit(&cropFlag)) {
            return false;
        }

        uint32_t cropLeft = 0;
        uint32_t cropRight = 0;
        uint32_t cropTop = 0;
        uint32_t cropBottom = 0;

        if (cropFlag != 0) {
            if (!br.readUE(&cropLeft) ||
                !br.readUE(&cropRight) ||
                !br.readUE(&cropTop) ||
                !br.readUE(&cropBottom)) {
                return false;
            }
        }

        int width = static_cast<int>(widthInMbsMinus1 + 1u) * 16;
        int height = static_cast<int>(heightInMapUnitsMinus1 + 1u) * 16;
        if (!frameMbsOnly) {
            height *= 2;
        }

        int cropUnitX = 1;
        int cropUnitY = 1;
        if (chromaFormatIdc != 0) {
            cropUnitX = 2;
            cropUnitY = frameMbsOnly ? 2 : 4;
        }

        width -= cropUnitX * static_cast<int>(cropLeft + cropRight);
        height -= cropUnitY * static_cast<int>(cropTop + cropBottom);

        if (width <= 0 || height <= 0) {
            return false;
        }

        outWidth = width;
        outHeight = height;
        return true;
    }

    bool skipProfileTierLevel(BitReader& br, uint32_t maxSubLayersMinus1) {
        if (!br.skipBits(2)) {
            return false;
        }
        uint32_t tier = 0;
        if (!br.readBit(&tier)) {
            return false;
        }
        if (!br.skipBits(5)) {
            return false;
        }
        if (!br.skipBits(32)) {
            return false;
        }
        if (!br.skipBits(4)) {
            return false;
        }
        if (!br.skipBits(44)) {
            return false;
        }
        if (!br.skipBits(8)) {
            return false;
        }

        if (maxSubLayersMinus1 == 0) {
            return true;
        }

        std::vector<uint8_t> profilePresent(maxSubLayersMinus1, 0);
        std::vector<uint8_t> levelPresent(maxSubLayersMinus1, 0);

        for (uint32_t i = 0; i < maxSubLayersMinus1; ++i) {
            uint32_t pp = 0;
            uint32_t lp = 0;
            if (!br.readBit(&pp) || !br.readBit(&lp)) {
                return false;
            }
            profilePresent[i] = pp;
            levelPresent[i] = lp;
        }

        for (uint32_t i = maxSubLayersMinus1; i < 8; ++i) {
            if (!br.skipBits(2)) {
                return false;
            }
        }

        for (uint32_t i = 0; i < maxSubLayersMinus1; ++i) {
            if (profilePresent[i] != 0) {
                if (!br.skipBits(2)) {
                    return false;
                }
                uint32_t tier2 = 0;
                if (!br.readBit(&tier2)) {
                    return false;
                }
                if (!br.skipBits(5)) {
                    return false;
                }
                if (!br.skipBits(32)) {
                    return false;
                }
                if (!br.skipBits(4)) {
                    return false;
                }
                if (!br.skipBits(44)) {
                    return false;
                }
            }
            if (levelPresent[i] != 0 && !br.skipBits(8)) {
                return false;
            }
        }

        return true;
    }

    bool parseH265SpsDimensions(const uint8_t* nal, size_t size, int& outWidth, int& outHeight, int& outPocBits) {
        if (nal == nullptr || size < 2 || (((nal[0] >> 1) & 0x3f) != 33)) {
            return false;
        }

        const std::vector<uint8_t> rbsp = makeRbsp(nal, size, 2);
        if (rbsp.empty()) {
            return false;
        }

        BitReader br(rbsp);
        uint32_t v = 0;

        if (!br.readBits(4, &v) || !br.readBits(3, &v)) {
            return false;
        }
        uint32_t maxSubLayersMinus1 = v;

        uint32_t temporalNesting = 0;
        if (!br.readBit(&temporalNesting)) {
            return false;
        }
        (void)temporalNesting;

        if (!skipProfileTierLevel(br, maxSubLayersMinus1)) {
            return false;
        }

        if (!br.readUE(&v) || !br.readUE(&v)) {
            return false;
        }
        uint32_t chromaFormatIdc = v;

        if (chromaFormatIdc == 3 && !br.skipBits(1)) {
            return false;
        }

        uint32_t picWidth = 0;
        uint32_t picHeight = 0;
        if (!br.readUE(&picWidth) || !br.readUE(&picHeight)) {
            return false;
        }

        uint32_t cropFlag = 0;
        if (!br.readBit(&cropFlag)) {
            return false;
        }

        uint32_t cropLeft = 0;
        uint32_t cropRight = 0;
        uint32_t cropTop = 0;
        uint32_t cropBottom = 0;

        if (cropFlag != 0) {
            if (!br.readUE(&cropLeft) ||
                !br.readUE(&cropRight) ||
                !br.readUE(&cropTop) ||
                !br.readUE(&cropBottom)) {
                return false;
            }
        }

        if (!br.readUE(&v) || !br.readUE(&v) || !br.readUE(&v)) {
            return false;
        }
        uint32_t log2MaxPocLsbMinus4 = v;

        int cropUnit = (chromaFormatIdc == 0) ? 1 : 2;
        int width = static_cast<int>(picWidth) - cropUnit * static_cast<int>(cropLeft + cropRight);
        int height = static_cast<int>(picHeight) - cropUnit * static_cast<int>(cropTop + cropBottom);

        if (width <= 0 || height <= 0) {
            return false;
        }

        outWidth = width;
        outHeight = height;
        outPocBits = static_cast<int>(log2MaxPocLsbMinus4 + 4u);
        if (outPocBits < 4) {
            outPocBits = 4;
        }
        return true;
    }

    bool parseH265PpsFlags(const uint8_t* nal, size_t size, int& outExtraBits, bool& outOutputFlag) {
        if (nal == nullptr || size < 2 || (((nal[0] >> 1) & 0x3f) != 34)) {
            return false;
        }

        const std::vector<uint8_t> rbsp = makeRbsp(nal, size, 2);
        if (rbsp.empty()) {
            return false;
        }

        BitReader br(rbsp);
        uint32_t v = 0;

        if (!br.readUE(&v) || !br.readUE(&v)) {
            return false;
        }

        uint32_t outputPresent = 0;
        if (!br.readBit(&outputPresent)) {
            return false;
        }

        if (!br.readBits(3, &v)) {
            return false;
        }

        outOutputFlag = (outputPresent != 0);
        outExtraBits = static_cast<int>(v);
        if (outExtraBits < 0 || outExtraBits > 31) {
            return false;
        }
        return true;
    }

    bool parseH265PpsFlagsFromAnnexB(const std::vector<uint8_t>& annex, int& outExtraBits, bool& outOutputFlag) {
        if (annex.empty()) {
            return false;
        }

        size_t scLen = 0;
        size_t start = findStartCodeIn(annex.data(), annex.size(), 0, &scLen);
        if (start == kNotFound) {
            return false;
        }

        size_t nalStart = start + scLen;
        if (nalStart >= annex.size()) {
            return false;
        }

        size_t nextLen = 0;
        size_t next = findStartCodeIn(annex.data(), annex.size(), nalStart, &nextLen);
        size_t end = (next == kNotFound) ? annex.size() : next;

        if (end <= nalStart + 1) {
            return false;
        }

        return parseH265PpsFlags(annex.data() + nalStart, end - nalStart, outExtraBits, outOutputFlag);
    }

    bool parseHevcSlicePocLsb(
        const uint8_t* nal,
        size_t size,
        int extraBits,
        bool outputFlagPresent,
        int pocBits,
        int& pocLsb
    ) {
        if (nal == nullptr || size < 2 || pocBits <= 0) {
            return false;
        }

        int nalType = (nal[0] >> 1) & 0x3f;
        if (nalType > 31) {
            return false;
        }

        const std::vector<uint8_t> rbsp = makeRbsp(nal, size, 2);
        if (rbsp.empty()) {
            return false;
        }

        BitReader br(rbsp);
        uint32_t firstSlice = 0;
        if (!br.readBit(&firstSlice) || firstSlice == 0) {
            return false;
        }

        uint32_t dummy = 0;
        if (nalType >= 16 && nalType <= 23) {
            if (!br.readBit(&dummy)) {
                return false;
            }
        }

        if (!br.readUE(&dummy)) {
            return false;
        }

        if (extraBits > 0) {
            if (!br.skipBits(static_cast<size_t>(extraBits))) {
                return false;
            }
        }

        if (firstSlice != 0 && nalType >= 16 && nalType <= 23) {
            if (!br.skipBits(1)) {
                return false;
            }
        }

        uint32_t sliceType = 0;
        if (!br.readUE(&sliceType)) {
            return false;
        }

        if (outputFlagPresent) {
            if (!br.readBit(&dummy)) {
                return false;
            }
        }

        uint32_t poc = 0;
        if (!br.readBits(static_cast<size_t>(pocBits), &poc)) {
            return false;
        }

        pocLsb = static_cast<int>(poc);
        return true;
    }

    bool parseHevcFirstSlicePocLsb(
        const uint8_t* au,
        size_t size,
        int extraBits,
        bool outputFlagPresent,
        int pocBits,
        int& pocLsb
    ) {
        if (au == nullptr || size < 2 || pocBits <= 0) {
            return false;
        }

        size_t p = 0;
        while (p < size) {
            size_t scLen = 0;
            size_t start = findStartCodeIn(au, size, p, &scLen);
            if (start == kNotFound) {
                break;
            }

            size_t nalStart = start + scLen;
            if (nalStart >= size) {
                break;
            }

            size_t nextLen = 0;
            size_t next = findStartCodeIn(au, size, nalStart, &nextLen);
            size_t end = (next == kNotFound) ? size : next;

            if (end > nalStart + 1) {
                if (parseHevcSlicePocLsb(au + nalStart, end - nalStart, extraBits, outputFlagPresent, pocBits, pocLsb)) {
                    return true;
                }
            }

            if (end > p) {
                p = end;
            } else {
                p = nalStart + 1;
            }
        }

        return false;
    }

    bool extractFakeCsd(
        const std::vector<uint8_t>& blob,
        int codec,
        std::vector<uint8_t>& out0,
        std::vector<uint8_t>& out1
    ) {
        out0.clear();
        out1.clear();

        bool haveSps = false;
        bool havePps = false;
        bool haveVps = false;

        size_t p = 0;
        while (p < blob.size()) {
            size_t scLen = 0;
            size_t start = findStartCodeIn(blob.data(), blob.size(), p, &scLen);
            if (start == kNotFound) {
                break;
            }

            size_t nalStart = start + scLen;
            if (nalStart >= blob.size()) {
                break;
            }

            size_t nextLen = 0;
            size_t next = findStartCodeIn(blob.data(), blob.size(), nalStart, &nextLen);
            size_t end = (next == kNotFound) ? blob.size() : next;

            if (end > nalStart) {
                if (codec == kCodecTypeH264) {
                    uint8_t nalType = blob[nalStart] & 0x1f;
                    if (nalType == 7 && !haveSps) {
                        out0.insert(out0.end(), blob.begin() + static_cast<std::ptrdiff_t>(start),
                                    blob.begin() + static_cast<std::ptrdiff_t>(end));
                        haveSps = true;
                    } else if (nalType == 8 && !havePps) {
                        out1.insert(out1.end(), blob.begin() + static_cast<std::ptrdiff_t>(start),
                                    blob.begin() + static_cast<std::ptrdiff_t>(end));
                        havePps = true;
                    }
                } else if (codec == kCodecTypeH265) {
                    int nalType = (blob[nalStart] >> 1) & 0x3f;
                    if (nalType == 32 && !haveVps) {
                        out0.insert(out0.end(), blob.begin() + static_cast<std::ptrdiff_t>(start),
                                    blob.begin() + static_cast<std::ptrdiff_t>(end));
                        haveVps = true;
                    } else if (nalType == 33 && !haveSps) {
                        out0.insert(out0.end(), blob.begin() + static_cast<std::ptrdiff_t>(start),
                                    blob.begin() + static_cast<std::ptrdiff_t>(end));
                        haveSps = true;
                    } else if (nalType == 34 && !havePps) {
                        out0.insert(out0.end(), blob.begin() + static_cast<std::ptrdiff_t>(start),
                                    blob.begin() + static_cast<std::ptrdiff_t>(end));
                        havePps = true;
                    }
                }
            }

            if (end > p) {
                p = end;
            } else {
                p = nalStart + 1;
            }
        }

        if (codec == kCodecTypeH264) {
            return haveSps && havePps;
        }
        if (codec == kCodecTypeH265) {
            return haveVps && haveSps && havePps;
        }
        return false;
    }

    bool setBits(std::vector<uint8_t>& blob, size_t bitPos, int bitCount, uint64_t value) {
        if (bitCount <= 0 || bitCount > 31) {
            return false;
        }
        if (bitPos + static_cast<size_t>(bitCount) > blob.size() * 8u) {
            return false;
        }

        for (int i = 0; i < bitCount; ++i) {
            uint64_t bit = (value >> (bitCount - 1 - i)) & 1ull;
            size_t pos = bitPos + static_cast<size_t>(i);
            size_t byte = pos / 8u;
            int off = 7 - static_cast<int>(pos % 8u);

            if (bit != 0) {
                blob[byte] = static_cast<uint8_t>(blob[byte] | (1u << off));
            } else {
                blob[byte] = static_cast<uint8_t>(blob[byte] & ~(1u << off));
            }
        }

        return true;
    }

    bool patchHevcFakeIdr(std::vector<uint8_t>& blob, const FakeIdrSidecar& sidecar, int firstPocLsb) {
        if (!sidecar.valid || sidecar.craOffsetInFile < 0 || sidecar.pocLsbRbspBit < 0 || sidecar.pocLsbBits <= 0) {
            return true;
        }

        uint64_t mod = 1ull << static_cast<unsigned>(sidecar.pocLsbBits);
        int64_t v = static_cast<int64_t>(firstPocLsb) - 1;
        v %= static_cast<int64_t>(mod);
        if (v < 0) {
            v += static_cast<int64_t>(mod);
        }

        size_t bitPos = static_cast<size_t>(sidecar.craOffsetInFile + 2) * 8u + static_cast<size_t>(sidecar.pocLsbRbspBit);
        return setBits(blob, bitPos, sidecar.pocLsbBits, static_cast<uint64_t>(v));
    }

    bool findJsonInt(const std::string& s, const std::string& key, int& out) {
        const std::string quoted = "\"" + key + "\"";
        size_t pos = s.find(quoted);
        if (pos == std::string::npos) {
            return false;
        }

        pos = s.find(':', pos);
        if (pos == std::string::npos) {
            return false;
        }
        ++pos;

        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
            ++pos;
        }

        int sign = 1;
        if (pos < s.size() && s[pos] == '-') {
            sign = -1;
            ++pos;
        }

        long long value = 0;
        bool any = false;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
            any = true;
            value = value * 10 + (s[pos] - '0');
            ++pos;
        }

        if (!any || value > 2147483647LL || value < -2147483648LL) {
            return false;
        }

        out = static_cast<int>(sign * value);
        return true;
    }

    bool findJsonString(const std::string& s, const std::string& key, std::string& out) {
        const std::string quoted = "\"" + key + "\"";
        size_t pos = s.find(quoted);
        if (pos == std::string::npos) {
            return false;
        }

        pos = s.find(':', pos);
        if (pos == std::string::npos) {
            return false;
        }
        ++pos;

        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
            ++pos;
        }

        if (pos >= s.size() || s[pos] != '"') {
            return false;
        }
        ++pos;

        size_t end = pos;
        while (end < s.size() && s[end] != '"') {
            ++end;
        }

        if (end >= s.size()) {
            return false;
        }

        out = s.substr(pos, end - pos);
        return true;
    }

    bool parseSidecarJson(const std::string& json, FakeIdrSidecar& out) {
        out.valid = false;
        out.nal.clear();
        out.craOffsetInFile = -1;
        out.pocLsbRbspBit = -1;
        out.pocLsbBits = 8;

        bool ok = true;
        ok = ok && findJsonInt(json, "cra_offset_in_file", out.craOffsetInFile);
        ok = ok && findJsonInt(json, "poc_lsb_rbsp_bit", out.pocLsbRbspBit);
        (void)findJsonInt(json, "poc_lsb_bits", out.pocLsbBits);
        (void)findJsonString(json, "nal", out.nal);

        if (!ok) {
            return false;
        }

        if (out.craOffsetInFile < 0 || out.pocLsbRbspBit < 0 || out.pocLsbBits <= 0 || out.pocLsbBits > 31) {
            return false;
        }

        out.valid = true;
        return true;
    }

    int findFakeIdrIndex(int codec, int width, int height) {
        if (codec == 0 || width <= 0 || height <= 0) {
            return -1;
        }

        std::lock_guard<std::mutex> lock(g_fakeIdrMutex);
        for (size_t i = 0; i < g_fakeIdrs.size(); ++i) {
            if (g_fakeIdrs[i].codec == codec && g_fakeIdrs[i].width == width && g_fakeIdrs[i].height == height) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int codecFromName(const std::string& codecName) {
        const std::string s = toLowerAscii(codecName);
        if (s.find("265") != std::string::npos || s.find("hevc") != std::string::npos) {
            return kCodecTypeH265;
        }
        if (s.find("264") != std::string::npos || s.find("avc") != std::string::npos) {
            return kCodecTypeH264;
        }
        return 0;
    }

}

class StreamDecoder;

static void questVisorFrameRenderedCallback(
    AMediaCodec* codec,
    void* context,
    int64_t presentationTimeUs,
    int64_t renderTimeNanos
);

class StreamDecoder {
public:
    StreamDecoder(
        std::string decoderNameH265,
        std::string decoderNameH264,
        std::string decoderMode,
        std::string hwexp
    )
        : decoderNameH265_(std::move(decoderNameH265)),
          decoderNameH264_(std::move(decoderNameH264)),
          decoderMode_(normalizeDecoderMode(decoderMode)),
          hwExpString_(std::move(hwexp)) {
        const std::string lower = toLowerAscii(hwExpString_);
        expKeyFrame_ = lower.find("keyflag") != std::string::npos;
        expRecoverySei_ = lower.find("recsei") != std::string::npos;
    }

    ~StreamDecoder() {
        stop();
    }

    void startNetwork(const std::string& host, int port) {
        host_ = host;
        port_ = port;
        source_ = Source::Network;
        selftestActive_ = false;
        running_ = true;

        receiverThread_ = std::thread([this] { networkLoop(); });
        clockThread_ = std::thread([this] { clockLoop(); });
    }

    void startUsb(int fd, int ctrlIf, int epIn, int epOut) {
        source_ = Source::Usb;
        selftestActive_ = false;
        usbFd_ = fd;
        usbCtrlIf_ = ctrlIf;
        usbEpIn_ = epIn;
        usbEpOut_ = epOut;
        running_ = true;

        receiverThread_ = std::thread([this] { usbLoop(); });
    }

    void startSelftest(const std::string& path) {
        selftestPath_ = path;
        source_ = Source::Selftest;
        selftestActive_ = true;
        selftestDone_ = false;
        selftestReported_ = false;
        selftestEosSeen_ = false;
        running_ = true;

        receiverThread_ = std::thread([this] { selftestLoop(); });
    }

    // Plays the replay buffer in dir from the segment containing startMs (wall-clock ms), at the recorded pace.
    void startReplay(const std::string& dir, int64_t startMs) {
        replayDir_ = dir;
        replayStartMs_ = startMs;
        source_ = Source::Replay;
        selftestActive_ = false;
        replayState_ = 1;
        running_ = true;

        receiverThread_ = std::thread([this] { replayLoop(); });
    }

    void setReplayPaused(bool paused) { replayPaused_.store(paused); }

    // state: 0 not replaying, 1 playing, 2 paused, 3 reached the end; position in wall-clock ms.
    int replayState() const {
        if (source_ != Source::Replay) return 0;
        const int st = replayState_.load();
        return st == 1 && replayPaused_.load() ? 2 : st;
    }
    int64_t replayPositionMs() const { return replayPosUs_.load() / 1000; }

    void stop() {
        running_ = false;
        outputStopRequested_ = true;
        selftestActive_ = false;

        if (clockFd_ >= 0) {
            shutdown(clockFd_, SHUT_RDWR);
        }

        if (socketFd_ >= 0) {
            shutdown(socketFd_, SHUT_RDWR);
        }

        if (clockThread_.joinable()) {
            clockThread_.join();
        }

        // The receiver first: on the socket shutdown it may reset the decoder itself, which joins the output
        // thread. Joining that thread here at the same time aborted the app ("thread::join failed").
        if (receiverThread_.joinable()) {
            receiverThread_.join();
        }

        if (outputThread_.joinable()) {
            outputThread_.join();
        }

        {
            std::lock_guard<std::mutex> lock(configureMutex_);
            if (codec_ != nullptr) {
                AMediaCodec_stop(codec_);
                AMediaCodec_delete(codec_);
                codec_ = nullptr;
            }

            configured_ = false;
            outputThreadStarted_ = false;
            outputStopRequested_ = false;
            softwareActive_ = false;
            softwareFallbackRequested_ = false;
            hwConfigureFailedPermanent_ = false;
            swConfigureFailedPermanent_ = false;
            configurePlanIndex_ = 0;
            configuredPlanIndex_ = -1;
            configureVariant_ = -1;

            fakeSelected_.store(false, std::memory_order_release);
            pendingFakeInjection_.store(false, std::memory_order_release);
            fakeOutputPending_.store(false, std::memory_order_release);
            fakeOutputPts_.store(-1, std::memory_order_release);
            activeFake_ = FakeIdr();
            fakeCsd0_.clear();
            fakeCsd1_.clear();
        }

        {
            std::lock_guard<std::mutex> lock(fakeNameMutex_);
            fakeName_.clear();
        }

        {
            std::lock_guard<std::mutex> lock(nameMutex_);
            decoderName_.clear();
            swDecoderName_.clear();
        }

        if (clockFd_ >= 0) {
            close(clockFd_);
            clockFd_ = -1;
        }

        if (socketFd_ >= 0) {
            close(socketFd_);
            socketFd_ = -1;
        }
    }

    void setSurface(ANativeWindow* surface) {
        surface_.store(surface);
        surfaceReady_.store(surface != nullptr);

        std::lock_guard<std::mutex> lock(configureMutex_);

        if (configured_.load() && codec_ != nullptr && surface != nullptr) {
            AMediaCodec_setOutputSurface(codec_, surface);
        }

        if (!configured_.load()) {
            tryConfigureLocked();
        }
    }

    int uiState() {
        if (!running_.load()) {
            return 0;
        }
        if (totalFramesDecoded_.load() > 0 && configured_.load()) {
            return 3;
        }
        if (receiving_.load()) {
            return 2;
        }
        if (connected_.load()) {
            return 1;
        }
        return 1;
    }

    void getStats(long* out) {
        const int64_t now = nowUs();

        std::lock_guard<std::mutex> lock(statsMutex_);
        updateCountersLocked(now);

        double avgDecode = 0.0;
        double maxDecode = 0.0;
        double avgDisplay = 0.0;
        double maxDisplay = 0.0;
        double avgRelay = 0.0;
        double maxRelay = 0.0;

        auto windowStats = [&](const std::deque<LatencySample>& dq,
                               double& avgMs, double& maxMs) {
            if (dq.empty()) {
                avgMs = 0.0;
                maxMs = 0.0;
                return;
            }

            int64_t sum = 0;
            int64_t max = 0;
            for (const auto& s : dq) {
                sum += s.us;
                if (s.us > max) {
                    max = s.us;
                }
            }

            avgMs = static_cast<double>(sum) / static_cast<double>(dq.size()) / 1000.0;
            maxMs = static_cast<double>(max) / 1000.0;
        };

        windowStats(decodeSamples_, avgDecode, maxDecode);
        windowStats(displaySamples_, avgDisplay, maxDisplay);
        windowStats(relaySamples_, avgRelay, maxRelay);

        out[0] = static_cast<long>(lastBytesPerSec_);
        out[1] = static_cast<long>(lastChunksPerSec_);
        out[2] = static_cast<long>((lastBytesPerSec_ * 8ULL) / 100000ULL);
        out[3] = static_cast<long>(lastNalsPerSec_);
        out[4] = static_cast<long>(lastAusPerSec_);
        out[5] = static_cast<long>(lastDecodeFramesPerSec_);
        out[6] = static_cast<long>(lastRenderFramesPerSec_);
        out[7] = static_cast<long>(totalFramesDropped_.load());
        out[8] = static_cast<long>(avgDecode * 10.0);
        out[9] = static_cast<long>(maxDecode * 10.0);
        out[10] = static_cast<long>(avgDisplay * 10.0);
        out[11] = static_cast<long>(maxDisplay * 10.0);
        out[12] = static_cast<long>(avgRelay * 10.0);
        out[13] = static_cast<long>(maxRelay * 10.0);
        out[14] = static_cast<long>(clockValid_.load() ? 1 : 0);
        out[15] = static_cast<long>(clockRttUs_.load());
        out[16] = static_cast<long>(clockOffsetUs_.load());
        out[17] = static_cast<long>(codecType_.load());
        out[18] = static_cast<long>(width_.load());
        out[19] = static_cast<long>(height_.load());
        out[20] = static_cast<long>(configureVariant_.load());
        out[21] = static_cast<long>(uiState());
    }

    std::string decoderName() {
        std::lock_guard<std::mutex> lock(nameMutex_);
        if (softwareActive_.load()) {
            return swDecoderName_;
        }
        return decoderName_;
    }

    std::string statusText() {
        std::string decoderNameCopy = decoderName();

        std::string fakeLabel;
        bool fakeActive = fakeSelected_.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(fakeNameMutex_);
            fakeLabel = fakeName_;
        }

        std::lock_guard<std::mutex> statsLock(statsMutex_);

        const int64_t now = nowUs();
        updateCountersLocked(now);
        pruneWindowsLocked(now);

        std::ostringstream out;

        out << "Source: " << sourceName()
            << "   State: " << stateName(uiState())
            << "\n";

        if (source_ == Source::Network) {
            out << "Network: ";
            if (connected_.load()) {
                out << "connected " << host_ << ":" << port_;
            } else {
                out << "connecting/reconnecting";
            }
            out << "\n";
        } else if (source_ == Source::Usb) {
            out << "USB: " << usbStatus_ << "\n";
        } else {
            out << "Selftest: " << (selftestDone_ ? "done" : "running") << "\n";
        }

        const double mbit = static_cast<double>(lastBytesPerSec_ * 8ULL) / 1000000.0;
        out << std::fixed << std::setprecision(1);
        out << "rx " << lastBytesPerSec_ << " B/s  "
            << "chunks " << lastChunksPerSec_ << "/s  "
            << "input " << mbit << " Mbit/s  "
            << "decoded " << lastDecodeFramesPerSec_ << " fps  "
            << "rendered " << lastRenderFramesPerSec_ << " fps\n";

        out << "nals " << totalNals_.load()
            << " aus " << totalAus_.load()
            << " decoded " << totalFramesDecoded_.load()
            << " rendered " << totalFramesRendered_.load()
            << " dropped " << totalFramesDropped_.load() << "\n";

        auto fmt = [&](const std::deque<LatencySample>& dq) -> std::string {
            if (dq.empty()) {
                return "-";
            }

            int64_t sum = 0;
            int64_t max = 0;
            for (const auto& s : dq) {
                sum += s.us;
                if (s.us > max) {
                    max = s.us;
                }
            }

            std::ostringstream o;
            o << std::fixed << std::setprecision(1)
              << (static_cast<double>(sum) / static_cast<double>(dq.size()) / 1000.0)
              << " / "
              << (static_cast<double>(max) / 1000.0);
            return o.str();
        };

        out << "Arrival->decoded avg/max ms: " << fmt(decodeSamples_) << "\n";
        out << "Arrival->displayed avg/max ms: " << fmt(displaySamples_) << "\n";

        if (!clockValid_.load()) {
            out << "Relay clock: no relay clock\n";
        } else {
            out << "Relay clock rtt: "
                << (static_cast<double>(clockRttUs_.load()) / 1000.0)
                << " ms offset: "
                << (static_cast<double>(clockOffsetUs_.load()) / 1000.0)
                << " ms\n";
            out << "Relay->displayed avg/max ms: " << fmt(relaySamples_) << "\n";
        }

        out << "Decoder mode: " << decoderMode_
            << "  hwexp: " << (hwExpString_.empty() ? "-" : hwExpString_)
            << "\n";

        out << "Decoder: " << (decoderNameCopy.empty() ? "-" : decoderNameCopy)
            << "  codec: " << codecMime()
            << "  size: " << width_.load() << "x" << height_.load()
            << "  variant: " << configureVariant_.load()
            << "\n";

        if (fakeActive && !fakeLabel.empty()) {
            out << "fake IDR: yes (" << fakeLabel << ")\n";
        } else if (fakeActive) {
            out << "fake IDR: yes\n";
        } else {
            out << "fake IDR: no\n";
        }

        if (softwareActive_.load()) {
            out << "Software decode avg/max ms: " << fmt(swDecodeSamples_) << "\n";
        }

        return out.str();
    }

    void onFrameRendered(int64_t pts, int64_t renderTimeNanos) {
        if (!running_.load() || pts <= 0) {
            return;
        }

        const int64_t renderUs = renderTimeNanos / 1000LL;
        const int64_t localNowUs = nowUs();

        int64_t displayLatUs = renderUs - pts;
        if (displayLatUs < 0) {
            displayLatUs = 0;
        }

        bool hasRelay = false;
        int64_t relayTs = 0;
        int64_t dequeuedUs = 0;

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pendingRenders_.find(pts);
            if (it != pendingRenders_.end()) {
                hasRelay = it->second.hasRelay;
                relayTs = it->second.relayTs;
                dequeuedUs = it->second.dequeueUs;
                pendingRenders_.erase(it);
            }
        }

        if (dequeuedUs > 0 && renderUs >= dequeuedUs) {
            ivRenderSumUs_.fetch_add(static_cast<uint64_t>(renderUs - dequeuedUs), std::memory_order_relaxed);
            ivRenderCount_.fetch_add(1, std::memory_order_relaxed);
        }

        totalFramesRendered_.fetch_add(1, std::memory_order_relaxed);
        globalDisplaySumUs_.fetch_add(static_cast<uint64_t>(displayLatUs), std::memory_order_relaxed);
        globalDisplayCount_.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            displaySamples_.push_back({localNowUs, displayLatUs});
            renderWindow_.push_back(localNowUs);
            pruneWindowsLocked(localNowUs);
        }

        if (hasRelay && clockValid_.load()) {
            int64_t relayLatUs = renderUs + clockOffsetUs_.load() - relayTs;
            if (relayLatUs >= 0 && relayLatUs < 60000000LL) {
                ivRelaySumUs_.fetch_add(static_cast<uint64_t>(relayLatUs), std::memory_order_relaxed);
                ivRelayCount_.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(statsMutex_);
                relaySamples_.push_back({localNowUs, relayLatUs});
                pruneWindowsLocked(localNowUs);
            }
        }
    }

private:
    enum class Source {
        None,
        Network,
        Usb,
        Selftest,
        Replay,
    };

    struct ConfigurePlan {
        std::string codecName;
        int variant;
    };

    struct LatencySample {
        int64_t t;
        int64_t us;
    };

    struct PendingRender {
        int64_t dequeueUs;
        int64_t relayTs;
        bool hasRelay;
    };

    struct ClockSample {
        int64_t rttUs;
        int64_t offsetUs;
    };

    std::string host_;
    int port_ = 0;
    std::string decoderNameH265_;
    std::string decoderNameH264_;
    std::string selftestPath_;
    Source source_ = Source::None;

    std::atomic<bool> running_{false};
    std::thread receiverThread_;
    std::thread outputThread_;
    std::thread clockThread_;

    int socketFd_ = -1;
    int clockFd_ = -1;

    int usbFd_ = -1;
    int usbCtrlIf_ = 0;
    int usbEpIn_ = 0;
    int usbEpOut_ = 0;
    std::string usbStatus_ = "USB: idle";

    std::vector<uint8_t> rawBuffer_;
    std::vector<uint8_t> currentAu_;
    bool auStarted_ = false;
    bool justStartedFromAud_ = false;
    bool currentAuHasVcl_ = false;
    bool currentAuHasPrefix_ = false;
    bool currentAuHasRelay_ = false;
    bool currentAuHadParam_ = false;
    // Set (rx thread only) when an SPS with new dimensions arrives while the decoder runs; acted on at the next PPS.
    bool reconfigurePending_ = false;
    int lastSpsWidth_ = 0;    // last SPS dimensions seen by the rx thread (width_/height_ are also written by
    int lastSpsHeight_ = 0;   // the output thread from the decoder's output format, so they are not compared)
    bool currentAuSeiInserted_ = false;
    int64_t currentAuArrivalUs_ = 0;
    int64_t currentAuRelayTs_ = 0;
    bool pendingRelayHas_ = false;
    int64_t pendingRelayTs_ = 0;
    int64_t currentChunkArrivalUs_ = 0;

    std::atomic<ANativeWindow*> surface_{nullptr};
    std::atomic<bool> surfaceReady_{false};

    std::atomic<int> codecType_{0};
    std::atomic<bool> paramsReady_{false};
    std::atomic<bool> configured_{false};
    std::atomic<bool> outputThreadStarted_{false};
    std::atomic<bool> outputStopRequested_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> receiving_{false};
    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
    std::atomic<int> configureVariant_{-1};
    std::atomic<bool> clockValid_{false};
    std::atomic<int64_t> clockRttUs_{0};
    std::atomic<int64_t> clockOffsetUs_{0};
    std::atomic<int64_t> lastClockReplyUs_{0};
    std::atomic<bool> selftestActive_{false};
    std::string replayDir_;
    int64_t replayStartMs_ = 0;
    std::atomic<int> replayState_{0};
    std::atomic<bool> replayPaused_{false};
    std::atomic<int64_t> replayPosUs_{0};
    std::atomic<bool> selftestEosSeen_{false};

    std::string decoderMode_ = "auto";
    std::string hwExpString_;
    bool expKeyFrame_ = false;
    bool expRecoverySei_ = false;

    AMediaCodec* codec_ = nullptr;
    std::atomic<bool> softwareActive_{false};
    std::atomic<bool> softwareFallbackRequested_{false};
    std::atomic<bool> hwConfigureFailedPermanent_{false};
    std::atomic<bool> swConfigureFailedPermanent_{false};
    std::atomic<bool> recseiFirstAuDone_{false};
    std::atomic<int64_t> firstAuQueuedUs_{0};
    std::atomic<uint64_t> hwAusQueued_{0};

    std::atomic<bool> fakeSelected_{false};
    std::atomic<bool> pendingFakeInjection_{false};
    std::atomic<int64_t> fakeOutputPts_{-1};
    std::atomic<bool> fakeOutputPending_{false};
    std::atomic<uint64_t> totalFakeIdrInjected_{0};
    std::atomic<uint64_t> totalFakeIdrOutputs_{0};
    std::atomic<int> hevcPocBits_{0};
    FakeIdr activeFake_;
    std::vector<uint8_t> fakeCsd0_;
    std::vector<uint8_t> fakeCsd1_;
    std::mutex fakeNameMutex_;
    std::string fakeName_;

    std::mutex csdMutex_;
    std::vector<uint8_t> vps_;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    std::vector<uint8_t> csd0_;
    std::vector<uint8_t> csd1_;

    std::mutex nameMutex_;
    std::string decoderName_;
    std::string swDecoderName_;

    std::mutex configureMutex_;
    std::atomic<int64_t> lastPtsUs_{0};

    std::atomic<uint64_t> totalRxBytes_{0};
    std::atomic<uint64_t> totalNals_{0};
    std::atomic<uint64_t> totalAus_{0};
    std::atomic<uint64_t> totalFramesDecoded_{0};
    std::atomic<uint64_t> totalFramesRendered_{0};
    std::atomic<uint64_t> totalFramesDropped_{0};

    std::atomic<uint64_t> globalDecodeSumUs_{0};
    std::atomic<uint64_t> globalDecodeCount_{0};
    std::atomic<uint64_t> globalDisplaySumUs_{0};
    // Per-log-interval latency split (reset by maybeLogSummary): first byte of the AU on the socket ->
    // queued to the codec, queued -> output dequeued, dequeued -> on screen, relay (PC) arrival -> on screen.
    std::atomic<uint64_t> ivWireSumUs_{0};
    std::atomic<uint64_t> ivCodecSumUs_{0};
    std::atomic<uint64_t> ivSplitCount_{0};
    std::atomic<uint64_t> ivRenderSumUs_{0};
    std::atomic<uint64_t> ivRenderCount_{0};
    std::atomic<uint64_t> ivRelaySumUs_{0};
    std::atomic<uint64_t> ivRelayCount_{0};
    std::unordered_map<int64_t, int64_t> queuedAtByPts_;   // guarded by relayMapMutex_
    std::atomic<uint64_t> globalDisplayCount_{0};
    std::atomic<uint64_t> globalSwDecodeSumUs_{0};
    std::atomic<uint64_t> globalSwDecodeCount_{0};

    std::mutex statsMutex_;
    std::deque<LatencySample> decodeSamples_;
    std::deque<LatencySample> displaySamples_;
    std::deque<LatencySample> relaySamples_;
    std::deque<LatencySample> swDecodeSamples_;
    std::deque<int64_t> decodeOutputWindow_;
    std::deque<int64_t> renderWindow_;
    std::deque<std::pair<int64_t, uint64_t>> rxBytesWindow_;

    uint64_t lastBytesPerSec_ = 0;
    uint64_t lastChunksPerSec_ = 0;
    uint64_t lastNalsPerSec_ = 0;
    uint64_t lastAusPerSec_ = 0;
    uint64_t lastDecodeFramesPerSec_ = 0;
    uint64_t lastRenderFramesPerSec_ = 0;

    int64_t counterWindowStartUs_ = 0;
    uint64_t startRxBytes_ = 0;
    uint64_t startRxChunks_ = 0;
    uint64_t startNals_ = 0;
    uint64_t startAus_ = 0;
    uint64_t startDecoded_ = 0;
    uint64_t startRendered_ = 0;
    uint64_t rxChunks_ = 0;

    std::atomic<int64_t> lastLogUs_{0};

    std::mutex pendingMutex_;
    std::unordered_map<int64_t, PendingRender> pendingRenders_;

    std::mutex relayMapMutex_;
    std::unordered_map<int64_t, int64_t> queuedRelayByPts_;

    std::mutex clockSamplesMutex_;
    std::vector<ClockSample> clockSamples_;

    bool selftestDone_ = false;
    bool selftestReported_ = false;

    std::vector<ConfigurePlan> configurePlans_;
    int configurePlansCodecType_ = 0;
    std::atomic<int> configurePlanIndex_{0};
    std::atomic<int> configuredPlanIndex_{-1};
    std::atomic<int> configurePlanCount_{0};
    std::atomic<int64_t> lastOutputUs_{0};

    std::string sourceName() const {
        switch (source_) {
            case Source::Network: return "Network";
            case Source::Usb: return "Goggles USB";
            case Source::Selftest: return "Selftest";
            case Source::Replay: return "Replay";
            default: return "None";
        }
    }

    std::string codecMime() const {
        if (codecType_.load() == kCodecTypeH265) {
            return "video/hevc";
        }
        if (codecType_.load() == kCodecTypeH264) {
            return "video/avc";
        }
        return "unknown";
    }

    static std::string stateName(int code) {
        switch (code) {
            case 0: return "Disconnected";
            case 1: return "Connecting";
            case 2: return "Receiving";
            case 3: return "Decoding";
            default: return "Unknown";
        }
    }

    size_t findStartCode(size_t searchFrom, size_t* outLen) const {
        if (rawBuffer_.size() < 3 || searchFrom + 2 >= rawBuffer_.size()) {
            return kNotFound;
        }

        for (size_t i = searchFrom; i + 2 < rawBuffer_.size(); ++i) {
            if (rawBuffer_[i] == 0 &&
                rawBuffer_[i + 1] == 0 &&
                rawBuffer_[i + 2] == 1) {

                size_t p = i;
                while (p > searchFrom && rawBuffer_[p - 1] == 0) {
                    --p;
                }

                if (outLen != nullptr) {
                    if (p + 3 < rawBuffer_.size() &&
                        rawBuffer_[p] == 0 &&
                        rawBuffer_[p + 1] == 0 &&
                        rawBuffer_[p + 2] == 0 &&
                        rawBuffer_[p + 3] == 1) {
                        *outLen = 4;
                    } else {
                        *outLen = 3;
                    }
                }

                return p;
            }
        }

        return kNotFound;
    }

    static std::vector<uint8_t> withStartCode(const uint8_t* nal, size_t size) {
        static const uint8_t kStartCode[4] = {0, 0, 0, 1};
        std::vector<uint8_t> out;
        out.reserve(size + sizeof(kStartCode));
        out.insert(out.end(), kStartCode, kStartCode + sizeof(kStartCode));
        out.insert(out.end(), nal, nal + size);
        return out;
    }

    std::vector<uint8_t> makeRecoverySeiNal(int codec) const {
        std::vector<uint8_t> raw;

        if (codec == kCodecTypeH265) {
            raw.push_back(0x4e);
            raw.push_back(0x01);
            raw.push_back(0x06);
            raw.push_back(0x01);
            raw.push_back(0xc0);
            raw.push_back(0x80);
        } else if (codec == kCodecTypeH264) {
            raw.push_back(0x06);
            raw.push_back(0x06);
            raw.push_back(0x01);
            raw.push_back(0xc0);
            raw.push_back(0x80);
        }

        std::vector<uint8_t> nal = applyEmulationPrevention(raw);
        return withStartCode(nal.data(), nal.size());
    }

    void pushStream(const uint8_t* data, size_t len, int64_t arrivalUs) {
        if (!running_.load() || data == nullptr || len == 0) {
            return;
        }

        currentChunkArrivalUs_ = arrivalUs;
        receiving_.store(true, std::memory_order_relaxed);

        if (source_ == Source::Network || source_ == Source::Usb) {
            g_recorder.write(data, len);
        }

        totalRxBytes_.fetch_add(len, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            rxBytesWindow_.emplace_back(arrivalUs, len);
            rxChunks_++;
        }

        rawBuffer_.insert(rawBuffer_.end(), data, data + len);

        if (rawBuffer_.size() > 8u * 1024u * 1024u) {
            const size_t keep = 2u * 1024u * 1024u;
            rawBuffer_.erase(rawBuffer_.begin(), rawBuffer_.begin() + (rawBuffer_.size() - keep));
        }

        parseBuffer();
    }

    void resetCurrentAccessUnit() {
        currentAu_.clear();
        auStarted_ = false;
        justStartedFromAud_ = false;
        currentAuHasVcl_ = false;
        currentAuHasPrefix_ = false;
        currentAuHasRelay_ = false;
        currentAuHadParam_ = false;
        currentAuSeiInserted_ = false;
        currentAuArrivalUs_ = 0;
        currentAuRelayTs_ = 0;
    }

    void resetStreamState() {
        rawBuffer_.clear();
        resetCurrentAccessUnit();
        pendingRelayHas_ = false;
        pendingRelayTs_ = 0;
        currentChunkArrivalUs_ = 0;
    }

    void flushParser() {
        processTrailingNal();

        if (!currentAu_.empty() && currentAuHasVcl_) {
            feedAu(currentAu_, currentAuArrivalUs_, currentAuHasRelay_, currentAuRelayTs_);
            totalAus_.fetch_add(1, std::memory_order_relaxed);
        }

        resetCurrentAccessUnit();
        rawBuffer_.clear();
        pendingRelayHas_ = false;
        pendingRelayTs_ = 0;
    }

    void processTrailingNal() {
        if (rawBuffer_.empty()) {
            return;
        }

        size_t startCodeLen = 0;
        const size_t start = findStartCode(0, &startCodeLen);
        if (start == kNotFound) {
            return;
        }

        const size_t nalStart = start + startCodeLen;
        if (nalStart < rawBuffer_.size()) {
            processNal(rawBuffer_.data() + nalStart, rawBuffer_.size() - nalStart);
        }
    }

    void maybeAssignPendingRelay() {
        if (pendingRelayHas_ && !currentAuHasRelay_) {
            currentAuHasRelay_ = true;
            currentAuRelayTs_ = pendingRelayTs_;
            pendingRelayHas_ = false;
            pendingRelayTs_ = 0;
        }
    }

    void appendNalToAu(const uint8_t* nal, size_t size, bool isVcl) {
        if (currentAu_.empty() && currentAuArrivalUs_ == 0) {
            currentAuArrivalUs_ = currentChunkArrivalUs_ != 0 ? currentChunkArrivalUs_ : nowUs();
        }

        maybeAssignPendingRelay();

        if (expRecoverySei_ && !currentAuSeiInserted_ && isVcl) {
            bool shouldInsert = !recseiFirstAuDone_.load();
            if (!shouldInsert) {
                shouldInsert = currentAuHadParam_;
            }

            if (shouldInsert) {
                const std::vector<uint8_t> sei = makeRecoverySeiNal(codecType_.load());
                if (!sei.empty()) {
                    currentAu_.insert(currentAu_.end(), sei.begin(), sei.end());
                    currentAuSeiInserted_ = true;
                    recseiFirstAuDone_.store(true, std::memory_order_relaxed);
                }
            }
        }

        const std::vector<uint8_t> nalWithSc = withStartCode(nal, size);
        currentAu_.insert(currentAu_.end(), nalWithSc.begin(), nalWithSc.end());

        if (isVcl) {
            currentAuHasVcl_ = true;
        } else {
            currentAuHasPrefix_ = true;
        }
    }

    void startNewAccessUnit() {
        const bool hadVcl = currentAuHasVcl_;
        const bool hadParam = currentAuHadParam_;

        if (!currentAu_.empty() && currentAuHasVcl_) {
            feedAu(currentAu_, currentAuArrivalUs_, currentAuHasRelay_, currentAuRelayTs_);
            totalAus_.fetch_add(1, std::memory_order_relaxed);
        }

        currentAu_.clear();
        currentAuHasVcl_ = false;
        currentAuHasPrefix_ = false;
        currentAuHasRelay_ = false;
        currentAuHadParam_ = (!hadVcl && hadParam);
        currentAuSeiInserted_ = false;
        currentAuRelayTs_ = 0;
        currentAuArrivalUs_ = 0;
        auStarted_ = true;
        justStartedFromAud_ = false;

        currentAuHasRelay_ = pendingRelayHas_;
        currentAuRelayTs_ = pendingRelayTs_;
        pendingRelayHas_ = false;
        pendingRelayTs_ = 0;
    }

    // Length of the AUD at nal (header included) if the buffer holds a complete one, else 0.
    // H.265: 2-byte NAL header + 1 byte (pic_type + stop bit). H.264: 1-byte header + 1 byte.
    size_t fixedAudLength(const uint8_t* nal, size_t avail) const {
        const int codec = codecType_.load();
        if (codec == kCodecTypeH265) {
            return (avail >= 3 && ((nal[0] >> 1) & 0x3f) == 35) ? 3 : 0;
        }
        if (codec == kCodecTypeH264) {
            return (avail >= 2 && (nal[0] & 0x1f) == 9) ? 2 : 0;
        }
        return 0;
    }

    void parseBuffer() {
        size_t pos = 0;

        while (pos < rawBuffer_.size()) {
            size_t startCodeLen = 0;
            size_t start = findStartCode(pos, &startCodeLen);

            if (start == kNotFound) {
                if (pos > 0) {
                    rawBuffer_.erase(rawBuffer_.begin(), rawBuffer_.begin() + pos);
                }
                break;
            }

            if (start > pos) {
                rawBuffer_.erase(rawBuffer_.begin(), rawBuffer_.begin() + start);
                pos = 0;
            } else {
                pos = start;
            }

            const size_t nalStart = pos + startCodeLen;
            if (nalStart + 1 >= rawBuffer_.size()) {
                break;
            }

            size_t nextCodeLen = 0;
            size_t next = findStartCode(nalStart, &nextCodeLen);
            if (next == kNotFound) {
                // An access unit delimiter has a fixed size, so it is complete without the next start code.
                // The goggles send it right after the frame it closes (measured: 0.00 ms median), so acting
                // on it now hands the finished frame to the decoder one frame interval earlier than waiting
                // for the next frame's data to terminate the NAL.
                const size_t audLen = fixedAudLength(rawBuffer_.data() + nalStart, rawBuffer_.size() - nalStart);
                if (audLen > 0) {
                    processNal(rawBuffer_.data() + nalStart, audLen);
                    rawBuffer_.erase(rawBuffer_.begin(), rawBuffer_.begin() + nalStart + audLen);
                    pos = 0;
                    continue;
                }
                break;
            }

            if (next > nalStart) {
                processNal(rawBuffer_.data() + nalStart, next - nalStart);
            }

            pos = next;

            if (pos > 1024) {
                rawBuffer_.erase(rawBuffer_.begin(), rawBuffer_.begin() + pos);
                pos = 0;
            }
        }

        if (!rawBuffer_.empty() && pos > 0 && pos < rawBuffer_.size()) {
            rawBuffer_.erase(rawBuffer_.begin(), rawBuffer_.begin() + pos);
        }
    }

    static int detectCodec(const uint8_t* nal, size_t size) {
        if (size == 0) {
            return 0;
        }

        const uint8_t b0 = nal[0];

        if (b0 == 0x06 || b0 == 0x09) {
            return kCodecTypeH264;
        }

        const int h264Type = (b0 & 0x1f);
        if (h264Type == 7 || h264Type == 8) {
            return kCodecTypeH264;
        }

        if (h264Type >= 1 && h264Type <= 5 && b0 >= 0x61 && b0 <= 0x65) {
            return kCodecTypeH264;
        }

        if (size >= 2) {
            const int h265Type = ((nal[0] >> 1) & 0x3f);
            if (h265Type == 32 ||
                h265Type == 33 ||
                h265Type == 34 ||
                h265Type == 35 ||
                h265Type == 39 ||
                h265Type == 40) {
                return kCodecTypeH265;
            }
        }

        if (b0 == 0x4E || b0 == 0x4F || b0 == 0x50 || b0 == 0x46) {
            return kCodecTypeH265;
        }

        return 0;
    }

    bool parseRelaySei(const uint8_t* nal, size_t size, int codec, int64_t& relayUs) {
        if (nal == nullptr || size < 1) {
            return false;
        }

        int type = -1;
        size_t headerSize = 0;

        if (codec == kCodecTypeH265) {
            if (size < 2) {
                return false;
            }
            type = ((nal[0] >> 1) & 0x3f);
            headerSize = 2;
            if (type != 39) {
                return false;
            }
        } else if (codec == kCodecTypeH264) {
            type = (nal[0] & 0x1f);
            headerSize = 1;
            if (type != 6) {
                return false;
            }
        } else {
            return false;
        }

        std::vector<uint8_t> rbsp;
        rbsp.reserve(size - headerSize);

        for (size_t i = headerSize; i < size; ) {
            if (i + 2 < size && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3) {
                rbsp.push_back(0);
                rbsp.push_back(0);
                i += 3;
            } else {
                rbsp.push_back(nal[i]);
                i++;
            }
        }

        static const uint8_t kUuid[16] = {
            0x51, 0x52, 0x4C, 0x59, 0x2D, 0x54, 0x53, 0x2D,
            0x76, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
        };

        size_t idx = 0;
        while (idx + 1 < rbsp.size()) {
            const uint8_t seiType = rbsp[idx++];

            int payloadSize = 0;
            while (idx < rbsp.size() && rbsp[idx] == 0xff) {
                payloadSize += 255;
                idx++;
            }

            if (idx >= rbsp.size()) {
                break;
            }

            payloadSize += rbsp[idx++];
            if (payloadSize <= 0) {
                continue;
            }

            if (idx + static_cast<size_t>(payloadSize) > rbsp.size()) {
                break;
            }

            if (seiType == 5 && payloadSize >= 28) {
                const uint8_t* p = rbsp.data() + idx;
                if (std::memcmp(p, kUuid, sizeof(kUuid)) == 0) {
                    relayUs = static_cast<int64_t>(getBE64(p + 16));
                    return true;
                }
            }

            idx += static_cast<size_t>(payloadSize);
        }

        return false;
    }

    void storeCsd(int codec, int nalType, const uint8_t* nal, size_t size) {
        if (codec == kCodecTypeH264 && nalType == 7) {
            int w = 0;
            int h = 0;
            if (parseH264SpsDimensions(nal, size, w, h)) {
                width_.store(w, std::memory_order_relaxed);
                height_.store(h, std::memory_order_relaxed);
            }
        } else if (codec == kCodecTypeH265 && nalType == 33) {
            int w = 0;
            int h = 0;
            int pocBits = 0;
            if (parseH265SpsDimensions(nal, size, w, h, pocBits)) {
                width_.store(w, std::memory_order_relaxed);
                height_.store(h, std::memory_order_relaxed);
                hevcPocBits_.store(pocBits, std::memory_order_relaxed);
            }
        }

        std::vector<uint8_t> nalWithSc = withStartCode(nal, size);

        std::lock_guard<std::mutex> lock(csdMutex_);

        if (codec == kCodecTypeH265 && nalType >= 32 && nalType <= 34) {
            g_recorder.setParamSet(nalType - 32, nalWithSc.data(), nalWithSc.size(), true);
        } else if (codec == kCodecTypeH264 && (nalType == 7 || nalType == 8)) {
            g_recorder.setParamSet(nalType == 7 ? 1 : 2, nalWithSc.data(), nalWithSc.size(), false);
        }

        if (codec == kCodecTypeH265) {
            if (nalType == 32) {
                vps_ = std::move(nalWithSc);
            } else if (nalType == 33) {
                sps_ = std::move(nalWithSc);
            } else if (nalType == 34) {
                pps_ = std::move(nalWithSc);
            }

            if (!vps_.empty() && !sps_.empty() && !pps_.empty()) {
                csd0_.clear();
                csd0_.insert(csd0_.end(), vps_.begin(), vps_.end());
                csd0_.insert(csd0_.end(), sps_.begin(), sps_.end());
                csd0_.insert(csd0_.end(), pps_.begin(), pps_.end());
                paramsReady_.store(true);
            }
        } else if (codec == kCodecTypeH264) {
            if (nalType == 7) {
                csd0_ = std::move(nalWithSc);
            } else if (nalType == 8) {
                csd1_ = std::move(nalWithSc);
            }

            if (!csd0_.empty() && !csd1_.empty()) {
                paramsReady_.store(true);
            }
        }
    }

    void processNal(const uint8_t* nal, size_t size) {
        if (!running_.load() || size == 0 || nal == nullptr) {
            return;
        }

        totalNals_.fetch_add(1, std::memory_order_relaxed);

        int codec = codecType_.load();
        if (codec == 0) {
            codec = detectCodec(nal, size);
            if (codec != 0) {
                codecType_.store(codec);
                LOGI("auto-detected codec %s", codec == kCodecTypeH265 ? "video/hevc" : "video/avc");
            }
        }

        if (codec == 0) {
            return;
        }

        // The goggles change codec on the fly (D-Log M is H.264, the normal modes H.265). The codec is otherwise
        // detected once per connection, so an H.264 stream after H.265 was parsed with H.265 NAL types: no AUs,
        // decoder frozen on the last frame. Switch on an unambiguous parameter-set header of the other codec:
        // an H.264 SPS byte (0x27/0x47/0x67) is odd, which as H.265 would mean nuh_layer_id >= 32 (single-layer
        // streams always have an even first byte); the H.265 VPS header 40 01 is NAL type 0 in H.264, never valid.
        const int switchTo =
            (codec == kCodecTypeH265 && (nal[0] == 0x27 || nal[0] == 0x47 || nal[0] == 0x67)) ? kCodecTypeH264
            : (codec == kCodecTypeH264 && size >= 2 && nal[0] == 0x40 && nal[1] == 0x01)       ? kCodecTypeH265
                                                                                                 : 0;
        if (switchTo != 0) {
            LOGI("stream codec changed %s -> %s; decoder reset",
                 codec == kCodecTypeH265 ? "video/hevc" : "video/avc",
                 switchTo == kCodecTypeH265 ? "video/hevc" : "video/avc");
            resetDecoderForReconnect();          // codec, csd, fake IDR state; not the socket or parse buffers
            codecType_.store(switchTo);
            codec = switchTo;
            currentAu_.clear();                  // holds the old codec's AUD/SEI
            auStarted_ = false;
            currentAuHasVcl_ = false;
            currentAuHadParam_ = false;
            reconfigurePending_ = false;
            lastSpsWidth_ = 0;
            lastSpsHeight_ = 0;
        }

        int type = -1;
        bool isParam = false;
        bool isVcl = false;
        bool isAud = false;
        bool isPrefixSei = false;

        if (codec == kCodecTypeH265) {
            if (size < 2) {
                return;
            }

            type = (nal[0] >> 1) & 0x3f;
            isParam = (type == 32 || type == 33 || type == 34);
            isVcl = (type <= 31);
            isAud = (type == 35);
            isPrefixSei = (type == 39);
        } else {
            type = nal[0] & 0x1f;
            isParam = (type == 7 || type == 8);
            isVcl = (type >= 1 && type <= 5);
            isAud = (type == 9);
            isPrefixSei = (type == 6);
        }

        if (isPrefixSei) {
            int64_t relayTs = 0;
            if (parseRelaySei(nal, size, codec, relayTs)) {
                if (auStarted_ && !currentAu_.empty()) {
                    currentAuHasRelay_ = true;
                    currentAuRelayTs_ = relayTs;
                } else {
                    pendingRelayHas_ = true;
                    pendingRelayTs_ = relayTs;
                }
            }
        }

        if (isParam) {
            currentAuHadParam_ = true;
            const bool isSps = (codec == kCodecTypeH265) ? (type == 33) : (type == 7);
            const bool isPps = (codec == kCodecTypeH265) ? (type == 34) : (type == 8);
            storeCsd(codec, type, nal, size);

            // Parameter sets only reach the decoder as csd-0/csd-1 at configure time, never in-band, so a mode
            // switch (the goggles go 1832x1032p30 <-> 1920x1080p60 on arm/disarm) left the running decoder on the
            // old SPS and it decoded the new slices into garbage. Restart it once the new SPS/PPS pair is complete;
            // the goggles follow the switch with a real IDR. Repeated SPS with the same size do nothing.
            if (isSps) {
                const int newW = width_.load(std::memory_order_relaxed);   // just set by storeCsd from this SPS
                const int newH = height_.load(std::memory_order_relaxed);
                if (configured_.load() && lastSpsWidth_ > 0 && (newW != lastSpsWidth_ || newH != lastSpsHeight_)) {
                    LOGI("stream resolution changed %dx%d -> %dx%d; decoder restart at next PPS",
                         lastSpsWidth_, lastSpsHeight_, newW, newH);
                    reconfigurePending_ = true;
                }
                lastSpsWidth_ = newW;
                lastSpsHeight_ = newH;
            }
            if (isPps && reconfigurePending_) {
                reconfigurePending_ = false;
                std::lock_guard<std::mutex> lock(configureMutex_);
                configured_.store(false, std::memory_order_release);
                tryConfigureLocked();   // stops the old codec, picks a fake IDR for the new size, configures
                LOGI("decoder restarted for %dx%d: %s", lastSpsWidth_, lastSpsHeight_,
                     configured_.load() ? "ok" : "not configured");
                return;
            }

            tryConfigure();
            return;
        }

        bool newAu = false;

        if (codec == kCodecTypeH265) {
            if (isAud) {
                newAu = true;
            } else if (isVcl) {
                if (!auStarted_) {
                    newAu = true;
                } else if (!justStartedFromAud_ && size >= 3 && (nal[2] & 0x80)) {
                    if (currentAuHasVcl_ || currentAu_.empty()) {
                        newAu = true;
                    }
                }
            }
        } else {
            if (isAud) {
                newAu = true;
            } else if (isVcl) {
                if (!auStarted_) {
                    newAu = true;
                } else if (!justStartedFromAud_ && size >= 2 && (nal[1] & 0x80)) {
                    if (currentAuHasVcl_ || currentAu_.empty()) {
                        newAu = true;
                    }
                }
            }
        }

        if (newAu) {
            startNewAccessUnit();
        }

        if (isAud) {
            justStartedFromAud_ = true;
        }

        if (isVcl) {
            appendNalToAu(nal, size, isVcl);
        }
    }

    void queueRelayForPts(int64_t pts, int64_t relayTs) {
        std::lock_guard<std::mutex> lock(relayMapMutex_);
        queuedRelayByPts_[pts] = relayTs;
        if (queuedRelayByPts_.size() > 2048) {
            queuedRelayByPts_.clear();
        }
    }

    bool takeQueuedRelay(int64_t pts, int64_t& relayTs) {
        std::lock_guard<std::mutex> lock(relayMapMutex_);
        auto it = queuedRelayByPts_.find(pts);
        if (it == queuedRelayByPts_.end()) {
            return false;
        }
        relayTs = it->second;
        queuedRelayByPts_.erase(it);
        return true;
    }

    int64_t nextPts(int64_t arrivalUs) {
        int64_t pts = arrivalUs > 0 ? arrivalUs : nowUs();
        int64_t last = lastPtsUs_.load(std::memory_order_relaxed);
        if (pts <= last) {
            pts = last + 1;
        }
        lastPtsUs_.store(pts, std::memory_order_relaxed);
        return pts;
    }

    void stopCodecLocked() {
        outputStopRequested_.store(true, std::memory_order_release);

        if (outputThread_.joinable()) {
            outputThread_.join();
        }

        if (codec_ != nullptr) {
            AMediaCodec_stop(codec_);
            AMediaCodec_delete(codec_);
            codec_ = nullptr;
        }

        configured_.store(false, std::memory_order_release);
        outputThreadStarted_.store(false, std::memory_order_release);
        outputStopRequested_.store(false, std::memory_order_release);
        softwareActive_.store(false, std::memory_order_release);
        configureVariant_.store(-1, std::memory_order_relaxed);
        configuredPlanIndex_.store(-1, std::memory_order_relaxed);
    }

    void buildConfigurePlansLocked() {
        configurePlans_.clear();
        const int codecType = codecType_.load();
        configurePlansCodecType_ = codecType;

        const std::string requested =
            (codecType == kCodecTypeH265)
                ? decoderNameH265_
                : ((codecType == kCodecTypeH264) ? decoderNameH264_ : std::string());

        auto addPlan = [&](const std::string& name, int variant) {
            for (const auto& p : configurePlans_) {
                if (p.codecName == name && p.variant == variant) {
                    return;
                }
            }
            configurePlans_.push_back({name, variant});
        };

        std::string noLow = requested;
        const std::string suffix = ".low_latency";
        if (endsWith(noLow, suffix)) {
            noLow = noLow.substr(0, noLow.size() - suffix.size());
        }

        // Prefer the chosen decoder (normally c2.qti.*.low_latency, FEATURE_LowLatency) now that the fake
        // keyframe makes the hardware path work; the standard component is only a fallback.
        if (!requested.empty() && requested != noLow) {
            addPlan(requested, 2);
            addPlan(requested, 3);
            addPlan(requested, 1);
            addPlan(requested, 0);
        }

        addPlan(noLow, 2);
        addPlan(noLow, 3);
        addPlan(noLow, 1);
        addPlan(noLow, 0);

        addPlan(std::string(), 2);
        addPlan(std::string(), 3);
        addPlan(std::string(), 1);
        addPlan(std::string(), 0);

        if (codecType == kCodecTypeH265) {
            addPlan("c2.android.hevc.decoder", 2);
            addPlan("c2.android.hevc.decoder", 0);
        } else if (codecType == kCodecTypeH264) {
            addPlan("c2.android.avc.decoder", 2);
            addPlan("c2.android.avc.decoder", 0);
        }

        if (!requested.empty() && requested != noLow) {
            addPlan(requested, 2);
        }
    }

    // The FFmpeg software decoder was removed: decoding is hardware only. A software fallback request is refused.
    void configureSoftwareLocked() {
        swConfigureFailedPermanent_.store(true, std::memory_order_relaxed);
        softwareFallbackRequested_.store(false, std::memory_order_release);
        LOGW("software decoding is not available (hardware only)");
    }

    bool queueInputBlobLocked(const std::vector<uint8_t>& blob, int64_t pts, uint32_t flags) {
        if (codec_ == nullptr || outputStopRequested_.load()) {
            return false;
        }

        ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 2000);
        if (idx < 0) {
            return false;
        }

        size_t bufSize = 0;
        void* buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &bufSize);
        if (buf == nullptr || blob.size() > bufSize) {
            releaseInputBufferSafely(codec_, static_cast<size_t>(idx));
            return false;
        }

        std::memcpy(buf, blob.data(), blob.size());

        const media_status_t status =
            AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, blob.size(), pts, flags);

        if (status != AMEDIA_OK) {
            releaseInputBufferSafely(codec_, static_cast<size_t>(idx));
            return false;
        }

        return true;
    }

    std::vector<uint8_t> buildLiveAuWithCsdLocked(const std::vector<uint8_t>& au) {
        std::vector<uint8_t> packet;
        packet.reserve(au.size() + 256);

        std::lock_guard<std::mutex> lock(csdMutex_);
        const int codecType = codecType_.load();

        if (codecType == kCodecTypeH264) {
            packet.insert(packet.end(), csd0_.begin(), csd0_.end());
            packet.insert(packet.end(), csd1_.begin(), csd1_.end());
        } else if (codecType == kCodecTypeH265) {
            packet.insert(packet.end(), csd0_.begin(), csd0_.end());
        }

        packet.insert(packet.end(), au.begin(), au.end());
        return packet;
    }

    bool injectFakeLocked(const std::vector<uint8_t>& au) {
        if (activeFake_.blob.empty() || codec_ == nullptr || outputStopRequested_.load()) {
            return false;
        }

        std::vector<uint8_t> blob = activeFake_.blob;
        const int codec = codecType_.load();
        const FakeIdrSidecar sidecar = activeFake_.sidecar;

        if (codec == kCodecTypeH265 && sidecar.valid) {
            int extraBits = 0;
            bool outputFlag = false;
            std::vector<uint8_t> ppsCopy;

            {
                std::lock_guard<std::mutex> csdLock(csdMutex_);
                ppsCopy = pps_;
            }

            (void)parseH265PpsFlagsFromAnnexB(ppsCopy, extraBits, outputFlag);

            int pocBits = sidecar.pocLsbBits;
            if (pocBits <= 0) {
                pocBits = hevcPocBits_.load(std::memory_order_relaxed);
            }

            int pocLsb = 0;
            if (pocBits > 0) {
                if (!parseHevcFirstSlicePocLsb(au.data(), au.size(), extraBits, outputFlag, pocBits, pocLsb)) {
                    LOGW("fake IDR: failed to read first live HEVC POC lsb");
                }
            }

            patchHevcFakeIdr(blob, sidecar, pocLsb);
        }

        const int64_t pts = nextPts(nowUs());
        fakeOutputPts_.store(pts, std::memory_order_release);
        fakeOutputPending_.store(true, std::memory_order_release);
        totalFakeIdrInjected_.fetch_add(1, std::memory_order_relaxed);

        if (!queueInputBlobLocked(blob, pts, kBufferFlagKeyFrame)) {
            fakeOutputPending_.store(false, std::memory_order_release);
            return false;
        }

        if (firstAuQueuedUs_.load(std::memory_order_relaxed) == 0) {
            firstAuQueuedUs_.store(nowUs(), std::memory_order_relaxed);
        }

        std::string fn;
        {
            std::lock_guard<std::mutex> lock(fakeNameMutex_);
            fn = fakeName_;
        }

        LOGI("fake IDR injected: %s", fn.empty() ? "fake" : fn.c_str());
        return true;
    }

    void tryConfigureLocked() {
        if (!running_.load()) {
            return;
        }

        if (configured_.load()) {
            return;
        }

        if (swConfigureFailedPermanent_.load()) {
            return;
        }

        if (!paramsReady_.load()) {
            return;
        }

        ANativeWindow* surface = surface_.load();
        if (surface == nullptr) {
            return;
        }

        stopCodecLocked();

        fakeSelected_.store(false, std::memory_order_release);
        pendingFakeInjection_.store(false, std::memory_order_release);
        fakeOutputPending_.store(false, std::memory_order_release);
        fakeOutputPts_.store(-1, std::memory_order_release);
        activeFake_ = FakeIdr();
        fakeCsd0_.clear();
        fakeCsd1_.clear();
        {
            std::lock_guard<std::mutex> lock(fakeNameMutex_);
            fakeName_.clear();
        }

        if (decoderMode_ == "sw" || softwareFallbackRequested_.load()) {
            configureSoftwareLocked();
            return;
        }

        if (hwConfigureFailedPermanent_.load()) {
            if (decoderMode_ == "auto") {
                configureSoftwareLocked();
            }
            return;
        }

        const int codecType = codecType_.load();
        const std::string mime = (codecType == kCodecTypeH265) ? "video/hevc" : "video/avc";

        if (configurePlans_.empty() || configurePlansCodecType_ != codecType) {
            buildConfigurePlansLocked();
        }

        configurePlanCount_.store(static_cast<int>(configurePlans_.size()), std::memory_order_relaxed);
        configurePlanIndex_.store(0, std::memory_order_relaxed);

        bool fakeMatch = false;
        std::string fakeNameCopy;
        const int liveWidth = width_.load(std::memory_order_relaxed);
        const int liveHeight = height_.load(std::memory_order_relaxed);

        if (codecType == kCodecTypeH265 || codecType == kCodecTypeH264) {
            int fakeIdx = findFakeIdrIndex(codecType, liveWidth, liveHeight);
            if (fakeIdx >= 0) {
                FakeIdr entry;
                {
                    std::lock_guard<std::mutex> lock(g_fakeIdrMutex);
                    entry = g_fakeIdrs[static_cast<size_t>(fakeIdx)];
                }

                if (extractFakeCsd(entry.blob, codecType, fakeCsd0_, fakeCsd1_)) {
                    activeFake_ = entry;
                    fakeMatch = true;
                    fakeNameCopy = entry.name;
                } else {
                    activeFake_ = FakeIdr();
                    fakeCsd0_.clear();
                    fakeCsd1_.clear();
                }
            }
        }

        if (fakeMatch) {
            fakeSelected_.store(true, std::memory_order_release);
            pendingFakeInjection_.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(fakeNameMutex_);
                fakeName_ = fakeNameCopy;
            }
            LOGI("fake IDR selected: %s", fakeNameCopy.c_str());
        } else {
            activeFake_ = FakeIdr();
            fakeCsd0_.clear();
            fakeCsd1_.clear();
        }

        for (int idx = 0; idx < static_cast<int>(configurePlans_.size()); ++idx) {
            const ConfigurePlan& plan = configurePlans_[idx];

            AMediaCodec* c = nullptr;
            bool usedRequestedName = false;

            if (!plan.codecName.empty()) {
                c = AMediaCodec_createCodecByName(plan.codecName.c_str());
                if (c == nullptr) {
                    LOGW("createCodecByName failed for %s, fallback to createDecoderByType",
                         plan.codecName.c_str());
                } else {
                    usedRequestedName = true;
                }
            }

            if (c == nullptr) {
                c = AMediaCodec_createDecoderByType(mime.c_str());
            }

            if (c == nullptr) {
                LOGW("could not create codec for mime=%s name=%s",
                     mime.c_str(), plan.codecName.c_str());
                continue;
            }

            AMediaFormat* format = AMediaFormat_new();
            if (format == nullptr) {
                AMediaCodec_delete(c);
                LOGE("AMediaFormat_new failed");
                continue;
            }

            {
                std::lock_guard<std::mutex> csdLock(csdMutex_);
                const bool useFakeCsd = fakeMatch && !fakeCsd0_.empty();

                AMediaFormat_setString(format, "mime", mime.c_str());
                AMediaFormat_setInt32(format, "width", kDefaultWidth);
                AMediaFormat_setInt32(format, "height", kDefaultHeight);
                AMediaFormat_setInt32(format, "max-width", kMaxWidth);
                AMediaFormat_setInt32(format, "max-height", kMaxHeight);
                AMediaFormat_setInt32(format, "max-input-size", kMaxInputSize);

                AMediaFormat_setInt32(format, "allow-decoding-without-keyframe", 1);
                AMediaFormat_setInt32(format, "latency", 0);
                if (hwExpString_.find("dropcorrupt") != std::string::npos) {
                    AMediaFormat_setInt32(format, "vendor.qti-ext-dec-drop-corrupt.value", 0);
                    LOGI("hwexp: vendor.qti-ext-dec-drop-corrupt.value=0");
                }

                if (plan.variant >= 1) {
                    AMediaFormat_setInt32(format, "low-latency", 1);
                }

                if (plan.variant >= 2) {
                    AMediaFormat_setInt32(format, "vendor.qti-ext-dec-low-latency.enable", 1);
                    AMediaFormat_setInt32(format, "priority", 0);
                    // Output in decode order: no reorder wait. The goggles stream has no B-frames, so decode
                    // order == display order. hwexp "nopicorder" turns it off for A/B runs.
                    if (hwExpString_.find("nopicorder") == std::string::npos) {
                        AMediaFormat_setInt32(format, "vendor.qti-ext-dec-picture-order.enable", 1);
                    }
                }

                if (plan.variant == 3) {
                    AMediaFormat_setInt32(format, "operating-rate", 32767);
                }

                if (codecType == kCodecTypeH265) {
                    if (useFakeCsd) {
                        if (!fakeCsd0_.empty()) {
                            AMediaFormat_setBuffer(format, "csd-0", fakeCsd0_.data(), fakeCsd0_.size());
                        }
                    } else if (!csd0_.empty()) {
                        AMediaFormat_setBuffer(format, "csd-0", csd0_.data(), csd0_.size());
                    }
                } else if (codecType == kCodecTypeH264) {
                    if (useFakeCsd) {
                        if (!fakeCsd0_.empty()) {
                            AMediaFormat_setBuffer(format, "csd-0", fakeCsd0_.data(), fakeCsd0_.size());
                        }
                        if (!fakeCsd1_.empty()) {
                            AMediaFormat_setBuffer(format, "csd-1", fakeCsd1_.data(), fakeCsd1_.size());
                        }
                    } else {
                        if (!csd0_.empty()) {
                            AMediaFormat_setBuffer(format, "csd-0", csd0_.data(), csd0_.size());
                        }
                        if (!csd1_.empty()) {
                            AMediaFormat_setBuffer(format, "csd-1", csd1_.data(), csd1_.size());
                        }
                    }
                }
            }

            const media_status_t configureStatus =
                AMediaCodec_configure(c, format, surface, nullptr, 0);
            AMediaFormat_delete(format);

            if (configureStatus != AMEDIA_OK) {
                LOGW("configure failed variant=%d plan=%d status=%d name=%s mime=%s",
                     plan.variant,
                     idx,
                     static_cast<int>(configureStatus),
                     plan.codecName.c_str(),
                     mime.c_str());
                AMediaCodec_delete(c);
                continue;
            }

            const media_status_t startStatus = AMediaCodec_start(c);
            if (startStatus != AMEDIA_OK) {
                LOGW("codec start failed: %d", static_cast<int>(startStatus));
                AMediaCodec_delete(c);
                continue;
            }

            const media_status_t frameCbStatus =
                AMediaCodec_setOnFrameRenderedCallback(c, questVisorFrameRenderedCallback, this);
            if (frameCbStatus != AMEDIA_OK) {
                LOGW("setOnFrameRenderedCallback failed: %d", static_cast<int>(frameCbStatus));
            }

            if (outputThread_.joinable()) {
                outputStopRequested_.store(true, std::memory_order_release);
                outputThread_.join();
                outputStopRequested_.store(false, std::memory_order_release);
            }

            codec_ = c;
            configureVariant_.store(plan.variant, std::memory_order_relaxed);
            configurePlanIndex_.store(idx, std::memory_order_relaxed);
            configuredPlanIndex_.store(idx, std::memory_order_relaxed);
            firstAuQueuedUs_.store(0, std::memory_order_relaxed);
            hwAusQueued_.store(0, std::memory_order_relaxed);

            std::string actualName;
            if (usedRequestedName) {
                actualName = plan.codecName;
            } else {
                actualName = mime + " (default)";
            }

            {
                std::lock_guard<std::mutex> nameLock(nameMutex_);
                decoderName_ = actualName;
            }

            configured_.store(true, std::memory_order_release);
            selftestEosSeen_.store(false, std::memory_order_release);
            LOGI("configure succeeded variant=%d plan=%d name=%s mime=%s",
                 plan.variant,
                 idx,
                 actualName.c_str(),
                 mime.c_str());

            outputStopRequested_.store(false, std::memory_order_release);
            outputThreadStarted_.store(true, std::memory_order_release);
            outputThread_ = std::thread([this] { outputLoop(); });

            return;
        }

        LOGE("all hardware configure attempts failed");
        hwConfigureFailedPermanent_.store(true, std::memory_order_relaxed);

        fakeSelected_.store(false, std::memory_order_release);
        pendingFakeInjection_.store(false, std::memory_order_release);
        fakeOutputPending_.store(false, std::memory_order_release);
        activeFake_ = FakeIdr();
        fakeCsd0_.clear();
        fakeCsd1_.clear();
        {
            std::lock_guard<std::mutex> lock(fakeNameMutex_);
            fakeName_.clear();
        }

        if (decoderMode_ == "auto") {
            configureSoftwareLocked();
        }
    }

    void tryConfigure() {
        std::lock_guard<std::mutex> lock(configureMutex_);
        tryConfigureLocked();
    }

    void ensureDecoderConfiguredLocked() {
        if (!running_.load()) {
            return;
        }

        if (softwareActive_.load()) {
            return;
        }

        if (!configured_.load()) {
            tryConfigureLocked();
        }
    }

    void maybeCheckHwStallLocked() {
        if (decoderMode_ != "auto") {
            return;
        }

        if (softwareActive_.load()) {
            return;
        }

        const int64_t firstUs = firstAuQueuedUs_.load(std::memory_order_relaxed);
        if (firstUs == 0) {
            return;
        }

        if (totalFramesDecoded_.load(std::memory_order_relaxed) > 0) {
            return;
        }

        const int64_t now = nowUs();
        const bool fakeMode = fakeSelected_.load(std::memory_order_relaxed);
        const int64_t timeoutUs = fakeMode ? 2000000 : 1000000;
        const uint64_t consumedThreshold = fakeMode ? 60 : 30;

        const bool timedOut = (now - firstUs >= timeoutUs);
        const bool consumedMany = (hwAusQueued_.load(std::memory_order_relaxed) >= consumedThreshold);

        if (timedOut || consumedMany) {
            LOGI("switching to software decoder (no hw output)");
            softwareFallbackRequested_.store(true, std::memory_order_release);
            outputStopRequested_.store(true, std::memory_order_release);
            configured_.store(false, std::memory_order_release);
        }
    }

    void feedAu(const std::vector<uint8_t>& au, int64_t arrivalUs, bool hasRelay, int64_t relayTs) {
        if (au.empty()) {
            return;
        }

        if (!surfaceReady_.load()) {
            totalFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::lock_guard<std::mutex> lock(configureMutex_);

        if (!running_.load()) {
            return;
        }

        ensureDecoderConfiguredLocked();

        if (softwareActive_.load()) {
            feedSoftwareLocked(au, arrivalUs, hasRelay, relayTs);
            return;
        }

        if (!configured_.load() || codec_ == nullptr || outputStopRequested_.load()) {
            totalFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::vector<uint8_t> modifiedAu;
        const std::vector<uint8_t>* feedData = &au;

        if (pendingFakeInjection_.load(std::memory_order_acquire) && !softwareActive_.load()) {
            bool injected = injectFakeLocked(au);
            if (injected) {
                modifiedAu = buildLiveAuWithCsdLocked(au);
                feedData = &modifiedAu;
            }
            pendingFakeInjection_.store(false, std::memory_order_release);
        }

        const size_t feedSize = feedData->size();

        ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 2000);
        if (idx < 0) {
            totalFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            if (idx != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                LOGW("dequeueInputBuffer failed: %zd", idx);
            }
            return;
        }

        size_t bufSize = 0;
        void* buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &bufSize);
        if (buf == nullptr || feedSize > bufSize) {
            releaseInputBufferSafely(codec_, static_cast<size_t>(idx));
            totalFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            LOGW("input buffer too small or null: required=%zu available=%zu", feedSize, bufSize);
            return;
        }

        std::memcpy(buf, feedData->data(), feedSize);

        const int64_t pts = nextPts(arrivalUs);

        if (hasRelay) {
            queueRelayForPts(pts, relayTs);
        }

        {
            std::lock_guard<std::mutex> lock(relayMapMutex_);
            if (queuedAtByPts_.size() > 512) {
                queuedAtByPts_.clear();
            }
            queuedAtByPts_[pts] = nowUs();
        }

        uint32_t flags = 0;
        if (expKeyFrame_) {
            flags |= kBufferFlagKeyFrame;
        }

        const media_status_t status =
            AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, feedSize, pts, flags);

        if (status != AMEDIA_OK) {
            releaseInputBufferSafely(codec_, static_cast<size_t>(idx));
            totalFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            LOGW("queueInputBuffer failed: %d", static_cast<int>(status));
            return;
        }

        hwAusQueued_.fetch_add(1, std::memory_order_relaxed);
        if (firstAuQueuedUs_.load(std::memory_order_relaxed) == 0) {
            firstAuQueuedUs_.store(nowUs(), std::memory_order_relaxed);
        }

        maybeCheckHwStallLocked();

        if (softwareFallbackRequested_.load() && !softwareActive_.load()) {
            configureSoftwareLocked();
        }
    }

    void feedSoftwareLocked(const std::vector<uint8_t>& /*au*/, int64_t /*arrivalUs*/, bool /*hasRelay*/,
                            int64_t /*relayTs*/) {
        totalFramesDropped_.fetch_add(1, std::memory_order_relaxed);     // never active: hardware only
    }

    bool queueEndOfStream() {
        if (!configured_.load()) {
            tryConfigure();
        }

        std::lock_guard<std::mutex> lock(configureMutex_);

        if (softwareActive_.load()) {
            return false;
        }

        if (!configured_.load() || codec_ == nullptr || outputStopRequested_.load()) {
            return false;
        }

        ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 2000);
        if (idx < 0) {
            LOGW("queueEndOfStream dequeueInputBuffer failed: %zd", idx);
            return false;
        }

        size_t bufSize = 0;
        void* buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &bufSize);
        if (buf == nullptr) {
            releaseInputBufferSafely(codec_, static_cast<size_t>(idx));
            LOGW("queueEndOfStream input buffer null");
            return false;
        }

        const int64_t pts = nextPts(nowUs());
        const media_status_t status =
            AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, 0, pts, kBufferFlagEndOfStream);

        if (status != AMEDIA_OK) {
            releaseInputBufferSafely(codec_, static_cast<size_t>(idx));
            LOGW("queueEndOfStream queueInputBuffer failed: %d", static_cast<int>(status));
            return false;
        }

        if (source_ == Source::Selftest) {
            selftestEosSeen_.store(true, std::memory_order_release);
        }

        return true;
    }

    void outputLoop() {
        AMediaCodec* c = codec_;
        if (c == nullptr) {
            outputThreadStarted_.store(false);
            return;
        }

        outputStopRequested_.store(false);
        raiseThreadPriority("codec-output");

        AMediaCodecBufferInfo info {};

        while (running_.load() && !outputStopRequested_.load()) {
            // dequeueOutputBuffer returns as soon as a buffer is ready; the timeout only bounds how long an
            // idle loop sleeps, so 10 ms costs no latency and wakes the thread 5x less often than 2 ms.
            ssize_t idx = AMediaCodec_dequeueOutputBuffer(c, &info, 10000);

            if (idx >= 0) {
                const int64_t dequeueUs = nowUs();
                const int64_t pts = info.presentationTimeUs;
                const bool eos = (info.flags & kBufferFlagEndOfStream) != 0;

                bool fakeFrame = false;
                if (fakeOutputPending_.load(std::memory_order_acquire) &&
                    pts == fakeOutputPts_.load(std::memory_order_relaxed)) {
                    fakeFrame = true;
                    fakeOutputPending_.store(false, std::memory_order_relaxed);
                    totalFakeIdrOutputs_.fetch_add(1, std::memory_order_relaxed);
                }

                const bool countedFrame = !eos && info.size > 0 && !fakeFrame;

                if (countedFrame) {
                    totalFramesDecoded_.fetch_add(1, std::memory_order_relaxed);
                }

                lastOutputUs_.store(dequeueUs, std::memory_order_relaxed);

                if (countedFrame && pts > 0) {
                    int64_t decodeLatUs = dequeueUs - pts;
                    if (decodeLatUs < 0) {
                        decodeLatUs = 0;
                    }

                    globalDecodeSumUs_.fetch_add(static_cast<uint64_t>(decodeLatUs), std::memory_order_relaxed);
                    globalDecodeCount_.fetch_add(1, std::memory_order_relaxed);

                    int64_t relayTs = 0;
                    const bool hasRelay = takeQueuedRelay(pts, relayTs);

                    int64_t queuedAt = 0;
                    {
                        std::lock_guard<std::mutex> lock(relayMapMutex_);
                        auto qit = queuedAtByPts_.find(pts);
                        if (qit != queuedAtByPts_.end()) {
                            queuedAt = qit->second;
                            queuedAtByPts_.erase(qit);
                        }
                    }
                    if (queuedAt >= pts && dequeueUs >= queuedAt) {
                        ivWireSumUs_.fetch_add(static_cast<uint64_t>(queuedAt - pts), std::memory_order_relaxed);
                        ivCodecSumUs_.fetch_add(static_cast<uint64_t>(dequeueUs - queuedAt), std::memory_order_relaxed);
                        ivSplitCount_.fetch_add(1, std::memory_order_relaxed);
                    }

                    {
                        std::lock_guard<std::mutex> lock(pendingMutex_);
                        pendingRenders_[pts] = PendingRender{dequeueUs, relayTs, hasRelay};
                        if (pendingRenders_.size() > 1024) {
                            pendingRenders_.clear();
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lock(statsMutex_);
                        decodeSamples_.push_back({dequeueUs, decodeLatUs});
                        decodeOutputWindow_.push_back(dequeueUs);
                        pruneWindowsLocked(dequeueUs);
                    }
                }

                if (eos) {
                    selftestEosSeen_.store(true, std::memory_order_release);
                }

                const bool render = surfaceReady_.load() && info.size > 0 && !fakeFrame;
                AMediaCodec_releaseOutputBuffer(c, static_cast<size_t>(idx), render);

                maybeLogSummary();
            } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* format = AMediaCodec_getOutputFormat(c);
                if (format != nullptr) {
                    int32_t w = 0;
                    int32_t h = 0;
                    AMediaFormat_getInt32(format, "width", &w);
                    AMediaFormat_getInt32(format, "height", &h);
                    if (w > 0 && h > 0) {
                        width_.store(w);
                        height_.store(h);
                    }
                    LOGI("output format changed: %dx%d", w, h);
                    AMediaFormat_delete(format);
                }
            } else if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                const int64_t now = nowUs();
                const int64_t firstUs = firstAuQueuedUs_.load(std::memory_order_relaxed);
                const uint64_t decoded = totalFramesDecoded_.load(std::memory_order_relaxed);
                const uint64_t consumed = hwAusQueued_.load(std::memory_order_relaxed);
                const bool fakeMode = fakeSelected_.load(std::memory_order_relaxed);
                const int64_t timeoutUs = fakeMode ? 2000000 : 1000000;
                const uint64_t consumedThreshold = fakeMode ? 60 : 30;

                if (!selftestActive_.load() &&
                    configured_.load() &&
                    !softwareActive_.load() &&
                    decoderMode_ == "auto" &&
                    firstUs != 0 &&
                    decoded == 0 &&
                    (now - firstUs >= timeoutUs || consumed >= consumedThreshold)) {
                    LOGI("switching to software decoder (no hw output)");
                    softwareFallbackRequested_.store(true, std::memory_order_release);
                    outputStopRequested_.store(true, std::memory_order_release);
                    configured_.store(false, std::memory_order_release);
                    break;
                }
            } else if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                // Ignored by NDK API.
            } else {
                LOGE("dequeueOutputBuffer error: %zd", idx);
                if (decoderMode_ == "auto") {
                    softwareFallbackRequested_.store(true, std::memory_order_release);
                } else {
                    hwConfigureFailedPermanent_.store(true, std::memory_order_relaxed);
                }
                outputStopRequested_.store(true, std::memory_order_release);
                configured_.store(false, std::memory_order_release);
                break;
            }
        }

        outputThreadStarted_.store(false);

        if (running_.load() && !outputStopRequested_.load() && !softwareFallbackRequested_.load()) {
            LOGW("output loop ended unexpectedly");
        }
    }

    void pruneWindowsLocked(int64_t now) {
        decodeSamples_.erase(
            std::remove_if(decodeSamples_.begin(), decodeSamples_.end(),
                [now](const LatencySample& s) { return now - s.t > 1000000; }),
            decodeSamples_.end());

        displaySamples_.erase(
            std::remove_if(displaySamples_.begin(), displaySamples_.end(),
                [now](const LatencySample& s) { return now - s.t > 1000000; }),
            displaySamples_.end());

        relaySamples_.erase(
            std::remove_if(relaySamples_.begin(), relaySamples_.end(),
                [now](const LatencySample& s) { return now - s.t > 1000000; }),
            relaySamples_.end());

        swDecodeSamples_.erase(
            std::remove_if(swDecodeSamples_.begin(), swDecodeSamples_.end(),
                [now](const LatencySample& s) { return now - s.t > 1000000; }),
            swDecodeSamples_.end());

        while (!decodeOutputWindow_.empty() && now - decodeOutputWindow_.front() > 1000000) {
            decodeOutputWindow_.pop_front();
        }

        while (!renderWindow_.empty() && now - renderWindow_.front() > 1000000) {
            renderWindow_.pop_front();
        }

        while (!rxBytesWindow_.empty() && now - rxBytesWindow_.front().first > 1000000) {
            rxBytesWindow_.pop_front();
        }
    }

    void pruneWindows(int64_t now) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        pruneWindowsLocked(now);
    }

    void updateCounters(int64_t now) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        updateCountersLocked(now);
    }

    void updateCountersLocked(int64_t now) {
        pruneWindowsLocked(now);

        if (counterWindowStartUs_ == 0) {
            counterWindowStartUs_ = now;
            startRxBytes_ = totalRxBytes_.load(std::memory_order_relaxed);
            startRxChunks_ = rxChunks_;
            startNals_ = totalNals_.load(std::memory_order_relaxed);
            startAus_ = totalAus_.load(std::memory_order_relaxed);
            startDecoded_ = totalFramesDecoded_.load(std::memory_order_relaxed);
            startRendered_ = totalFramesRendered_.load(std::memory_order_relaxed);
        }

        const int64_t elapsed = now - counterWindowStartUs_;
        if (elapsed >= 1000000) {
            const uint64_t curRxBytes = totalRxBytes_.load(std::memory_order_relaxed);
            const uint64_t curNals = totalNals_.load(std::memory_order_relaxed);
            const uint64_t curAus = totalAus_.load(std::memory_order_relaxed);
            const uint64_t curDecoded = totalFramesDecoded_.load(std::memory_order_relaxed);
            const uint64_t curRendered = totalFramesRendered_.load(std::memory_order_relaxed);

            uint64_t bytes = 0;
            for (const auto& p : rxBytesWindow_) {
                bytes += p.second;
            }

            lastBytesPerSec_ = bytes;
            lastChunksPerSec_ = rxChunks_ - startRxChunks_;
            lastNalsPerSec_ = curNals - startNals_;
            lastAusPerSec_ = curAus - startAus_;
            lastDecodeFramesPerSec_ = decodeOutputWindow_.size();
            lastRenderFramesPerSec_ = renderWindow_.size();

            counterWindowStartUs_ = now;
            startRxBytes_ = curRxBytes;
            startRxChunks_ = rxChunks_;
            startNals_ = curNals;
            startAus_ = curAus;
            startDecoded_ = curDecoded;
            startRendered_ = curRendered;
        }
    }

    void maybeLogSummary() {
        if (!running_.load()) {
            return;
        }

        if (!connected_.load() && !receiving_.load() && !selftestActive_.load()) {
            return;
        }

        const int64_t now = nowUs();
        int64_t last = lastLogUs_.load(std::memory_order_relaxed);
        if (now - last < 1000000) {
            return;
        }
        lastLogUs_.store(now, std::memory_order_relaxed);

        updateCounters(now);

        LOGI("rx %llu B/s, nals %llu, aus %llu, codec %s, params %s, decoder %s",
             static_cast<unsigned long long>(lastBytesPerSec_),
             static_cast<unsigned long long>(totalNals_.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(totalAus_.load(std::memory_order_relaxed)),
             codecMime().c_str(),
             paramsReady_.load() ? "yes" : "no",
             configured_.load() ? "running" : "not-running");

        const uint64_t decoded = totalFramesDecoded_.load(std::memory_order_relaxed);
        const uint64_t rendered = totalFramesRendered_.load(std::memory_order_relaxed);
        const uint64_t dropped = totalFramesDropped_.load(std::memory_order_relaxed);
        const uint64_t decodeCount = globalDecodeCount_.load(std::memory_order_relaxed);
        const uint64_t displayCount = globalDisplayCount_.load(std::memory_order_relaxed);
        const double decodeAvgMs = decodeCount > 0
            ? static_cast<double>(globalDecodeSumUs_.load(std::memory_order_relaxed)) / decodeCount / 1000.0
            : 0.0;
        const double displayAvgMs = displayCount > 0
            ? static_cast<double>(globalDisplaySumUs_.load(std::memory_order_relaxed)) / displayCount / 1000.0
            : 0.0;
        const uint64_t swCount = globalSwDecodeCount_.load(std::memory_order_relaxed);
        const double swAvgMs = swCount > 0
            ? static_cast<double>(globalSwDecodeSumUs_.load(std::memory_order_relaxed)) / swCount / 1000.0
            : 0.0;

        // The panel only shows these while it is visible, so log them: an offline record makes two
        // runs comparable and makes rendered=0 obvious without a screenshot.
        LOGI("frames src=%s decoded=%llu rendered=%llu dropped=%llu | "
             "avg decode %.1f ms (n=%llu), avg displayed %.1f ms (n=%llu), avg sw %.1f ms (n=%llu)",
             sourceName().c_str(),
             static_cast<unsigned long long>(decoded),
             static_cast<unsigned long long>(rendered),
             static_cast<unsigned long long>(dropped),
             decodeAvgMs,
             static_cast<unsigned long long>(decodeCount),
             displayAvgMs,
             static_cast<unsigned long long>(displayCount),
             swAvgMs,
             static_cast<unsigned long long>(swCount));

        // Where the time goes, averaged over the last interval only (comparable between A/B runs).
        const uint64_t nSplit = ivSplitCount_.exchange(0, std::memory_order_relaxed);
        const uint64_t wireUs = ivWireSumUs_.exchange(0, std::memory_order_relaxed);
        const uint64_t codecUs = ivCodecSumUs_.exchange(0, std::memory_order_relaxed);
        const uint64_t nRender = ivRenderCount_.exchange(0, std::memory_order_relaxed);
        const uint64_t renderSumUs = ivRenderSumUs_.exchange(0, std::memory_order_relaxed);
        const uint64_t nRelay = ivRelayCount_.exchange(0, std::memory_order_relaxed);
        const uint64_t relaySumUs = ivRelaySumUs_.exchange(0, std::memory_order_relaxed);
        auto avgMs = [](uint64_t sum, uint64_t n) { return n > 0 ? static_cast<double>(sum) / n / 1000.0 : 0.0; };
        LOGI("latency 1s: arrive->queued %.1f ms, queued->decoded %.1f ms (n=%llu), decoded->displayed %.1f ms (n=%llu), "
             "relay->displayed %.1f ms (n=%llu, clock %s, rtt %.1f ms)",
             avgMs(wireUs, nSplit), avgMs(codecUs, nSplit), static_cast<unsigned long long>(nSplit),
             avgMs(renderSumUs, nRender), static_cast<unsigned long long>(nRender),
             avgMs(relaySumUs, nRelay), static_cast<unsigned long long>(nRelay),
             clockValid_.load() ? "ok" : "none", clockRttUs_.load() / 1000.0);
    }

    void closeSocket() {
        if (socketFd_ >= 0) {
            close(socketFd_);
            socketFd_ = -1;
        }
    }

    void resetDecoderForReconnect() {
        {
            std::lock_guard<std::mutex> lock(configureMutex_);
            stopCodecLocked();

            fakeSelected_.store(false, std::memory_order_release);
            pendingFakeInjection_.store(false, std::memory_order_release);
            fakeOutputPending_.store(false, std::memory_order_release);
            fakeOutputPts_.store(-1, std::memory_order_release);
            activeFake_ = FakeIdr();
            fakeCsd0_.clear();
            fakeCsd1_.clear();

            paramsReady_.store(false, std::memory_order_release);
            codecType_.store(0, std::memory_order_relaxed);
            width_.store(0, std::memory_order_relaxed);
            height_.store(0, std::memory_order_relaxed);
            hevcPocBits_.store(0, std::memory_order_relaxed);
            lastPtsUs_.store(0, std::memory_order_relaxed);
            selftestEosSeen_.store(false, std::memory_order_release);
            softwareFallbackRequested_.store(false, std::memory_order_release);
            hwConfigureFailedPermanent_.store(false, std::memory_order_relaxed);
            swConfigureFailedPermanent_.store(false, std::memory_order_relaxed);
            configurePlanIndex_.store(0, std::memory_order_relaxed);
            configuredPlanIndex_.store(-1, std::memory_order_relaxed);
            configureVariant_.store(-1, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(fakeNameMutex_);
            fakeName_.clear();
        }

        {
            std::lock_guard<std::mutex> lock(nameMutex_);
            decoderName_.clear();
            swDecoderName_.clear();
        }

        {
            std::lock_guard<std::mutex> lock(csdMutex_);
            vps_.clear();
            sps_.clear();
            pps_.clear();
            csd0_.clear();
            csd1_.clear();
        }
    }

    bool connectOnce(int connectTimeoutMs) {
        if (host_.empty() || port_ <= 0 || port_ > 65535) {
            return false;
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            LOGE("socket failed: %s", std::strerror(errno));
            return false;
        }

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port_));

        if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
            LOGE("invalid host: %s", host_.c_str());
            close(fd);
            return false;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1) {
            close(fd);
            return false;
        }
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rc = connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        bool ok = false;

        if (rc == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            pollfd pfd {};
            pfd.fd = fd;
            pfd.events = POLLOUT;

            const int pollRc = poll(&pfd, 1, connectTimeoutMs);
            if (pollRc > 0 && (pfd.revents & POLLOUT) != 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                ok = (err == 0);
            }
        }

        if (!ok) {
            LOGW("connect failed: %s", std::strerror(errno));
            close(fd);
            return false;
        }

        fcntl(fd, F_SETFL, flags);

        int yes = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        int rcvBuf = kRcvBufSize;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));

        timeval tv {};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        socketFd_ = fd;
        connected_.store(true);
        LOGI("connected to %s:%d", host_.c_str(), port_);
        return true;
    }

    void sleepUs(int64_t us) {
        const int64_t deadline = nowUs() + us;
        while (running_.load() && nowUs() < deadline) {
            usleep(50000);
        }
    }

    void networkLoop() {
        raiseThreadPriority("net-rx");
        while (running_.load()) {
            if (socketFd_ < 0) {
                connected_.store(false);

                if (!connectOnce(5000)) {
                    sleepUs(1000000);
                    continue;
                }

                resetStreamState();
            }

            uint8_t tmp[32768];
            ssize_t n = recv(socketFd_, tmp, sizeof(tmp), 0);

            if (n > 0) {
                receiving_.store(true);
                pushStream(tmp, static_cast<size_t>(n), nowUs());
            } else if (!running_.load()) {
                break;                                  // stop() shut the socket down
            } else if (n == 0) {
                LOGW("connection closed by peer");
                flushParser();
                closeSocket();
                resetDecoderForReconnect();
                connected_.store(false);
                sleepUs(1000000);
            } else {
                const int err = errno;
                if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
                    maybeLogSummary();
                    continue;
                }

                LOGW("recv error: %s", std::strerror(err));
                flushParser();
                closeSocket();
                resetDecoderForReconnect();
                connected_.store(false);
                sleepUs(1000000);
            }

            maybeLogSummary();
        }

        flushParser();
        closeSocket();
    }

    void startClockIfNetwork() {
        if (clockFd_ >= 0 || source_ != Source::Network || host_.empty()) {
            return;
        }

        clockFd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (clockFd_ < 0) {
            LOGW("UDP clock socket failed: %s", std::strerror(errno));
            return;
        }

        int flags = fcntl(clockFd_, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(clockFd_, F_SETFL, flags | O_NONBLOCK);
        }
    }

    void sendClockRequest(int64_t t0) {
        if (clockFd_ < 0 || host_.empty()) {
            return;
        }

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kRelayClockPort);
        if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
            return;
        }

        uint8_t req[8];
        putBE64(req, t0);

        sendto(clockFd_, req, sizeof(req), 0,
               reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    }

    void processClockReply(const uint8_t* data, size_t len, int64_t t1) {
        if (len < 16) {
            return;
        }

        const int64_t t0 = static_cast<int64_t>(getBE64(data));
        const int64_t tr = static_cast<int64_t>(getBE64(data + 8));
        const int64_t rtt = t1 - t0;
        const int64_t offset = tr - ((t0 + t1) / 2);

        std::lock_guard<std::mutex> lock(clockSamplesMutex_);

        if (rtt < 0 || rtt > 10000000) {
            return;
        }

        clockSamples_.push_back(ClockSample{rtt, offset});
        if (clockSamples_.size() > 8) {
            std::sort(clockSamples_.begin(), clockSamples_.end(),
                      [](const ClockSample& a, const ClockSample& b) {
                          return a.rttUs < b.rttUs;
                      });
            clockSamples_.resize(8);
        }

        clockRttUs_.store(clockSamples_.front().rttUs, std::memory_order_relaxed);
        clockOffsetUs_.store(clockSamples_.front().offsetUs, std::memory_order_relaxed);
        lastClockReplyUs_.store(t1, std::memory_order_relaxed);
        clockValid_.store(true, std::memory_order_relaxed);
    }

    void clockLoop() {
        startClockIfNetwork();

        if (clockFd_ < 0) {
            while (running_.load()) {
                sleepUs(1000000);
            }
            return;
        }

        while (running_.load()) {
            const int64_t t0 = monoUs();
            sendClockRequest(t0);

            const int64_t sendDeadline = t0 + 1000000;

            while (running_.load() && monoUs() < sendDeadline) {
                pollfd pfd {};
                pfd.fd = clockFd_;
                pfd.events = POLLIN;

                const int pr = poll(&pfd, 1, 50);
                if (pr <= 0) {
                    continue;
                }

                if ((pfd.revents & POLLIN) != 0) {
                    uint8_t buf[32] {};
                    sockaddr_in from {};
                    socklen_t fromLen = sizeof(from);
                    const ssize_t n = recvfrom(clockFd_, buf, sizeof(buf), 0,
                                               reinterpret_cast<sockaddr*>(&from), &fromLen);
                    if (n >= 8) {
                        processClockReply(buf, static_cast<size_t>(n), monoUs());
                    }
                }
            }

            if (monoUs() - lastClockReplyUs_.load(std::memory_order_relaxed) > 3000000) {
                clockValid_.store(false, std::memory_order_relaxed);
            }
        }
    }

    void usbLoop() {
        raiseThreadPriority("usb-rx");
        while (running_.load()) {
            if (usbFd_ < 0) {
                usbStatus_ = "USB fd invalid";
                sleepUs(1000000);
                continue;
            }

            int dupFd = dup(usbFd_);
            if (dupFd < 0) {
                usbStatus_ = std::string("dup fd failed: ") + std::strerror(errno);
                sleepUs(1000000);
                continue;
            }

            UsbRndis rndis(dupFd, usbCtrlIf_, usbEpIn_, usbEpOut_);

            if (!rndis.init()) {
                usbStatus_ = rndis.statusString();
                LOGW("USB RNDIS init failed");
                close(dupFd);
                sleepUs(1000000);
                continue;
            }

            connected_.store(true);
            usbStatus_ = rndis.statusString();

            TinyNet net(
                [&](const uint8_t* frame, size_t len) {
                    return rndis.sendFrame(frame, len);
                },
                [&](const uint8_t* payload, size_t len, int64_t t) {
                    if (len > 0) {
                        receiving_.store(true);
                    }
                    pushStream(payload, len, t);
                }
            );

            while (running_.load()) {
                int frames = rndis.readFrames(100,
                    [&](const uint8_t* eth, size_t ethLen, int64_t t) {
                        net.handleFrame(eth, ethLen, t);
                    });

                const int64_t now = nowUs();
                net.tick(now);

                if (net.streaming()) {
                    receiving_.store(true);
                }

                usbStatus_ = rndis.statusString() + " / " + net.statusString(now);
                updateCounters(now);
                maybeLogSummary();

                if (frames < 0) {
                    connected_.store(false);
                    usbStatus_ = "USB link error";
                    break;
                }
            }

            flushParser();
            close(dupFd);
            resetDecoderForReconnect();
            sleepUs(500000);
        }
    }

    void reportSelftest(const std::string& reason) {
        if (selftestReported_) {
            return;
        }

        selftestReported_ = true;
        selftestDone_ = true;

        const uint64_t nals = totalNals_.load(std::memory_order_relaxed);
        const uint64_t aus = totalAus_.load(std::memory_order_relaxed);
        const uint64_t decoded = totalFramesDecoded_.load(std::memory_order_relaxed);
        const uint64_t rendered = totalFramesRendered_.load(std::memory_order_relaxed);
        const bool fake = fakeSelected_.load(std::memory_order_relaxed);

        std::string fakeLabel;
        {
            std::lock_guard<std::mutex> lock(fakeNameMutex_);
            fakeLabel = fakeName_;
        }

        double avgLatMs = 0.0;
        if (globalDisplayCount_.load() > 0) {
            avgLatMs = static_cast<double>(globalDisplaySumUs_.load()) /
                static_cast<double>(globalDisplayCount_.load()) / 1000.0;
        } else if (globalDecodeCount_.load() > 0) {
            avgLatMs = static_cast<double>(globalDecodeSumUs_.load()) /
                static_cast<double>(globalDecodeCount_.load()) / 1000.0;
        }

        double swDecodeMsAvg = 0.0;
        if (globalSwDecodeCount_.load() > 0) {
            swDecodeMsAvg = static_cast<double>(globalSwDecodeSumUs_.load()) /
                static_cast<double>(globalSwDecodeCount_.load()) / 1000.0;
        }

        bool pass = (reason == "ok") &&
                    (aus >= 290) &&
                    (decoded >= ((aus * 85ULL) / 100ULL)) &&
                    (decoded > 0);

        std::ostringstream oss;
        if (pass) {
            oss << "SELFTEST PASS nals=" << nals
                << " aus=" << aus
                << " decoded=" << decoded
                << " rendered=" << rendered
                << " codec=" << codecMime()
                << " decoder=" << decoderName()
                << " path=" << (softwareActive_.load() ? "sw" : "hw")
                << " fakeIdr=" << (fake ? "yes" : "no")
                << " hwexp=" << (hwExpString_.empty() ? "-" : hwExpString_)
                << " swDecodeMsAvg=" << std::fixed << std::setprecision(1) << swDecodeMsAvg
                << " avgLatMs=" << std::fixed << std::setprecision(1) << avgLatMs;
            LOGI("%s", oss.str().c_str());
        } else {
            oss << "SELFTEST FAIL " << reason
                << " nals=" << nals
                << " aus=" << aus
                << " decoded=" << decoded
                << " rendered=" << rendered
                << " codec=" << codecMime()
                << " decoder=" << decoderName()
                << " path=" << (softwareActive_.load() ? "sw" : "hw")
                << " fakeIdr=" << (fake ? "yes" : "no")
                << " hwexp=" << (hwExpString_.empty() ? "-" : hwExpString_)
                << " swDecodeMsAvg=" << std::fixed << std::setprecision(1) << swDecodeMsAvg
                << " avgLatMs=" << std::fixed << std::setprecision(1) << avgLatMs;
            LOGE("%s", oss.str().c_str());
        }

        (void)fakeLabel;
    }

    bool selftestPasses() {
        const uint64_t aus = totalAus_.load(std::memory_order_relaxed);
        const uint64_t decoded = totalFramesDecoded_.load(std::memory_order_relaxed);
        return aus >= 290 && decoded > 0 && decoded >= ((aus * 85ULL) / 100ULL);
    }

    void resetSelftestRun() {
        {
            std::lock_guard<std::mutex> lock(configureMutex_);
            stopCodecLocked();

            fakeSelected_.store(false, std::memory_order_relaxed);
            pendingFakeInjection_.store(false, std::memory_order_relaxed);
            fakeOutputPending_.store(false, std::memory_order_relaxed);
            fakeOutputPts_.store(-1, std::memory_order_relaxed);
            activeFake_ = FakeIdr();
            fakeCsd0_.clear();
            fakeCsd1_.clear();

            hwConfigureFailedPermanent_.store(false, std::memory_order_relaxed);
            swConfigureFailedPermanent_.store(false, std::memory_order_relaxed);
            softwareFallbackRequested_.store(false, std::memory_order_relaxed);
            configurePlanIndex_.store(0, std::memory_order_relaxed);
            configuredPlanIndex_.store(-1, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(fakeNameMutex_);
            fakeName_.clear();
        }

        {
            std::lock_guard<std::mutex> lock(nameMutex_);
            decoderName_.clear();
            swDecoderName_.clear();
        }

        resetStreamState();

        {
            std::lock_guard<std::mutex> lock(csdMutex_);
            vps_.clear();
            sps_.clear();
            pps_.clear();
            csd0_.clear();
            csd1_.clear();
        }

        paramsReady_.store(false);
        codecType_.store(0);
        width_.store(0);
        height_.store(0);
        hevcPocBits_.store(0);
        lastPtsUs_.store(0);
        selftestEosSeen_.store(false);
        selftestDone_ = false;
        selftestReported_ = false;
        recseiFirstAuDone_.store(false, std::memory_order_relaxed);
        firstAuQueuedUs_.store(0, std::memory_order_relaxed);
        hwAusQueued_.store(0, std::memory_order_relaxed);
        lastOutputUs_.store(0, std::memory_order_relaxed);

        totalFakeIdrInjected_.store(0, std::memory_order_relaxed);
        totalFakeIdrOutputs_.store(0, std::memory_order_relaxed);

        totalRxBytes_.store(0);
        totalNals_.store(0);
        totalAus_.store(0);
        totalFramesDecoded_.store(0);
        totalFramesRendered_.store(0);
        totalFramesDropped_.store(0);
        globalDecodeSumUs_.store(0);
        globalDecodeCount_.store(0);
        globalDisplaySumUs_.store(0);
        globalDisplayCount_.store(0);
        globalSwDecodeSumUs_.store(0);
        globalSwDecodeCount_.store(0);
        lastLogUs_.store(0);

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            decodeSamples_.clear();
            displaySamples_.clear();
            relaySamples_.clear();
            swDecodeSamples_.clear();
            decodeOutputWindow_.clear();
            renderWindow_.clear();
            rxBytesWindow_.clear();

            lastBytesPerSec_ = 0;
            lastChunksPerSec_ = 0;
            lastNalsPerSec_ = 0;
            lastAusPerSec_ = 0;
            lastDecodeFramesPerSec_ = 0;
            lastRenderFramesPerSec_ = 0;

            counterWindowStartUs_ = 0;
            startRxBytes_ = 0;
            startRxChunks_ = 0;
            startNals_ = 0;
            startAus_ = 0;
            startDecoded_ = 0;
            startRendered_ = 0;
            rxChunks_ = 0;
        }

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pendingRenders_.clear();
        }

        {
            std::lock_guard<std::mutex> lock(relayMapMutex_);
            queuedRelayByPts_.clear();
        }
    }

    bool feedSelftestFileOnce() {
        std::ifstream file(selftestPath_, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        file.seekg(0, std::ios::end);
        size_t fileSize = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);

        std::mt19937_64 rng(42);
        std::uniform_int_distribution<int> chunkDist(1, 65536);

        std::vector<uint8_t> tmp(65536);
        size_t remaining = fileSize;
        uint64_t lastAus = totalAus_.load(std::memory_order_relaxed);

        while (running_.load() && remaining > 0) {
            int wanted = chunkDist(rng);
            if (wanted <= 0) {
                wanted = 1;
            }

            size_t take = static_cast<size_t>(wanted);
            if (take > remaining) {
                take = remaining;
            }

            file.read(reinterpret_cast<char*>(tmp.data()), static_cast<std::streamsize>(take));
            if (!file && (file.eof() == false)) {
                return false;
            }

            pushStream(tmp.data(), take, nowUs());
            remaining -= take;

            const uint64_t curAus = totalAus_.load(std::memory_order_relaxed);
            const uint64_t delta = curAus - lastAus;

            if (delta > 0) {
                lastAus = curAus;
                const int frames = static_cast<int>(delta > 3 ? 3 : delta);
                usleep(static_cast<useconds_t>(frames * 33333));
            } else {
                usleep(1000);
            }
        }

        return true;
    }

    void selftestLoop() {
        int64_t start = nowUs();
        while (running_.load() && !surfaceReady_.load()) {
            usleep(50000);
            if (nowUs() - start > 15000000) {
                reportSelftest("no-surface");
                return;
            }
        }

        if (!running_.load()) {
            return;
        }

        resetSelftestRun();

        if (!feedSelftestFileOnce()) {
            reportSelftest("read-failed");
            return;
        }

        flushParser();

        if (!softwareActive_.load()) {
            for (int attempt = 0; attempt < 20 && running_.load(); ++attempt) {
                if (queueEndOfStream()) {
                    break;
                }
                usleep(50000);
            }
        }

        int64_t waitStart = nowUs();
        const uint64_t expected = totalAus_.load(std::memory_order_relaxed);
        const uint64_t passThreshold = (expected * 85ULL) / 100ULL;
        bool sawEos = false;
        uint64_t lastDecoded = 0;
        int64_t lastProgressUs = nowUs();

        while (running_.load() && nowUs() - waitStart < 10000000) {
            if (selftestEosSeen_.load(std::memory_order_acquire)) {
                sawEos = true;
            }

            const uint64_t dec = totalFramesDecoded_.load(std::memory_order_relaxed);

            if (dec != lastDecoded) {
                lastDecoded = dec;
                lastProgressUs = nowUs();
            }

            if (expected > 0 && dec >= expected) {
                break;
            }

            if (dec >= passThreshold && (sawEos || softwareActive_.load()) && nowUs() - lastProgressUs > 500000) {
                break;
            }

            if (dec == 0 && nowUs() - waitStart > 2500000) {
                break;
            }

            usleep(10000);
        }

        if (!running_.load()) {
            reportSelftest("cancelled");
            return;
        }

        reportSelftest(selftestPasses() ? "ok" : "no-decode");
    }

    // Sleeps until the recorded time tUs is due (base: baseTs played at baseNow). Returns false when stopped.
    bool replayWait(int64_t tUs, int64_t& baseTs, int64_t& baseNow) {
        for (;;) {
            if (!running_.load()) return false;
            if (replayPaused_.load()) {
                const int64_t pausedAt = nowUs();
                while (replayPaused_.load() && running_.load()) usleep(20000);
                baseNow += nowUs() - pausedAt;          // resume where we paused
                continue;
            }
            const int64_t due = baseNow + (tUs - baseTs);
            const int64_t wait = due - nowUs();
            if (wait <= 0) return true;
            usleep(static_cast<useconds_t>(std::min<int64_t>(wait, 20000)));
        }
    }

    void replayLoop() {
        raiseThreadPriority("replay");
        const int64_t start = nowUs();
        while (running_.load() && !surfaceReady_.load()) {
            usleep(50000);
            if (nowUs() - start > 15000000) break;
        }

        const std::vector<replay::Segment> segs = replay::listSegments(replayDir_);
        size_t first = 0;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (segs[i].startMs <= replayStartMs_) first = i;
        }
        LOGI("replay: %zu segments, starting at segment %zu", segs.size(), first);

        bool paramsFed = false;
        bool dataFed = false;
        int64_t baseTs = -1, baseNow = 0, prevTs = 0;
        std::vector<uint8_t> buf;
        for (size_t i = first; i < segs.size() && running_.load(); ++i) {
            FILE* f = std::fopen(segs[i].path.c_str(), "rb");
            if (f == nullptr) continue;
            char magic[4] = {};
            if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "QVR1", 4) != 0) {
                std::fclose(f);
                continue;
            }
            for (;;) {
                uint8_t type = 0;
                int64_t tUs = 0;
                uint32_t len = 0;
                if (std::fread(&type, 1, 1, f) != 1 || std::fread(&tUs, 8, 1, f) != 1 ||
                    std::fread(&len, 4, 1, f) != 1 || len > (16u << 20)) {
                    break;
                }
                buf.resize(len);
                if (len > 0 && std::fread(buf.data(), 1, len, f) != len) break;
                if (type == 1) {
                    // Only before the first data: later segments continue a NAL that may span the split.
                    if (!dataFed) {
                        pushStream(buf.data(), buf.size(), nowUs());
                        paramsFed = true;
                    }
                    continue;
                }
                if (type != 2 || !paramsFed) continue;
                size_t off = 0;
                if (!dataFed) {             // join at the first start code
                    while (off + 3 <= buf.size() && !(buf[off] == 0 && buf[off + 1] == 0 && buf[off + 2] == 1)) ++off;
                    if (off + 3 > buf.size()) continue;
                }
                if (baseTs < 0 || tUs < prevTs || tUs - prevTs > 1000000) {   // start, or a gap between sessions
                    baseTs = tUs;
                    baseNow = nowUs();
                }
                if (!replayWait(tUs, baseTs, baseNow)) break;
                pushStream(buf.data() + off, buf.size() - off, nowUs());
                dataFed = true;
                prevTs = tUs;
                replayPosUs_.store(tUs);
                maybeLogSummary();
            }
            std::fclose(f);
        }
        if (running_.load()) {
            LOGI("replay: reached the end of the buffer");
            replayState_ = 3;
        }
    }
};

// AMediaCodecOnFrameRendered is (codec, userdata, mediaTimeUs, systemNano). The first two
// arguments were missing here, so context received the codec and presentationTimeUs received
// the userdata pointer: rendered/displayed counters never moved, and the member call would run
// against the wrong object type.
static void questVisorFrameRenderedCallback(
    AMediaCodec* codec,
    void* context,
    int64_t presentationTimeUs,
    int64_t renderTimeNanos
) {
    (void)codec;
    auto* self = static_cast<StreamDecoder*>(context);
    if (self != nullptr) {
        self->onFrameRendered(presentationTimeUs, renderTimeNanos);
    }
}

namespace {
    std::mutex g_decoderMutex;
    std::unique_ptr<StreamDecoder> g_decoder;
    ANativeWindow* g_surface = nullptr;

    std::string jStringToStd(JNIEnv* env, jstring value) {
        if (env == nullptr || value == nullptr) {
            return std::string();
        }

        const char* chars = env->GetStringUTFChars(value, nullptr);
        if (chars == nullptr) {
            return std::string();
        }

        std::string out(chars);
        env->ReleaseStringUTFChars(value, chars);
        return out;
    }

    void addFakeIdrNative(
        JNIEnv* env,
        jstring codecStr,
        jint width,
        jint height,
        jbyteArray data,
        jstring sidecarStr
    ) {
        if (env == nullptr || codecStr == nullptr || data == nullptr) {
            return;
        }

        const std::string codecName = jStringToStd(env, codecStr);
        const int codec = codecFromName(codecName);
        if (codec == 0 || width <= 0 || height <= 0) {
            return;
        }

        const jsize len = env->GetArrayLength(data);
        if (len <= 0) {
            return;
        }

        std::vector<uint8_t> blob(static_cast<size_t>(len));
        env->GetByteArrayRegion(data, 0, len, reinterpret_cast<jbyte*>(blob.data()));

        FakeIdrSidecar sidecar;
        if (sidecarStr != nullptr) {
            const std::string json = jStringToStd(env, sidecarStr);
            if (!json.empty()) {
                parseSidecarJson(json, sidecar);
            }
        }

        FakeIdr entry;
        entry.name = "black_" + std::to_string(static_cast<int>(width)) +
                     "x" + std::to_string(static_cast<int>(height));
        entry.codec = codec;
        entry.width = static_cast<int>(width);
        entry.height = static_cast<int>(height);
        entry.blob = std::move(blob);
        entry.sidecar = sidecar;

        std::lock_guard<std::mutex> lock(g_fakeIdrMutex);

        for (auto& existing : g_fakeIdrs) {
            if (existing.codec == entry.codec && existing.width == entry.width && existing.height == entry.height) {
                if (existing.sidecar.valid && !entry.sidecar.valid) {
                    return;
                }
                existing = entry;
                return;
            }
        }

        g_fakeIdrs.push_back(std::move(entry));
        LOGI("fake IDR registered: %s", g_fakeIdrs.back().name.c_str());
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_xerx_questvisor_NativePlayer_setSurface(
    JNIEnv* env,
    jclass /* clazz */,
    jobject surface
) {
    std::lock_guard<std::mutex> lock(g_decoderMutex);

    if (surface == nullptr) {
        if (g_decoder != nullptr) {
            g_decoder->setSurface(nullptr);
        }

        if (g_surface != nullptr) {
            ANativeWindow_release(g_surface);
            g_surface = nullptr;
        }
        return;
    }

    ANativeWindow* nextSurface = ANativeWindow_fromSurface(env, surface);
    if (nextSurface == nullptr) {
        LOGE("ANativeWindow_fromSurface returned null");
        return;
    }

    if (g_surface == nextSurface) {
        ANativeWindow_release(nextSurface);
        return;
    }

    if (g_decoder != nullptr) {
        g_decoder->setSurface(nextSurface);
    }

    if (g_surface != nullptr) {
        ANativeWindow_release(g_surface);
    }

    g_surface = nextSurface;
}

extern "C" JNIEXPORT void JNICALL
Java_com_xerx_questvisor_NativePlayer_addFakeIdr(
    JNIEnv* env,
    jclass /* clazz */,
    jstring codecStr,
    jint width,
    jint height,
    jbyteArray data
) {
    addFakeIdrNative(env, codecStr, width, height, data, nullptr);
}

extern "C" JNIEXPORT void JNICALL
Java_com_xerx_questvisor_NativePlayer_addFakeIdrWithSidecar(
    JNIEnv* env,
    jclass /* clazz */,
    jstring codecStr,
    jint width,
    jint height,
    jbyteArray data,
    jstring sidecarStr
) {
    addFakeIdrNative(env, codecStr, width, height, data, sidecarStr);
}

extern "C" JNIEXPORT void JNICALL
Java_com_xerx_questvisor_NativePlayer_startNetwork(
    JNIEnv* env,
    jclass /* clazz */,
    jstring hostStr,
    jint port,
    jstring decoderH265Str,
    jstring decoderH264Str,
    jstring decoderModeStr,
    jstring hwexpStr
) {
    std::lock_guard<std::mutex> lock(g_decoderMutex);

    if (g_decoder != nullptr) {
        g_decoder->stop();
        g_decoder.reset();
    }

    const std::string host = jStringToStd(env, hostStr);
    const std::string h265Name = jStringToStd(env, decoderH265Str);
    const std::string h264Name = jStringToStd(env, decoderH264Str);
    const std::string decoderMode = jStringToStd(env, decoderModeStr);
    const std::string hwexp = jStringToStd(env, hwexpStr);

    if (host.empty() || port <= 0 || port > 65535) {
        LOGE("invalid start arguments");
        return;
    }

    g_decoder = std::make_unique<StreamDecoder>(h265Name, h264Name, decoderMode, hwexp);
    g_decoder->setSurface(g_surface);
    g_recorder.setPaused(false);
    g_decoder->startNetwork(host, static_cast<int>(port));
}

extern "C" JNIEXPORT void JNICALL
Java_com_xerx_questvisor_NativePlayer_startUsb(
    JNIEnv* env,
    jclass /* clazz */,
    jint fd,
    jint ctrlIf,
    jint epIn,
    jint epOut,
    jstring decoderH265Str,
    jstring decoderH264Str,
    jstring decoderModeStr,
    jstring hwexpStr
) {
    std::lock_guard<std::mutex> lock(g_decoderMutex);

    if (g_decoder != nullptr) {
        g_decoder->stop();
        g_decoder.reset();
    }

    const std::string h265Name = jStringToStd(env, decoderH265Str);
    const std::string h264Name = jStringToStd(env, decoderH264Str);
    const std::string decoderMode = jStringToStd(env, decoderModeStr);
    const std::string hwexp = jStringToStd(env, hwexpStr);

    if (fd < 0 || epIn <= 0 || epOut <= 0) {
        LOGE("invalid USB arguments");
        return;
    }

    g_decoder = std::make_unique<StreamDecoder>(h265Name, h264Name, decoderMode, hwexp);
    g_decoder->setSurface(g_surface);
    g_recorder.setPaused(false);
    g_decoder->startUsb(static_cast<int>(fd), static_cast<int>(ctrlIf),
                        static_cast<int>(epIn), static_cast<int>(epOut));
}

extern "C" JNIEXPORT void JNICALL
Java_com_xerx_questvisor_NativePlayer_startSelftest(
    JNIEnv* env,
    jclass /* clazz */,
    jstring pathStr,
    jstring decoderH265Str,
    jstring decoderH264Str,
    jstring decoderModeStr,
    jstring hwexpStr
) {
    std::lock_guard<std::mutex> lock(g_decoderMutex);

    if (g_decoder != nullptr) {
        g_decoder->stop();
        g_decoder.reset();
    }

    const std::string path = jStringToStd(env, pathStr);
    const std::string h265Name = jStringToStd(env, decoderH265Str);
    const std::string h264Name = jStringToStd(env, decoderH264Str);
    const std::string decoderMode = jStringToStd(env, decoderModeStr);
    const std::string hwexp = jStringToStd(env, hwexpStr);

    if (path.empty()) {
        LOGE("invalid selftest path");
        return;
    }

    g_decoder = std::make_unique<StreamDecoder>(h265Name, h264Name, decoderMode, hwexp);
    g_decoder->setSurface(g_surface);
    g_decoder->startSelftest(path);
}

extern "C" JNIEXPORT void JNICALL
Java_com_xerx_questvisor_NativePlayer_setReplayBuffer(JNIEnv* env, jclass, jstring dirStr, jint seconds) {
    g_recorder.configure(jStringToStd(env, dirStr), static_cast<int>(seconds));
}

extern "C" JNIEXPORT void JNICALL
Java_com_xerx_questvisor_NativePlayer_startReplay(
    JNIEnv* env,
    jclass /* clazz */,
    jlong startMs,
    jstring decoderH265Str,
    jstring decoderH264Str,
    jstring decoderModeStr,
    jstring hwexpStr
) {
    std::lock_guard<std::mutex> lock(g_decoderMutex);

    if (g_decoder != nullptr) {
        g_decoder->stop();
        g_decoder.reset();
    }
    const std::string dir = g_recorder.dir();
    if (dir.empty()) {
        LOGE("replay: no replay buffer");
        return;
    }
    g_recorder.setPaused(true);     // keep what is being watched from being pruned

    g_decoder = std::make_unique<StreamDecoder>(jStringToStd(env, decoderH265Str), jStringToStd(env, decoderH264Str),
                                                jStringToStd(env, decoderModeStr), jStringToStd(env, hwexpStr));
    g_decoder->setSurface(g_surface);
    g_decoder->startReplay(dir, static_cast<int64_t>(startMs));
}

extern "C" JNIEXPORT void JNICALL
Java_com_xerx_questvisor_NativePlayer_setReplayPaused(JNIEnv*, jclass, jboolean paused) {
    std::lock_guard<std::mutex> lock(g_decoderMutex);
    if (g_decoder != nullptr) {
        g_decoder->setReplayPaused(paused == JNI_TRUE);
    }
}

// {state (0 live/none, 1 playing, 2 paused, 3 end), position ms, buffer start ms, buffer end ms}; times wall-clock.
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_xerx_questvisor_NativePlayer_replayInfo(JNIEnv* env, jclass) {
    jlong v[4] = {0, 0, 0, 0};
    {
        std::lock_guard<std::mutex> lock(g_decoderMutex);
        if (g_decoder != nullptr) {
            v[0] = g_decoder->replayState();
            v[1] = g_decoder->replayPositionMs();
        }
    }
    const std::string dir = g_recorder.dir();
    if (!dir.empty()) {
        const std::vector<replay::Segment> segs = replay::listSegments(dir);
        if (!segs.empty()) {
            v[2] = segs.front().startMs;
            v[3] = segs.back().endMs;
        }
    }
    jlongArray out = env->NewLongArray(4);
    if (out != nullptr) env->SetLongArrayRegion(out, 0, 4, v);
    return out;
}

extern "C" JNIEXPORT void JNICALL
Java_com_xerx_questvisor_NativePlayer_stop(
    JNIEnv* /* env */,
    jclass /* clazz */
) {
    std::lock_guard<std::mutex> lock(g_decoderMutex);

    if (g_decoder != nullptr) {
        g_decoder->stop();
        g_decoder.reset();
    }
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_xerx_questvisor_NativePlayer_stats(
    JNIEnv* env,
    jclass /* clazz */
) {
    long values[22] {};

    std::lock_guard<std::mutex> lock(g_decoderMutex);
    if (g_decoder != nullptr) {
        g_decoder->getStats(values);
    }

    jlongArray out = env->NewLongArray(22);
    if (out == nullptr) {
        return nullptr;
    }

    jlong native[22];
    for (int i = 0; i < 22; ++i) {
        native[i] = static_cast<jlong>(values[i]);
    }

    env->SetLongArrayRegion(out, 0, 22, native);
    return out;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_xerx_questvisor_NativePlayer_uiState(
    JNIEnv* /* env */,
    jclass /* clazz */
) {
    std::lock_guard<std::mutex> lock(g_decoderMutex);
    if (g_decoder == nullptr) {
        return 0;
    }
    return static_cast<jint>(g_decoder->uiState());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_xerx_questvisor_NativePlayer_statusText(
    JNIEnv* env,
    jclass /* clazz */
) {
    std::lock_guard<std::mutex> lock(g_decoderMutex);

    std::string text;
    if (g_decoder != nullptr) {
        text = g_decoder->statusText();
    }

    return env->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_xerx_questvisor_NativePlayer_decoderName(
    JNIEnv* env,
    jclass /* clazz */
) {
    std::lock_guard<std::mutex> lock(g_decoderMutex);

    std::string name;
    if (g_decoder != nullptr) {
        name = g_decoder->decoderName();
    }

    return env->NewStringUTF(name.c_str());
}

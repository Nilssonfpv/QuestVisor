// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE (GPL-3.0 with an additional permission
// to combine with the Meta Spatial SDK).
// Replay buffer: records the incoming FPV stream (exactly as received, Annex-B) on the headset so the last minutes
// of a flight can be watched again, e.g. after a crash.
//
// Storage: <dir>/seg_<wall-clock start ms>.qvr, one file per ~2 s. Each file starts with "QVR1" and holds records
//   u8 type (1 = parameter sets VPS/SPS/PPS with start codes, 2 = received bytes), i64 wall-clock us, u32 length, data.
// Every segment begins with a type-1 record holding the current parameter sets, so playback can start at any segment.
// Frames are never dropped or re-encoded: every DJI frame is a reference frame (see BUILD_AND_STATUS.md), so a
// thinned-out stream would not decode.
//
// Writing happens on its own thread so the receive thread never waits for storage.
#pragma once

#include <android/log.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace replay {

constexpr int64_t kSegmentUs = 2000000;
constexpr size_t kMaxQueuedBytes = 64u << 20;   // ~8 s of the heaviest mode; more means storage can't keep up

inline int64_t wallUs() {
    timeval tv {};
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}

struct Segment {
    std::string path;
    int64_t startMs = 0;
    int64_t endMs = 0;
};

// Segment files in dir, oldest first. endMs is the file's modification time (or the next segment's start).
inline std::vector<Segment> listSegments(const std::string& dir) {
    std::vector<Segment> out;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
        return out;
    }
    while (dirent* e = readdir(d)) {
        long long ms = 0;
        if (std::sscanf(e->d_name, "seg_%lld.qvr", &ms) != 1) {
            continue;
        }
        Segment s;
        s.path = dir + "/" + e->d_name;
        s.startMs = ms;
        struct stat st {};
        if (stat(s.path.c_str(), &st) == 0) {
            s.endMs = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
        }
        s.endMs = std::max(s.endMs, s.startMs);
        out.push_back(std::move(s));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const Segment& a, const Segment& b) { return a.startMs < b.startMs; });
    for (size_t i = 0; i + 1 < out.size(); ++i) {
        out[i].endMs = std::min(out[i].endMs, out[i + 1].startMs);
    }
    return out;
}

class Recorder {
public:
    ~Recorder() { configure("", 0); }

    // dir empty or maxSeconds <= 0: recording off and all segments deleted.
    void configure(const std::string& dir, int maxSeconds) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (dir == dir_ && maxSeconds == maxSeconds_) {
                return;
            }
        }
        stopThread();
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string oldDir = dir_;
        dir_ = dir;
        maxSeconds_ = maxSeconds;
        if (maxSeconds <= 0) {
            const std::string target = dir.empty() ? oldDir : dir;
            if (!target.empty()) {
                for (const Segment& s : listSegments(target)) {
                    unlink(s.path.c_str());
                }
            }
            __android_log_print(ANDROID_LOG_INFO, "QuestVisor", "replay: recording off");
            return;
        }
        mkdir(dir.c_str(), 0700);
        stopping_ = false;
        thread_ = std::thread([this] { writerLoop(); });
        __android_log_print(ANDROID_LOG_INFO, "QuestVisor", "replay: keeping the last %d s in %s", maxSeconds,
                            dir.c_str());
    }

    bool enabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return maxSeconds_ > 0 && !dir_.empty();
    }

    std::string dir() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dir_;
    }

    // Pauses writing (during playback, so the footage being watched is not pruned) and closes the open segment.
    void setPaused(bool paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = paused;
        if (paused) {
            queue_.push_back(Item{0, 0, {}});   // type 0: close the current segment
            cv_.notify_one();
        }
    }

    // Parameter sets as seen by the decoder (with start codes); written at the head of every segment.
    void setParamSet(int nalKey, const uint8_t* data, size_t len, bool isH265) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isH265 != paramsH265_) {
            vps_.clear();
            sps_.clear();
            pps_.clear();
            paramsH265_ = isH265;
            needParams_ = true;
        }
        std::vector<uint8_t>* slot = nalKey == 0 ? &vps_ : nalKey == 1 ? &sps_ : &pps_;
        slot->assign(data, data + len);
    }

    void write(const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (maxSeconds_ <= 0 || paused_ || stopping_ || len == 0) {
            return;
        }
        if (queuedBytes_ + len > kMaxQueuedBytes) {
            if (!overflowLogged_) {
                __android_log_print(ANDROID_LOG_WARN, "QuestVisor", "replay: storage too slow, dropping input");
                overflowLogged_ = true;
            }
            needParams_ = true;     // restart cleanly at the next segment
            return;
        }
        queue_.push_back(Item{2, wallUs(), std::vector<uint8_t>(data, data + len)});
        queuedBytes_ += len;
        cv_.notify_one();
    }

private:
    struct Item {
        int type;
        int64_t tUs;
        std::vector<uint8_t> data;
    };

    void stopThread() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            cv_.notify_one();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    static void putRecord(FILE* f, uint8_t type, int64_t tUs, const uint8_t* data, uint32_t len) {
        std::fwrite(&type, 1, 1, f);
        std::fwrite(&tUs, 8, 1, f);
        std::fwrite(&len, 4, 1, f);
        if (len > 0) {
            std::fwrite(data, 1, len, f);
        }
    }

    void writerLoop() {
        FILE* file = nullptr;
        int64_t segStartUs = 0;
        bool splitPending = false;
        for (;;) {
            Item item;
            std::vector<uint8_t> params;
            std::string dir;
            int maxSeconds = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {   // stopping and drained
                    break;
                }
                item = std::move(queue_.front());
                queue_.pop_front();
                queuedBytes_ -= item.data.size();
                if (item.type == 2) {
                    overflowLogged_ = false;
                }
                dir = dir_;
                maxSeconds = maxSeconds_;
                params.insert(params.end(), vps_.begin(), vps_.end());
                params.insert(params.end(), sps_.begin(), sps_.end());
                params.insert(params.end(), pps_.begin(), pps_.end());
                if (needParams_) {
                    splitPending = true;
                    needParams_ = false;
                }
            }
            if (item.type == 0) {
                if (file != nullptr) {
                    std::fclose(file);
                    file = nullptr;
                }
                continue;
            }
            // A new segment needs parameter sets to be playable on its own; wait for them on a fresh stream.
            if (file == nullptr || splitPending || item.tUs - segStartUs >= kSegmentUs) {
                if (params.empty()) {
                    continue;
                }
                if (file != nullptr) {
                    std::fclose(file);
                }
                char name[64];
                std::snprintf(name, sizeof(name), "/seg_%013lld.qvr", static_cast<long long>(item.tUs / 1000));
                file = std::fopen((dir + name).c_str(), "wb");
                if (file == nullptr) {
                    continue;
                }
                std::fwrite("QVR1", 1, 4, file);
                putRecord(file, 1, item.tUs, params.data(), static_cast<uint32_t>(params.size()));
                segStartUs = item.tUs;
                splitPending = false;
                prune(dir, maxSeconds, item.tUs / 1000);
            }
            putRecord(file, 2, item.tUs, item.data.data(), static_cast<uint32_t>(item.data.size()));
        }
        if (file != nullptr) {
            std::fclose(file);
        }
    }

    // Deletes segments that ended more than maxSeconds before nowMs.
    static void prune(const std::string& dir, int maxSeconds, int64_t nowMs) {
        const int64_t keepFromMs = nowMs - static_cast<int64_t>(maxSeconds) * 1000;
        for (const Segment& s : listSegments(dir)) {
            if (s.endMs < keepFromMs) {
                unlink(s.path.c_str());
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::deque<Item> queue_;
    size_t queuedBytes_ = 0;
    bool stopping_ = false;
    bool paused_ = false;
    bool overflowLogged_ = false;
    bool needParams_ = false;
    std::string dir_;
    int maxSeconds_ = 0;
    bool paramsH265_ = true;
    std::vector<uint8_t> vps_, sps_, pps_;
};

}  // namespace replay

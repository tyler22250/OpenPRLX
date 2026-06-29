// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 tyler22250
//
// OpenPRLX — owned runtime context.
//
// Replaces the old QmlNativeAPI singleton + the file-static globals in the wfb
// receive path with ONE object created in main() and passed by reference to the
// pieces that need it (WFBReceiver, the health/control server). This removes the
// mutable-global state and the static-destruction teardown race: the egress
// socket lives and dies with this object's scope in main().
//
#ifndef OPENPRLX_CONTEXT_H
#define OPENPRLX_CONTEXT_H

#include <winsock2.h> // must precede any <windows.h> pulled in by driver headers

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>

// Overwrite a buffer in a way the compiler can't optimize away (no libsodium
// dependency in this header). Used to wipe transient copies of the wfb key.
inline void secureZero(void *p, size_t n) {
    volatile unsigned char *vp = static_cast<volatile unsigned char *>(p);
    while (n--) {
        *vp++ = 0;
    }
}

// Capture-pipeline lifecycle (daemon-level). A capture fault moves to Error while
// the process stays alive (fail-fast at the session, not the process).
enum class SessionState { Idle, Starting, Streaming, Error, Stopping };

inline const char *stateName(SessionState s) {
    switch (s) {
    case SessionState::Idle: return "idle";
    case SessionState::Starting: return "starting";
    case SessionState::Streaming: return "streaming";
    case SessionState::Error: return "error";
    case SessionState::Stopping: return "stopping";
    }
    return "unknown";
}

// Control-API version (handshake; bump on incompatible field/route changes).
inline constexpr const char *kOpenprlxApiVersion = "1";

// Video codec on the wfb RTP stream. AUTO is sniffed from the first packet; an
// explicit --codec pins it. Stored atomically (capture thread writes, server reads).
enum class Codec { Auto, H264, H265 };

inline const char *codecName(Codec c) {
    switch (c) {
    case Codec::H264: return "H264";
    case Codec::H265: return "H265";
    default: return "AUTO";
    }
}

inline Codec parseCodec(const std::string &s) {
    if (s == "H264" || s == "h264") return Codec::H264;
    if (s == "H265" || s == "h265" || s == "HEVC" || s == "hevc") return Codec::H265;
    return Codec::Auto;
}

// Desired session config: seeded from CLI, merged by POST /start, reused on restart.
struct SessionConfig {
    std::string vidpid;
    int channel = 161;
    int width = 0;
    std::string key = "gs.key";
};

// Mailbox: the control server STAGES requests here; the supervisor (main) drains
// start/stop and the capture thread drains tune/key (Step 5). The server thread
// never touches the device/aggregator directly.
struct ControlRequests {
    bool startPending = false;
    bool stopPending = false;
    bool tunePending = false;
    int pendingChannel = 0;
    int pendingWidth = -1;      // -1 = keep current width
    bool keyPending = false;       // sealed-key reseed staged (Step 7)
    uint8_t pendingKey[64] = { 0 }; // decrypted wfb key [rx_secret(32)][tx_public(32)]
};

struct OpenPrlxContext {
    OpenPrlxContext() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
            wsaOk_ = true;
            sendFd = socket(AF_INET, SOCK_DGRAM, 0);
        }
    }
    ~OpenPrlxContext() {
        if (sendFd != INVALID_SOCKET) {
            closesocket(sendFd);
        }
        if (wsaOk_) {
            WSACleanup();
        }
    }
    OpenPrlxContext(const OpenPrlxContext &) = delete;
    OpenPrlxContext &operator=(const OpenPrlxContext &) = delete;

    // Simple stderr logger (was QmlNativeAPI::PutLog).
    void log(const std::string &level, const std::string &msg) {
        std::cerr << "[" << level << "] " << msg << std::endl;
    }

    // Move the session to Error with a message (thread-safe); also logs it.
    void setError(const std::string &msg) {
        {
            std::lock_guard<std::mutex> lk(errMutex);
            lastError = msg;
        }
        state = SessionState::Error;
        log("error", msg);
    }
    std::string error() {
        std::lock_guard<std::mutex> lk(errMutex);
        return lastError;
    }

    // Reset the one-shot stream description so the next RTP packet re-describes the
    // stream (codec/pt/ssrc). Called on a live retune — the new channel may carry a
    // different air unit/codec. `codec` returns to the original hint so AUTO re-sniffs
    // while an explicit --codec stays pinned.
    void resetDetection() {
        described = false;
        rtpPayloadType = -1;
        codec = codecHint.load();
        haveParamSets = false;
        std::lock_guard<std::mutex> lk(sdpMutex);
        sdpVps.clear();
        sdpSps.clear();
        sdpPps.clear();
    }

    // --- session config (mutex-guarded) ---
    void setSessionConfig(const SessionConfig &c) {
        std::lock_guard<std::mutex> lk(sessMutex);
        sessionCfg_ = c;
    }
    SessionConfig sessionConfig() {
        std::lock_guard<std::mutex> lk(sessMutex);
        return sessionCfg_;
    }
    // Apply only the fields a POST /start body actually carried (partial merge).
    void mergeSessionConfig(bool hasVidpid, const std::string &vidpid, bool hasChannel, int channel,
                            bool hasWidth, int width, bool hasKey, const std::string &key) {
        std::lock_guard<std::mutex> lk(sessMutex);
        if (hasVidpid) sessionCfg_.vidpid = vidpid;
        if (hasChannel) sessionCfg_.channel = channel;
        if (hasWidth) sessionCfg_.width = width;
        if (hasKey) sessionCfg_.key = key;
    }

    // --- control mailbox (mutex-guarded) ---
    void requestStart() { std::lock_guard<std::mutex> lk(sessMutex); reqs_.startPending = true; }
    void requestStop() { std::lock_guard<std::mutex> lk(sessMutex); reqs_.stopPending = true; }
    void requestTune(int channel, int width) {
        std::lock_guard<std::mutex> lk(sessMutex);
        reqs_.tunePending = true;
        reqs_.pendingChannel = channel;
        reqs_.pendingWidth = width;
    }
    bool takeStart() {
        std::lock_guard<std::mutex> lk(sessMutex);
        bool v = reqs_.startPending;
        reqs_.startPending = false;
        return v;
    }
    bool takeStop() {
        std::lock_guard<std::mutex> lk(sessMutex);
        bool v = reqs_.stopPending;
        reqs_.stopPending = false;
        return v;
    }
    bool takeTune(int &channel, int &width) {
        std::lock_guard<std::mutex> lk(sessMutex);
        if (!reqs_.tunePending) return false;
        reqs_.tunePending = false;
        channel = reqs_.pendingChannel;
        width = reqs_.pendingWidth;
        return true;
    }
    // Sealed-key reseed (Step 7): server stages the decrypted 64-byte key; the capture
    // thread drains it and rebuilds the Aggregator. The staged copy is wiped on take.
    void requestKey(const uint8_t *key64) {
        std::lock_guard<std::mutex> lk(sessMutex);
        std::memcpy(reqs_.pendingKey, key64, 64);
        reqs_.keyPending = true;
    }
    bool takeKey(uint8_t *out64) {
        std::lock_guard<std::mutex> lk(sessMutex);
        if (!reqs_.keyPending) return false;
        reqs_.keyPending = false;
        std::memcpy(out64, reqs_.pendingKey, 64);
        secureZero(reqs_.pendingKey, 64);
        return true;
    }

    // --- config (set in main before capture starts) ---
    int outPort = 5600; // RTP sendto target port on 127.0.0.1 (was playerPort)
    // codecHint: original --codec, immutable after start. codec: live/detected value
    // (AUTO until the first packet sniffs it; reset to codecHint on retune). Atomic
    // because the capture thread writes and the control server reads concurrently.
    std::atomic<Codec> codecHint { Codec::Auto };
    std::atomic<Codec> codec { Codec::Auto };

    // --- egress (was the file-static sendFd) ---
    SOCKET sendFd = INVALID_SOCKET;

    // --- observability counters (were QmlNativeAPI atomics) ---
    std::atomic<uint64_t> wifiFrameCount { 0 };
    std::atomic<uint64_t> wfbFrameCount { 0 };
    std::atomic<uint64_t> rtpPktCount { 0 };
    std::atomic<int> rtpPayloadType { -1 };
    std::atomic<uint32_t> ssrc { 0 };

    // One-shot stream-description latch (was the file-static `playing`).
    std::atomic<bool> described { false };

    // --- SDP parameter sets (captured from the RTP egress; served by GET /sdp) ---
    // Raw NAL bytes (no Annex-B start code), "" until seen. Written by the capture
    // thread, read by the control server — both under sdpMutex. haveParamSets flips
    // true once the set required for the current codec is complete.
    std::mutex sdpMutex;
    std::string sdpVps, sdpSps, sdpPps;
    std::atomic<bool> haveParamSets { false };

    // Capture-thread liveness; set true as the capture thread's last action.
    // Drives /health "captureAlive". Complemented by the SessionState machine.
    std::atomic<bool> captureFinished { false };

    // Daemon session state + stop intent + last error.
    std::atomic<SessionState> state { SessionState::Idle };
    std::atomic<bool> shouldStop { false };

private:
    bool wsaOk_ = false;
    std::mutex errMutex;
    std::string lastError; // guarded by errMutex
    std::mutex sessMutex;
    SessionConfig sessionCfg_;
    ControlRequests reqs_;
};

#endif // OPENPRLX_CONTEXT_H

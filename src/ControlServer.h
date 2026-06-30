// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 tyler22250
//
// OpenPRLX — localhost control + health HTTP/JSON server (header-only, Winsock only).
//
// Routes:
//   GET  /health  -> status/diagnostics JSON (+ state, apiVersion, lastError-on-error)
//   POST /start   -> stage a capture-session start (optional {vidpid,channel,width,key})
//   POST /stop    -> stage a capture-session stop
//   POST /tune    -> stage a live retune ({channel, width?})   [applied by capture loop, Step 5]
//   POST /key     -> 501 (Phase 1b: sealed in-memory key feed)
//   GET  /sdp     -> 501 (Phase 1b: stream description)
//
// The server thread only parses/auths and STAGES requests into the ctx mailbox (or reads
// atomics for /health). It NEVER touches the device/aggregator — lifecycle runs on the
// main supervisor thread, device tuning on the capture thread.
//
// Auth: a shared token (--control-token / OPENPRLX_CONTROL_TOKEN) is required on every
// route as `Authorization: Bearer <tok>` or `?token=<tok>`. No token configured -> auth
// disabled (dev convenience) with a one-time warning.
//
#ifndef OPENPRLX_CONTROL_SERVER_H
#define OPENPRLX_CONTROL_SERVER_H

#include "Context.h"
#include "KeyVault.h"
#include "Sdp.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <sodium.h>
#include <string>
#include <thread>
#include <utility>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace openprlx_http {

inline std::string toLower(std::string s) {
    for (auto &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Case-insensitive header lookup over the raw header block. "" if absent.
inline std::string headerValue(const std::string &headers, const std::string &name) {
    const std::string lhdr = toLower(headers);
    const std::string key = "\r\n" + toLower(name) + ":";
    size_t p = lhdr.find(key);
    if (p == std::string::npos) {
        return "";
    }
    p += key.size();
    size_t e = headers.find("\r\n", p);
    if (e == std::string::npos) {
        e = headers.size();
    }
    std::string v = headers.substr(p, e - p);
    size_t a = v.find_first_not_of(" \t");
    if (a == std::string::npos) {
        return "";
    }
    size_t b = v.find_last_not_of(" \t");
    return v.substr(a, b - a + 1);
}

// Extract a key from a URL query string (e.g. token from "a=1&token=xyz").
inline std::string queryValue(const std::string &query, const std::string &key) {
    size_t p = 0;
    while (p <= query.size()) {
        size_t amp = query.find('&', p);
        std::string pair = query.substr(p, amp == std::string::npos ? std::string::npos : amp - p);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            return pair.substr(eq + 1);
        }
        if (amp == std::string::npos) {
            break;
        }
        p = amp + 1;
    }
    return "";
}

// Minimal flat-JSON scalar extractors — NO nesting/escape handling (bodies are tiny,
// flat, ASCII control objects; a malformed value reads as absent, never crashes).
inline bool jsonFindString(const std::string &body, const std::string &field, std::string &out) {
    const std::string k = "\"" + field + "\"";
    size_t p = body.find(k);
    if (p == std::string::npos) {
        return false;
    }
    p = body.find(':', p + k.size());
    if (p == std::string::npos) {
        return false;
    }
    p++;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) {
        p++;
    }
    if (p >= body.size() || body[p] != '"') {
        return false;
    }
    p++;
    size_t e = body.find('"', p);
    if (e == std::string::npos) {
        return false;
    }
    out = body.substr(p, e - p);
    return true;
}

inline bool jsonFindInt(const std::string &body, const std::string &field, int &out) {
    const std::string k = "\"" + field + "\"";
    size_t p = body.find(k);
    if (p == std::string::npos) {
        return false;
    }
    p = body.find(':', p + k.size());
    if (p == std::string::npos) {
        return false;
    }
    p++;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) {
        p++;
    }
    const char *start = body.c_str() + p;
    char *end = nullptr;
    long v = std::strtol(start, &end, 10);
    if (end == start) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

inline std::string jsonEscape(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
        }
        o += c;
    }
    return o;
}

// Constant-time compare for the control-API token (avoids a timing side-channel on the
// shared secret). Length is checked first (token length is not the secret); the byte
// compare uses libsodium's constant-time sodium_memcmp.
inline bool tokenEqual(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace openprlx_http

class ControlServer {
public:
    ControlServer(int port, OpenPrlxContext &ctx, std::string token, KeyVault &vault)
        : port_(port)
        , ctx_(ctx)
        , token_(std::move(token))
        , vault_(vault)
        , pubkeyB64_(vault.pubkeyBase64()) {}
    ~ControlServer() { stop(); }

    bool start() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return false;
        }
        started_ = true; // WSACleanup paired to this exactly once in stop()
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ == INVALID_SOCKET) {
            stop();
            return false;
        }
        int yes = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port_));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            stop();
            return false;
        }
        if (listen(listenFd_, 8) != 0) {
            stop();
            return false;
        }
        if (token_.empty()) {
            ctx_.log("warn", "control API auth DISABLED (no --control-token / OPENPRLX_CONTROL_TOKEN)");
        }
        run_ = true;
        thread_ = std::thread([this]() { loop(); });
        return true;
    }

    void stop() {
        run_ = false;
        if (listenFd_ != INVALID_SOCKET) {
            closesocket(listenFd_); // unblocks accept()
            listenFd_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (started_) {
            WSACleanup();
            started_ = false;
        }
    }

private:
    void loop() {
        while (run_) {
            SOCKET c = accept(listenFd_, nullptr, nullptr);
            if (c == INVALID_SOCKET) {
                if (!run_) {
                    break;
                }
                continue;
            }
            DWORD ms = 2000; // Winsock SO_RCVTIMEO takes a DWORD ms, NOT a timeval.
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
            handle(c);
            closesocket(c);
        }
    }

    // Read request line + headers + body (up to Content-Length). false on timeout/oversize/malformed.
    bool recvRequest(SOCKET c, std::string &method, std::string &path, std::string &query,
                     std::string &headers, std::string &body) {
        std::string buf;
        char tmp[2048];
        size_t hdrEnd = std::string::npos;
        // Overall wall-clock deadline so a client dribbling bytes under the per-recv
        // SO_RCVTIMEO can't park this (single-threaded) server for long (slowloris).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (true) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            int n = recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                return false;
            }
            buf.append(tmp, n);
            if (buf.size() > 16384) {
                return false;
            }
            hdrEnd = buf.find("\r\n\r\n");
            if (hdrEnd != std::string::npos) {
                break;
            }
        }
        headers = buf.substr(0, hdrEnd);
        size_t lineEnd = headers.find("\r\n");
        std::string reqLine = headers.substr(0, lineEnd == std::string::npos ? headers.size() : lineEnd);
        size_t sp1 = reqLine.find(' ');
        size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : reqLine.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            return false;
        }
        method = reqLine.substr(0, sp1);
        std::string target = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);
        size_t q = target.find('?');
        if (q == std::string::npos) {
            path = target;
            query = "";
        } else {
            path = target.substr(0, q);
            query = target.substr(q + 1);
        }
        body = buf.substr(hdrEnd + 4);
        int contentLength = 0;
        std::string cl = openprlx_http::headerValue(headers, "Content-Length");
        if (!cl.empty()) {
            contentLength = std::atoi(cl.c_str());
        }
        if (contentLength > 16384) {
            return false;
        }
        while (static_cast<int>(body.size()) < contentLength) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            int n = recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                break;
            }
            body.append(tmp, n);
            if (body.size() > 16384) {
                return false;
            }
        }
        return true;
    }

    bool authorized(const std::string &headers, const std::string &query) {
        if (token_.empty()) {
            return true; // auth disabled
        }
        std::string auth = openprlx_http::headerValue(headers, "Authorization");
        const std::string bearer = "Bearer ";
        if (auth.size() > bearer.size()
            && openprlx_http::toLower(auth.substr(0, bearer.size())) == "bearer ") {
            if (openprlx_http::tokenEqual(auth.substr(bearer.size()), token_)) {
                return true;
            }
        }
        return openprlx_http::tokenEqual(openprlx_http::queryValue(query, "token"), token_);
    }

    void respond(SOCKET c, int code, const std::string &reason, const std::string &body,
                 const std::string &extraHeaders = "", const std::string &contentType = "application/json") {
        std::string resp = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n"
            + "Content-Type: " + contentType + "\r\n"
            + "Connection: close\r\n" + extraHeaders
            + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        send(c, resp.c_str(), static_cast<int>(resp.size()), 0);
    }

    void methodNotAllowed(SOCKET c, const std::string &allow) {
        respond(c, 405, "Method Not Allowed", "{\"error\":\"method not allowed\"}", "Allow: " + allow + "\r\n");
    }

    void handle(SOCKET c) {
        std::string method, path, query, headers, body;
        if (!recvRequest(c, method, path, query, headers, body)) {
            respond(c, 400, "Bad Request", "{\"error\":\"bad request\"}");
            return;
        }
        if (!authorized(headers, query)) {
            respond(c, 401, "Unauthorized", "{\"error\":\"unauthorized\"}");
            return;
        }
        if (path == "/health") {
            if (method != "GET") {
                methodNotAllowed(c, "GET");
                return;
            }
            respond(c, 200, "OK", health());
        } else if (path == "/start") {
            if (method != "POST") {
                methodNotAllowed(c, "POST");
                return;
            }
            handleStart(c, body);
        } else if (path == "/stop") {
            if (method != "POST") {
                methodNotAllowed(c, "POST");
                return;
            }
            ctx_.requestStop();
            respond(c, 202, "Accepted", "{\"accepted\":true}");
        } else if (path == "/tune") {
            if (method != "POST") {
                methodNotAllowed(c, "POST");
                return;
            }
            handleTune(c, body);
        } else if (path == "/sdp") {
            if (method != "GET") {
                methodNotAllowed(c, "GET");
                return;
            }
            handleSdp(c);
        } else if (path == "/key") {
            if (method != "POST") {
                methodNotAllowed(c, "POST");
                return;
            }
            handleKey(c, body);
        } else if (path == "/pubkey") {
            if (method != "GET") {
                methodNotAllowed(c, "GET");
                return;
            }
            respond(c, 200, "OK", "{\"pubkey\":\"" + pubkeyB64_ + "\"}");
        } else {
            respond(c, 404, "Not Found", "{\"error\":\"not found\"}");
        }
    }

    void handleStart(SOCKET c, const std::string &body) {
        // Reject a busy start BEFORE touching the persisted config: a rejected request must
        // have no side effects (it must not silently reconfigure the next session).
        const auto s = ctx_.state.load();
        if (s == SessionState::Starting || s == SessionState::Streaming || s == SessionState::Stopping) {
            respond(c, 409, "Conflict", "{\"error\":\"already running\"}");
            return;
        }
        std::string vidpid, key;
        int channel = 0, width = 0;
        bool hasVidpid = openprlx_http::jsonFindString(body, "vidpid", vidpid);
        bool hasKey = openprlx_http::jsonFindString(body, "key", key);
        bool hasChannel = openprlx_http::jsonFindInt(body, "channel", channel);
        bool hasWidth = openprlx_http::jsonFindInt(body, "width", width);
        if ((hasChannel && !isValidChannel(channel)) || (hasWidth && !isValidWidth(width))) {
            respond(c, 400, "Bad Request", "{\"error\":\"channel 1..177 / width 0..2\"}");
            return;
        }
        ctx_.mergeSessionConfig(hasVidpid, vidpid, hasChannel, channel, hasWidth, width, hasKey, key);
        ctx_.requestStart();
        respond(c, 202, "Accepted", "{\"accepted\":true,\"state\":\"starting\"}");
    }

    void handleTune(SOCKET c, const std::string &body) {
        // A live retune only makes sense on an active session; staging one while not
        // streaming would otherwise be applied to the NEXT start, overriding its channel.
        if (ctx_.state.load() != SessionState::Streaming) {
            respond(c, 409, "Conflict", "{\"error\":\"not streaming\"}");
            return;
        }
        int channel = 0, width = -1;
        if (!openprlx_http::jsonFindInt(body, "channel", channel)) {
            respond(c, 400, "Bad Request", "{\"error\":\"channel required\"}");
            return;
        }
        openprlx_http::jsonFindInt(body, "width", width); // optional; -1 = keep current
        if (!isValidChannel(channel) || (width != -1 && !isValidWidth(width))) {
            respond(c, 400, "Bad Request", "{\"error\":\"channel 1..177 / width 0..2\"}");
            return;
        }
        ctx_.requestTune(channel, width);
        respond(c, 202, "Accepted", "{\"accepted\":true}");
    }

    // Serve an SDP for the forwarded RTP stream. Progressive: returns 503 until the
    // codec is detected, then 200 with m=/rtpmap, gaining the a=fmtp sprop line once
    // the parameter sets have been captured from the air.
    void handleSdp(SOCKET c) {
        const Codec codec = ctx_.codec.load();
        if (codec == Codec::Auto) {
            respond(c, 503, "Service Unavailable", "{\"sdpReady\":false,\"reason\":\"codec not detected\"}");
            return;
        }
        const int pt = ctx_.rtpPayloadType.load();
        std::string vps, sps, pps;
        {
            std::lock_guard<std::mutex> lk(ctx_.sdpMutex);
            vps = ctx_.sdpVps;
            sps = ctx_.sdpSps;
            pps = ctx_.sdpPps;
        }
        const std::string sdp = sdpBuild(codec, pt, ctx_.outPort, vps, sps, pps);
        respond(c, 200, "OK", sdp, "", "application/sdp");
    }

    // Accept a libsodium-sealed wfb key, open it in memory, and stage a live reseed.
    // Body: {"sealed":"<base64 crypto_box_seal(64-byte key, daemon_pubkey)>"}.
    void handleKey(SOCKET c, const std::string &body) {
        if (!vault_.ok()) {
            respond(c, 503, "Service Unavailable", "{\"error\":\"key vault unavailable\"}");
            return;
        }
        std::string b64;
        if (!openprlx_http::jsonFindString(body, "sealed", b64) || b64.empty()) {
            respond(c, 400, "Bad Request", "{\"error\":\"missing sealed key\"}");
            return;
        }
        uint8_t key[64];
        if (!vault_.openSealedBase64(b64, key)) {
            respond(c, 400, "Bad Request", "{\"error\":\"could not open sealed key\"}");
            return;
        }
        ctx_.requestKey(key);     // stage for the capture thread to apply
        secureZero(key, sizeof(key)); // wipe our local copy immediately
        respond(c, 202, "Accepted", "{\"accepted\":true}");
    }

    std::string health() {
        const auto &api = ctx_;
        const uint64_t pkts = api.rtpPktCount.load();
        const bool streaming = pkts > lastPkts_; // packets since previous poll
        lastPkts_ = pkts;
        const std::string codec = codecName(api.codec.load());
        const SessionState st = api.state.load();
        std::string s = "{";
        s += "\"status\":\"ok\",";
        s += "\"apiVersion\":\"" + std::string(kOpenprlxApiVersion) + "\",";
        s += "\"state\":\"" + std::string(stateName(st)) + "\",";
        s += "\"streaming\":";
        s += (streaming ? "true" : "false");
        s += ",";
        // captureAlive: is a capture session thread currently active? Derived from the
        // state machine (not the captureFinished edge-flag, which the supervisor clears
        // on reap) so it reads false the moment a fault drops the session to Error.
        const bool captureAlive = (st == SessionState::Starting || st == SessionState::Streaming
                                   || st == SessionState::Stopping);
        s += "\"captureAlive\":";
        s += (captureAlive ? "true" : "false");
        s += ",";
        s += "\"packetsForwarded\":" + std::to_string(pkts) + ",";
        s += "\"wfbFrames\":" + std::to_string(api.wfbFrameCount.load()) + ",";
        s += "\"wifiFrames\":" + std::to_string(api.wifiFrameCount.load()) + ",";
        s += "\"codec\":\"" + codec + "\",";
        s += "\"rtpPayloadType\":" + std::to_string(api.rtpPayloadType.load()) + ",";
        s += "\"ssrc\":" + std::to_string(api.ssrc.load()) + ",";
        // sdpReady: codec known -> GET /sdp returns 200. paramSets: a=fmtp sprop present.
        s += "\"sdpReady\":" + std::string(api.codec.load() != Codec::Auto ? "true" : "false") + ",";
        s += "\"paramSets\":" + std::string(api.haveParamSets.load() ? "true" : "false") + ",";
        // Daemon public key for the sealed POST /key feed ("" if the vault failed to init).
        s += "\"pubkey\":\"" + pubkeyB64_ + "\",";
        s += "\"port\":" + std::to_string(api.outPort);
        if (st == SessionState::Error) {
            s += ",\"lastError\":\"" + openprlx_http::jsonEscape(ctx_.error()) + "\"";
        }
        s += "}";
        return s;
    }

    int port_;
    OpenPrlxContext &ctx_;
    std::string token_;
    KeyVault &vault_;
    std::string pubkeyB64_; // cached base64 daemon pubkey (published; safe)
    SOCKET listenFd_ = INVALID_SOCKET;
    std::atomic<bool> run_ { false };
    bool started_ = false;
    std::thread thread_;
    uint64_t lastPkts_ = 0;
};

#endif // OPENPRLX_CONTROL_SERVER_H

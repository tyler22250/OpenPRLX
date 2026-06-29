// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 tyler22250
//
// openprlx_main.cpp — headless entry point for OpenPRLX (wfb-ng -> RTP forwarder).
//
// Receives an OpenIPC air unit's wfb-ng broadcast on an RTL8812AU (WinUSB/Zadig-bound),
// decrypts it with the wfb key, and forwards the RTP to 127.0.0.1:<out-port> (default
// 5600) — where a downstream RTP consumer ingests it. Exposes GET /health on
// 127.0.0.1:<health-port> (default 9301). No GUI/QML.
//
// Runs as a long-lived daemon. With --vidpid it auto-starts a capture session; without
// it, it idles awaiting control (control verbs land in a later step). A capture fault
// drops the session to an error state but keeps the process alive — the supervising
// process decides what to do via /health — instead of exiting.
//
#include "Context.h"
#include "ControlServer.h"
#include "KeyVault.h"
#include "wifi/WFBReceiver.h"

#include <sodium.h> // for the --seal-key reference sealer

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_run { true };
void onSignal(int) { g_run = false; }

std::string argVal(int argc, char **argv, const std::string &key, const std::string &def) {
    for (int i = 1; i < argc - 1; ++i) {
        if (key == argv[i]) {
            return argv[i + 1];
        }
    }
    return def;
}

bool hasFlag(int argc, char **argv, const std::string &key) {
    for (int i = 1; i < argc; ++i) {
        if (key == argv[i]) {
            return true;
        }
    }
    return false;
}

void usage() {
    std::cerr << "usage:\n"
                 "  openprlx [--vidpid VID:PID] [--channel N] [--width W] [--key PATH]\n"
                 "           [--out-port 5600] [--health-port 9301] [--codec AUTO|H264|H265]\n"
                 "           [--control-token TOKEN]\n"
                 "  openprlx --list-devices\n"
                 "\n"
                 "  With --vidpid the capture session auto-starts; without it the daemon idles\n"
                 "  awaiting POST /start. --control-token (or env OPENPRLX_CONTROL_TOKEN) guards\n"
                 "  the control API. --width is the ChannelWidth_t ordinal (0 = 20MHz, 1 = 40MHz).\n";
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 1) {
        usage();
        return 2;
    }

    // Enumerate libusb adapters as a JSON array (for an adapter picker).
    if (hasFlag(argc, argv, "--list-devices")) {
        auto list = WFBReceiver::GetDongleList();
        std::cout << "[";
        for (size_t i = 0; i < list.size(); ++i) {
            std::cout << "\"" << list[i] << "\"";
            if (i + 1 < list.size()) {
                std::cout << ",";
            }
        }
        std::cout << "]" << std::endl;
        return 0;
    }

    // Reference sealer (utility): openprlx --seal-key <daemon_pubkey_b64> [--key PATH]
    // reads the 64-byte wfb key file, crypto_box_seal()s it to the given pubkey, and
    // prints the base64 sealed blob for POST /key. Self-contained (no device); generic.
    if (hasFlag(argc, argv, "--seal-key")) {
        if (sodium_init() < 0) {
            std::cerr << "[error] libsodium init failed" << std::endl;
            return 2;
        }
        const std::string pkB64 = argVal(argc, argv, "--seal-key", "");
        const std::string keyPath = argVal(argc, argv, "--key", "gs.key");
        uint8_t pk[crypto_box_PUBLICKEYBYTES];
        size_t pkLen = 0;
        if (pkB64.empty()
            || sodium_base642bin(pk, sizeof(pk), pkB64.c_str(), pkB64.size(), nullptr, &pkLen, nullptr,
                                 sodium_base64_VARIANT_ORIGINAL)
                != 0
            || pkLen != sizeof(pk)) {
            std::cerr << "[error] --seal-key needs a valid base64 32-byte pubkey" << std::endl;
            return 2;
        }
        uint8_t key[64];
        FILE *fp = std::fopen(keyPath.c_str(), "rb");
        if (!fp || std::fread(key, 1, sizeof(key), fp) != sizeof(key)) {
            if (fp) std::fclose(fp);
            std::cerr << "[error] could not read 64-byte key file: " << keyPath << std::endl;
            return 2;
        }
        std::fclose(fp);
        uint8_t sealed[crypto_box_SEALBYTES + 64];
        crypto_box_seal(sealed, key, sizeof(key), pk);
        sodium_memzero(key, sizeof(key));
        char out[sodium_base64_ENCODED_LEN(sizeof(sealed), sodium_base64_VARIANT_ORIGINAL)];
        sodium_bin2base64(out, sizeof(out), sealed, sizeof(sealed), sodium_base64_VARIANT_ORIGINAL);
        std::cout << out << std::endl;
        return 0;
    }

    const std::string vidpid = argVal(argc, argv, "--vidpid", "");

    int channel = 161;
    int width = 0; // 20MHz
    int outPort = 5600;
    int healthPort = 9301;
    try {
        channel = std::stoi(argVal(argc, argv, "--channel", "161"));
        width = std::stoi(argVal(argc, argv, "--width", "0"));
        outPort = std::stoi(argVal(argc, argv, "--out-port", "5600"));
        healthPort = std::stoi(argVal(argc, argv, "--health-port", "9301"));
    } catch (const std::exception &e) {
        std::cerr << "[error] bad numeric argument: " << e.what() << std::endl;
        usage();
        return 2;
    }
    const std::string key = argVal(argc, argv, "--key", "gs.key");
    const std::string codec = argVal(argc, argv, "--codec", "AUTO");
    std::string token = argVal(argc, argv, "--control-token", "");
    if (token.empty()) {
        if (const char *env = std::getenv("OPENPRLX_CONTROL_TOKEN")) {
            token = env;
        }
    }

    // The capture thread reads ctx.outPort for its sendto() target and sets ctx.codec
    // on the first packet (AUTO -> H264/H265). ctx owns the egress socket; its scope
    // (this function) outlives the capture thread, so teardown is race-free. The session
    // config (vidpid/channel/width/key) is seeded here; a POST /start body can override it.
    OpenPrlxContext ctx;
    ctx.outPort = outPort;
    const Codec codecHint = parseCodec(codec);
    ctx.codecHint = codecHint; // original --codec, immutable after start
    ctx.codec = codecHint;     // AUTO re-sniffs on the first packet / after each retune
    {
        SessionConfig cfg;
        cfg.vidpid = vidpid;
        cfg.channel = channel;
        cfg.width = width;
        cfg.key = key;
        ctx.setSessionConfig(cfg);
    }
    WFBReceiver receiver(ctx);

    // Daemon keypair for the sealed in-memory key feed. If it fails to init, the sealed
    // POST /key path is disabled but the --key file fallback still works.
    KeyVault vault;
    if (!vault.ok()) {
        std::cerr << "[warn] key vault init failed — sealed POST /key disabled (--key file still works)"
                  << std::endl;
    } else {
        std::cerr << "[info] control key feed pubkey: " << vault.pubkeyBase64() << std::endl;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    ControlServer control(healthPort, ctx, token, vault);
    if (!control.start()) {
        std::cerr << "[warn] could not bind control port " << healthPort
                  << " — continuing without control/health" << std::endl;
    }

    if (!vidpid.empty()) {
        std::cerr << "[info] openprlx start: vidpid=" << vidpid << " channel=" << channel
                  << " width=" << width << " key=" << key << " out=127.0.0.1:" << outPort
                  << " control=127.0.0.1:" << healthPort << std::endl;
        ctx.requestStart(); // the supervisor loop performs the actual Start()
    } else {
        std::cerr << "[info] openprlx idle (no --vidpid); awaiting POST /start. control=127.0.0.1:"
                  << healthPort << std::endl;
    }

    // Supervisor run loop: stay alive until signaled; drive the capture lifecycle from
    // the control mailbox. All device/lifecycle calls happen on this thread.
    while (g_run.load()) {
        // (a) Reap a finished capture thread so a later Start()'s joinable guard passes.
        if (ctx.captureFinished.load()) {
            const auto s = ctx.state.load();
            if (s == SessionState::Idle || s == SessionState::Error) {
                receiver.Join();
                ctx.captureFinished = false;
            }
        }
        // (b) Stop request.
        if (ctx.takeStop()) {
            receiver.Stop();
        }
        // (c) Start request (only once not busy and any previous thread is reaped).
        if (ctx.takeStart()) {
            const auto s = ctx.state.load();
            const bool busy = (s == SessionState::Starting || s == SessionState::Streaming
                               || s == SessionState::Stopping);
            if (busy || ctx.captureFinished.load()) {
                ctx.requestStart(); // not ready yet — retry next tick
            } else {
                SessionConfig cfg = ctx.sessionConfig();
                if (cfg.vidpid.empty()) {
                    ctx.setError("start requested with no vidpid configured");
                } else {
                    receiver.Start(cfg.vidpid, static_cast<uint8_t>(cfg.channel), cfg.width, cfg.key);
                }
            }
        }
        // (d) tune/key requests are drained by the capture thread at loop-top (Step 5).
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cerr << "[info] stopping..." << std::endl;
    receiver.Stop();
    // Deterministic shutdown: join the capture thread (<=5s for the driver's read timeout)
    // before the control server and ctx (the egress socket) tear down.
    receiver.Join();
    control.stop();
    return 0;
}

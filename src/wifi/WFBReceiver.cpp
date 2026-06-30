//
// Created by Talus on 2024/6/10.
//

#include "WFBReceiver.h"
#include "Context.h"
#include "RxFrame.h"
#include "Sdp.h"
#include "WFBProcessor.h"
#include "WiFiDriver.h"
#include "logger.h"

#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "Rtp.h"

std::vector<std::string> WFBReceiver::GetDongleList() {
    std::vector<std::string> list;

    libusb_context *findctx;
    // Initialize libusb
    libusb_init(&findctx);

    // Get list of USB devices
    libusb_device **devs;
    ssize_t count = libusb_get_device_list(findctx, &devs);
    if (count < 0) {
        return list;
    }

    // Iterate over devices
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device *dev = devs[i];
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) == 0) {
            // Check if the device is using libusb driver
            if (desc.bDeviceClass == LIBUSB_CLASS_PER_INTERFACE) {
                std::stringstream ss;
                ss << std::setw(4) << std::setfill('0') << std::hex << desc.idVendor << ":";
                ss << std::setw(4) << std::setfill('0') << std::hex << desc.idProduct;
                list.push_back(ss.str());
            }
        }
    }
    std::sort(list.begin(), list.end(), [](std::string &a, std::string &b) {
        static std::vector<std::string> specialStrings = { "0b05:17d2", "0bda:8812", "0bda:881a" };
        auto itA = std::find(specialStrings.begin(), specialStrings.end(), a);
        auto itB = std::find(specialStrings.begin(), specialStrings.end(), b);
        if (itA != specialStrings.end() && itB == specialStrings.end()) {
            return true;
        }
        if (itB != specialStrings.end() && itA == specialStrings.end()) {
            return false;
        }
        return a < b;
    });

    // Free the list of devices
    libusb_free_device_list(devs, 1);

    // Deinitialize libusb
    libusb_exit(findctx);
    return list;
}

WFBReceiver::WFBReceiver(OpenPrlxContext &ctx)
    : ctx_(ctx) {}

WFBReceiver::~WFBReceiver() {
    Stop();
    Join();
}

bool WFBReceiver::Start(const std::string &vidPid, uint8_t channel, int channelWidth, const std::string &kPath) {
    if (usbThread_.joinable()) {
        return false; // already running
    }
    ctx_.state = SessionState::Starting;
    ctx_.wifiFrameCount = 0;
    ctx_.wfbFrameCount = 0;
    ctx_.rtpPktCount = 0;
    ctx_.captureFinished = false;
    ctx_.resetDetection(); // described=false, pt=-1, codec=codecHint (AUTO re-sniffs)
    ctx_.shouldStop = false;
    ctx_.clearTune(); // drop any tune staged in a prior session so this start honors its
                      // configured channel rather than a stale retune

    keyPath_ = kPath;
    int rc;

    // get vid pid
    std::istringstream iss(vidPid);
    unsigned int wifiDeviceVid, wifiDevicePid;
    char c;
    iss >> std::hex >> wifiDeviceVid >> c >> wifiDevicePid;

    auto logger = std::make_shared<Logger>(
        [this](const std::string &level, const std::string &msg) { ctx_.log(level, msg); });

    rc = libusb_init(&usbCtx_);
    if (rc < 0) {
        ctx_.setError("libusb_init failed");
        return false;
    }
    devHandle_ = libusb_open_device_with_vid_pid(usbCtx_, wifiDeviceVid, wifiDevicePid);
    if (devHandle_ == nullptr) {
        ctx_.setError("adapter " + vidPid + " not found (is it WinUSB/Zadig-bound?)");
        libusb_exit(usbCtx_);
        usbCtx_ = nullptr;
        return false;
    }

    /*Check if kenel driver attached*/
    if (libusb_kernel_driver_active(devHandle_, 0)) {
        rc = libusb_detach_kernel_driver(devHandle_, 0); // detach driver
    }
    rc = libusb_claim_interface(devHandle_, 0);

    if (rc < 0) {
        ctx_.setError("could not claim interface on " + vidPid + " (adapter busy?)");
        libusb_close(devHandle_);
        libusb_exit(usbCtx_);
        devHandle_ = nullptr;
        usbCtx_ = nullptr;
        return false;
    }

    // Build the wfb aggregator now that the key path is known. Held as a member
    // (not a function-static) so it can be rebuilt on a live key change (Step 5).
    // The ctor THROWS if the key file is missing/short — catch it so a bad key is a
    // graceful session error (daemon stays alive), not a process-killing exception.
    const uint32_t channel_id_f = (kLinkId << 8) + kVideoRadioPort;
    try {
        aggregator_ = std::make_unique<Aggregator>(
            keyPath_.c_str(), 0 /*epoch*/, channel_id_f,
            [this](uint8_t *payload, uint16_t packet_size) { handleRtp(payload, packet_size); });
    } catch (const std::exception &e) {
        ctx_.setError(std::string("key load failed: ") + e.what());
        libusb_release_interface(devHandle_, 0);
        libusb_close(devHandle_);
        libusb_exit(usbCtx_);
        devHandle_ = nullptr;
        usbCtx_ = nullptr;
        return false;
    }

    // Mark Streaming BEFORE spawning the capture thread so the thread's own terminal
    // state writes (Error via setError, or Idle on a clean exit) always win. Writing it
    // AFTER the spawn could clobber a fast-failed thread's terminal state, leaving
    // state=Streaming + captureFinished=true so the supervisor (which reaps only on
    // Idle/Error) never joins the dead thread and every later /start returns 409.
    ctx_.state = SessionState::Streaming;

    usbThread_ = std::thread([this, channel, channelWidth, logger]() {
        WiFiDriver wifi_driver { logger };
        try {
            rtlDevice_ = wifi_driver.CreateRtlDevice(devHandle_);
            SelectedChannel sc {
                .Channel = channel,
                .ChannelOffset = 0,
                .ChannelWidth = static_cast<ChannelWidth_t>(channelWidth),
            };
            rtlDevice_->BringUp(sc);
            curChannel_ = channel;
            curWidth_ = static_cast<ChannelWidth_t>(channelWidth);
            // Owned RX loop (replaces the driver's blocking Init() loop): apply any
            // staged tune at loop-top, then pump one read window. ReadOnce() returns
            // within ~5s even in RF silence, so a retune is never stuck waiting.
            // usb_rc lets us fail the session on an unplugged adapter (the driver
            // otherwise returns empty on both silence AND device-gone). A bare timeout
            // (silence) is fine; NO_DEVICE/NOT_FOUND is fatal now, and a sustained run
            // of any other USB error trips the same fail-fast as a backstop.
            int consecutiveErrors = 0;
            auto lastUsbErrLog = std::chrono::steady_clock::time_point {};
            // Stop is driven by the ctx_.shouldStop atomic (set by Stop() on the
            // supervisor thread). The capture thread owns rtlDevice_, so the supervisor
            // must never read it; checking the atomic here also catches a stop requested
            // during CreateRtlDevice/BringUp (the loop simply never enters).
            while (!ctx_.shouldStop.load()) {
                applyPendingControlActions();
                int usb_rc = 0;
                auto packets = rtlDevice_->ReadOnce(usb_rc);
                if (usb_rc == LIBUSB_ERROR_NO_DEVICE || usb_rc == LIBUSB_ERROR_NOT_FOUND) {
                    throw std::runtime_error("adapter disconnected (libusb " + std::to_string(usb_rc) + ")");
                }
                if (usb_rc == LIBUSB_ERROR_TIMEOUT) {
                    // Benign RF silence: leave the error streak unchanged. Resetting it on a
                    // timeout would let a steady error stream interleaved with timeouts evade
                    // the fail-fast below indefinitely.
                } else if (usb_rc < 0) {
                    auto now = std::chrono::steady_clock::now();
                    if (now - lastUsbErrLog >= std::chrono::seconds(1)) {
                        ctx_.log("warn", "USB read error (libusb " + std::to_string(usb_rc) + ")");
                        lastUsbErrLog = now;
                    }
                    if (++consecutiveErrors >= 10) {
                        throw std::runtime_error("adapter read errors (libusb " + std::to_string(usb_rc) + ")");
                    }
                } else {
                    consecutiveErrors = 0; // a clean read clears the streak
                }
                for (auto &p : packets) {
                    handle80211Frame(p);
                }
            }
        } catch (const std::runtime_error &e) {
            ctx_.setError(std::string("capture: ") + e.what());
        } catch (...) {
            ctx_.setError("capture: unknown error");
        }
        auto rc2 = libusb_release_interface(devHandle_, 0);
        if (rc2 < 0) {
            // error
        }
        logger->info("==========stopped==========");
        libusb_close(devHandle_);
        libusb_exit(usbCtx_);
        devHandle_ = nullptr;
        usbCtx_ = nullptr;
        // A clean (non-error) exit returns the session to Idle.
        if (ctx_.state.load() != SessionState::Error) {
            ctx_.state = SessionState::Idle;
        }
        // Drives /health "captureAlive" -> false once the RX thread has ended.
        ctx_.captureFinished = true;
    });

    return true;
}

void WFBReceiver::handle80211Frame(const Packet &packet) {
    ctx_.wifiFrameCount++;
    RxFrame frame(packet.Data);
    if (!frame.IsValidWfbFrame()) {
        return;
    }
    ctx_.wfbFrameCount++;

    static int8_t rssi[4] = { 1, 1, 1, 1 };
    static uint8_t antenna[4] = { 1, 1, 1, 1 };

    const uint32_t channel_id_f = (kLinkId << 8) + kVideoRadioPort;
    auto channel_id_be = htobe32(channel_id_f);
    auto *channel_id_be8 = reinterpret_cast<uint8_t *>(&channel_id_be);

    if (aggregator_ && frame.MatchesChannelID(channel_id_be8)) {
        aggregator_->process_packet(
            packet.Data.data() + sizeof(ieee80211_header), packet.Data.size() - sizeof(ieee80211_header) - 4, 0,
            antenna, rssi);
    }
}

void WFBReceiver::applyPendingControlActions() {
    int ch = 0, w = 0;
    if (ctx_.takeTune(ch, w)) {
        ChannelWidth_t width = (w < 0) ? curWidth_ : static_cast<ChannelWidth_t>(w);
        // The vendored driver throws std::logic_error on a bandwidth it doesn't implement;
        // a bad tune must not fault the whole capture session (the control API validates
        // width too — this is defense-in-depth). On failure, keep the current channel/width.
        try {
            // Register reprogram only — no USB re-init, no blackout on a same-codec hop.
            rtlDevice_->SetMonitorChannel(SelectedChannel {
                .Channel = static_cast<uint8_t>(ch),
                .ChannelOffset = 0,
                .ChannelWidth = width,
            });
            curChannel_ = static_cast<uint8_t>(ch);
            curWidth_ = width;
            ctx_.log("info", "retuned to channel " + std::to_string(ch));
            ctx_.resetDetection(); // re-describe the (possibly different) stream
        } catch (const std::exception &e) {
            ctx_.log("warn", std::string("ignored bad tune (channel ") + std::to_string(ch) + ", width "
                                 + std::to_string(w) + "): " + e.what());
        }
    }

    // Sealed-key reseed (Step 7): drain a staged key and rebuild the Aggregator in
    // place. This runs on the same thread that owns aggregator_, so the swap is
    // race-free. A new session key means a brief wfb resync (rate-limited "Unable to
    // decrypt") before forwarding resumes — same as a fresh session start.
    uint8_t key[64];
    if (ctx_.takeKey(key)) {
        std::array<uint8_t, 64> kp;
        std::memcpy(kp.data(), key, sizeof(key));
        const uint32_t channel_id_f = (kLinkId << 8) + kVideoRadioPort;
        try {
            aggregator_ = std::make_unique<Aggregator>(
                kp, 0 /*epoch*/, channel_id_f,
                [this](uint8_t *payload, uint16_t packet_size) { handleRtp(payload, packet_size); });
            ctx_.log("info", "wfb key reseeded");
            ctx_.resetDetection();
        } catch (const std::exception &e) {
            ctx_.setError(std::string("key reseed failed: ") + e.what());
        }
        sodium_memzero(key, sizeof(key));
        sodium_memzero(kp.data(), kp.size());
    }
}

// Best-effort codec sniff from the first RTP payload's NAL header byte. An H.265 NAL
// header byte0 is (forbidden_zero<<7)|(nal_type<<1)|layerId_hi, so with the usual
// layerId=0 it is always EVEN — an ODD low-5-bits value can therefore ONLY be H.264.
// {1,5,7}=H.264 single-NAL (non-IDR/IDR/SPS); {24,28}=H.264 STAP-A/FU-A. Everything
// else => H.265. Authoritative codec ID arrives with SDP param-set extraction (1b).
inline Codec sniffCodec(const uint8_t *data) {
    int t = data[0] & 0x1F;
    if (t == 1 || t == 5 || t == 7 || t == 24 || t == 28) {
        return Codec::H264;
    }
    return Codec::H265;
}

void WFBReceiver::handleRtp(uint8_t *payload, uint16_t packet_size) {
    // Guard before counting: a null device (pre-init) or a stopping device must not
    // tick the forwarded-packet counter that /health "streaming" is derived from.
    if (!rtlDevice_ || ctx_.shouldStop.load()) {
        return;
    }
    ctx_.rtpPktCount++;
    if (packet_size < 12) {
        return;
    }

    sockaddr_in serverAddr {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(static_cast<u_short>(ctx_.outPort));
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    auto *header = (RtpHeader *)payload;

    // Codec sniff: on the first packet that actually carries a payload. Guarded with
    // plen > 0 so a short or fully-header-consumed packet can't read past the payload
    // (sniffCodec derefs data[0], and getPayloadData() can point at/after the packet
    // end when csrc/ext eat it all) — same guard the param-set path below already uses.
    // Decoupled from the `described` latch so a degenerate first packet can't leave the
    // codec stuck at Auto (which would also disable SDP param-set capture) for the session.
    if (ctx_.codec.load() == Codec::Auto) {
        const ssize_t plen = header->getPayloadSize(packet_size);
        if (plen > 0) {
            Codec detected = sniffCodec(header->getPayloadData());
            ctx_.codec = detected;
            ctx_.log("debug", std::string("detected codec ") + codecName(detected));
        }
    }

    // One-shot stream description (pt/ssrc/log) on the first RTP packet. Reads only the
    // fixed 12-byte header (packet_size >= 12 already checked above), so no payload bound.
    if (!ctx_.described.exchange(true)) {
        ctx_.rtpPayloadType = header->pt;
        ctx_.ssrc = ntohl(header->ssrc);
        ctx_.log("info",
                 "RTP stream pt=" + std::to_string(header->pt) + " ssrc=" + std::to_string(ntohl(header->ssrc))
                     + " codec=" + codecName(ctx_.codec.load()) + " -> 127.0.0.1:"
                     + std::to_string(ctx_.outPort));
    }

    // Capture H.264/H.265 parameter sets for GET /sdp (progressive). Runs until the
    // required set is complete; the atomic short-circuits the cost afterward. Param
    // sets arrive with each IDR/GOP, so this completes within ~1 GOP of a fresh stream.
    if (!ctx_.haveParamSets.load()) {
        const Codec c = ctx_.codec.load();
        const ssize_t plen = header->getPayloadSize(packet_size);
        if (c != Codec::Auto && plen > 0) {
            std::lock_guard<std::mutex> lk(ctx_.sdpMutex);
            if (sdpExtractParamSets(c, header->getPayloadData(), static_cast<size_t>(plen), ctx_.sdpVps,
                                    ctx_.sdpSps, ctx_.sdpPps)
                && sdpParamsComplete(c, ctx_.sdpVps, ctx_.sdpSps, ctx_.sdpPps)) {
                ctx_.haveParamSets = true;
                ctx_.log("info", "captured SDP parameter sets");
            }
        }
    }

    // send video to the downstream consumer
    sendto(ctx_.sendFd, reinterpret_cast<const char *>(payload), packet_size, 0, (sockaddr *)&serverAddr,
           sizeof(serverAddr));
}

bool WFBReceiver::Stop() {
    const auto s = ctx_.state.load();
    if (s == SessionState::Streaming || s == SessionState::Starting) {
        ctx_.state = SessionState::Stopping;
    }
    ctx_.shouldStop = true;
    ctx_.described = false;
    // Stop is signalled solely via the ctx_.shouldStop atomic (read at the RX-loop top).
    // Stop() runs on the supervisor thread and MUST NOT touch rtlDevice_, which the
    // capture thread concurrently creates/owns (reading it here was a data race / UB).
    return true;
}

void WFBReceiver::Join() {
    if (usbThread_.joinable()) {
        usbThread_.join();
    }
}

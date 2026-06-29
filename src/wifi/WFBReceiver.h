//
// Created by Talus on 2024/6/10.
//

#ifndef WFBRECEIVER_H
#define WFBRECEIVER_H
// Winsock must be included before any <windows.h> the driver headers below may
// pull in. Originally this arrived transitively via <QUdpSocket>; the headless
// fork includes it directly since WFBReceiver.cpp uses raw Winsock sockets.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "FrameParser.h"
#include "Rtl8812aDevice.h"
#include <libusb.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct OpenPrlxContext; // owned runtime state (src/Context.h)
class Aggregator;       // wfb-ng FEC/decrypt aggregator (src/wifi/WFBProcessor.h)

class WFBReceiver {
public:
    explicit WFBReceiver(OpenPrlxContext &ctx);
    ~WFBReceiver();

    // Enumerate WinUSB-bound adapters as "vid:pid" strings. No instance state.
    static std::vector<std::string> GetDongleList();

    bool Start(const std::string &vidPid, uint8_t channel, int channelWidth, const std::string &keyPath);
    bool Stop();
    void Join(); // block until the capture thread has fully unwound
    void handle80211Frame(const Packet &pkt);
    void handleRtp(uint8_t *payload, uint16_t packet_size);

private:
    // Drain staged control requests (tune now; key reseed in 1b) at the top of the
    // owned RX loop. Runs on the capture thread — the only thread that touches the device.
    void applyPendingControlActions();

    OpenPrlxContext &ctx_;
    libusb_context *usbCtx_ = nullptr;
    libusb_device_handle *devHandle_ = nullptr;
    std::thread usbThread_;
    std::unique_ptr<Rtl8812aDevice> rtlDevice_;
    std::unique_ptr<Aggregator> aggregator_;
    std::string keyPath_;

    // Current tune, tracked so a tune with width=-1 ("keep current") can reuse it.
    // Only read/written on the capture thread.
    uint8_t curChannel_ = 0;
    ChannelWidth_t curWidth_ = CHANNEL_WIDTH_20;

    // wfb-ng channel identity (link_domain = "default").
    static constexpr uint32_t kLinkId = 7669206; // sha1 hash of "default"
    static constexpr uint8_t kVideoRadioPort = 0;
};

#endif // WFBRECEIVER_H

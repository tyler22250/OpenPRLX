#ifndef RTL8812ADEVICE_H
#define RTL8812ADEVICE_H

#include <functional>
#include <vector>

#include "logger.h"
#include "HalModule.h"
#include "SelectedChannel.h"
#include "EepromManager.h"
#include "RadioManagementModule.h"

using Action_RadioPacket = std::function<void(const Packet&)>;

class Rtl8812aDevice {
  std::shared_ptr<EepromManager> _eepromManager;
  std::shared_ptr<RadioManagementModule> _radioManagement;
  RtlUsbAdapter _device;
  HalModule _halModule;
  Logger_t _logger;
  Action_RadioPacket _packetProcessor = nullptr;

public:
  Rtl8812aDevice(RtlUsbAdapter device, Logger_t logger);
  void Init(Action_RadioPacket packetProcessor, SelectedChannel channel);
  void SetMonitorChannel(SelectedChannel channel);
  bool should_stop = false;

  // Caller-owned RX loop (OpenPRLX): BringUp() does the one-time monitor-mode
  // bring-up + initial tune; ReadOnce() pumps one ~5s USB read window (empty on
  // silence) and reports the libusb return code via usb_rc so the caller can
  // detect an unplugged adapter (LIBUSB_ERROR_NO_DEVICE) vs benign silence. This
  // lets the caller apply live tune/key changes between reads without the blocking
  // Init() loop. Init() is retained for upstream parity.
  void BringUp(SelectedChannel channel);
  std::vector<Packet> ReadOnce(int &usb_rc);

private:
  void StartWithMonitorMode(SelectedChannel selectedChannel);
  bool NetDevOpen(SelectedChannel selectedChannel);
};

#endif /* RTL8812ADEVICE_H */

<!-- SPDX-License-Identifier: GPL-2.0-only -->
# OpenPRLX — project guide for Claude Code

Headless Windows daemon: captures an OpenIPC FPV stream over **wfb-ng** on an **RTL8812AU**,
de-FECs (zfec) + decrypts (libsodium), and forwards raw **RTP → `udp:127.0.0.1:5600`** for a
separate downstream consumer app to decode/render. Exposes a localhost, token-auth HTTP
control/health API. **Non-goals:** no GUI, no decoder, no rendering. Fork of OpenIPC/fpv4win.

Current release: **v0.1.1** — the full capture → decrypt → forward → control pipeline is done and
hardware-validated (RTL8812AU `0bda:8812` + an OpenIPC H.265 air unit); v0.1.1 adds the post-audit
security & robustness hardening pass. See the README for user docs.

## Architecture

```
RTL8812AU (USB, WinUSB/Zadig) → wfb monitor capture → de-FEC + decrypt → raw RTP → udp 127.0.0.1:5600
                                                                         control/health HTTP on 127.0.0.1:9301
```

**Threading model — "server stages, supervisor acts" (do not violate):**
- **Control server thread** (`src/ControlServer.h`): parse → auth → route. For control verbs it ONLY
  writes a mutex-guarded mailbox in `OpenPrlxContext`. It NEVER touches libusb / the device / the aggregator.
- **Main thread = supervisor** (`src/openprlx_main.cpp`): drains start/stop from the mailbox; owns the
  capture lifecycle (`receiver.Start()/Stop()/Join()`); reaps the finished thread before a restart.
- **Capture thread** (`src/wifi/WFBReceiver.cpp`): owns the RX loop + the device + the `Aggregator`.
  At loop-top it drains pending **tune** and **key** actions, then pumps one `ReadOnce()` window.
  Device/aggregator are only ever mutated here.

State machine: `Idle → Starting → Streaming`, `→ Stopping → Idle`, or `→ Error` on a fault. **Fail-fast at
the session, not the process:** a capture fault sets `state=error` + `lastError` and the daemon stays alive.

## Key files

- `src/openprlx_main.cpp` — entry, arg parsing, supervisor loop, `--seal-key` utility.
- `src/Context.h` — `OpenPrlxContext`: owned runtime state (no singletons/statics). Counters, session
  state, the control mailbox (`requestTune/takeTune`, `requestKey/takeKey`), SDP param-set buffers, codec.
- `src/ControlServer.h` — header-only Winsock HTTP server: routes, token auth, `/health` JSON.
- `src/Sdp.{h,cpp}` — RTP parameter-set extraction (H.264 STAP-A / H.265 AP) + SDP builder.
- `src/KeyVault.{h,cpp}` — daemon X25519 keypair; opens libsodium-sealed keys in locked memory.
- `src/wifi/WFBReceiver.{h,cpp}` — libusb bring-up, the owned RX loop, 802.11→wfb→RTP egress, codec sniff.
- `src/wifi/WFBProcessor.{h,cpp}` — the wfb `Aggregator` (FEC + decrypt). **Keep the crypto/FEC math verbatim**
  (we added only a `delete[]` fix + a key-zeroing dtor, marked `// OpenPRLX divergence from upstream:`).
- `src/wifi/{Rtp.h,RxFrame.h,WFBDefine.h}`, `src/wifi/fec.{c,h}` — wire formats + zfec. Verbatim except a
  `RxFrame::PayloadSpan()` short-frame underflow guard (divergence-commented).
- `3rd/rtl8812au-monitor-pcap/` — vendored userspace driver (devourer). **Edit minimally** (we added
  `BringUp`/`ReadOnce(int&usb_rc)` accessors + a `usb_rc` out-param on `infinite_read` + made `logger.h`
  callback-only; and made `FrameParser`'s `Packet` OWN its bytes to fix a use-after-return — all
  `// OpenPRLX divergence from upstream:` commented). GPL-2.0-only — this is why the whole project is GPL-2.0.
- `CMakeLists.txt`, `vcpkg.json` (pinned static deps), `fork_compat.h` (MSVC `/FI` chrono shim),
  `.github/workflows/build-openprlx.yml` (CI).

## Build

VS2022 ("Desktop development with C++") + vcpkg. Deps (libusb, libsodium) are pinned in `vcpkg.json`
and linked **statically** (`x64-windows-static` + static CRT) → `openprlx.exe` is a single
self-contained binary, no DLLs / no VC++ redist. Local recipe:
```sh
vcpkg install libusb:x64-windows-static libsodium:x64-windows-static
cmake -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_MODE=OFF -S . -B build
cmake --build build --config Release --target openprlx
```
`-DVCPKG_MANIFEST_MODE=OFF` uses the classic-installed static deps above (matches CI); `vcpkg.json` pins
the same versions for manifest-mode/CI reproducibility. The VS-bundled cmake works too:
`"C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"`.
A single self-contained `openprlx.exe` lands in `build/Release/` (verify with `dumpbin /dependents` →
system DLLs only). After editing `CMakeLists.txt`, cmake reconfigures automatically.

## Run / hardware test

Adapter must be WinUSB-bound (Zadig). No `ffplay` on the dev box — verify the pipeline via `/health`
counters + binding `udp:5600`. Example:
```sh
openprlx --vidpid 0bda:8812 --channel 149 --width 0 --key gs.key --control-token TOK --health-port 9321
curl -H "Authorization: Bearer TOK" http://127.0.0.1:9321/health   # state, streaming, codec, counters...
```
Control acks are `202`; observe results via `/health`. Live retune: `POST /tune {"channel":N}`. Sealed
key feed: `GET /pubkey` → `openprlx --seal-key <pubkey> --key gs.key` → `POST /key {"sealed":"<b64>"}`.
PowerShell strips embedded quotes from `curl` JSON — send bodies from a file (`curl --data @body.json`).
wfb session-key sync: a fresh start/reseed logs rate-limited "Unable to decrypt" until the next sealed
session packet arrives, then forwards — this is normal, not a bug.

## Conventions / invariants

- **Keep the wfb crypto/FEC core and wire headers verbatim**; touch the vendored `3rd/` driver only via
  thin accessors. All device/libusb access stays on the capture thread.
- **Owned state, no globals/singletons.** New shared state goes in `OpenPrlxContext`, guarded by the
  appropriate mutex; the control server stages via the mailbox, it does not act.
- **Localhost-only + token auth + key-never-on-disk.** The wfb key arrives sealed (`crypto_box_seal`),
  opens only in `sodium_malloc`/`mlock`, transient copies are `secureZero`'d. Don't add disk writes of keys.
- **License:** new first-party files get an `SPDX-License-Identifier: GPL-2.0-only` + `Copyright (C) <year>
  tyler22250` header. Preserve upstream "Created by …" tags and third-party (zfec/driver) notices.
- **Never commit** `gs.key`, `_local/`, `vcpkg/`, `build/` (all gitignored). Line endings: LF (`.gitattributes`).

## Local-only context (not in this repo)

The detailed design rationale, decision log, and full step-by-step history live in `_local/DESIGN_NOTES.md`
(gitignored — present in the original working tree, never pushed) and the approved plan under
`~/.claude/plans/`. If you're in a fresh clone they won't be there; this file is the public-safe summary.

## Deferred / possible next work

wfb link-domain tunable (hardcoded `"default"` = link_id `7669206`, port 0), `RSSI`/`lastIdrMs` in
`/health`, an H.264 RF test (the H.264 SDP path is coded but only H.265 was tested on-air), rename
`--health-port` → `--control-port`, `CONTRIBUTING.md`/`SECURITY.md`, and a tagged GitHub Release with binaries.

<!-- SPDX-License-Identifier: GPL-2.0-only -->
# OpenPRLX

**A lightweight, headless WiFi-broadcast video ingest & routing tool for Windows** — forked from
[FPV4Win](https://github.com/openipc/fpv4win), built on [OpenIPC](https://github.com/OpenIPC).

OpenPRLX captures an OpenIPC FPV stream over [wfb-ng](https://github.com/svpcom/wfb-ng) on an
RTL8812AU adapter, de-FECs and decrypts it, and forwards the raw **RTP** to a local UDP port for a
downstream app to decode and display. No GUI, no decoder, no rendering — just a robust
receive-and-forward daemon with a localhost HTTP control/health API.

> ⚠️ **Early development (v0.1.1).** The receive → decrypt → forward → control pipeline is complete
> and has been validated on real hardware (RTL8812AU + an OpenIPC H.265 air unit). v0.1.1 adds a
> post-audit security & robustness hardening pass. Interfaces may still change.

## What it does

```
 RTL8812AU (USB, WinUSB/Zadig)
        │  wfb-ng monitor capture
        ▼
 de-FEC (zfec) + decrypt (libsodium)         ┌─────────────────────────────┐
        │                                     │  control + health  (HTTP)   │
        ▼                                     │  127.0.0.1:9301             │
 raw RTP  ──►  udp:127.0.0.1:5600   ◄────────►│  /health /start /stop /tune │
        (a downstream app decodes/renders)    │  /key /sdp /pubkey          │
                                              └─────────────────────────────┘
```

## Features

- **Headless daemon** — long-lived process; a capture fault drops the *session* to an error state
  while the process stays alive (fail-fast at the session, not the process).
- **Live control** over a localhost HTTP/JSON API: start/stop a session, **retune** channel/width
  with no restart or video blackout, feed a key, fetch an SDP.
- **Self-describing** — auto-detects H.264/H.265 and serves an `SDP` (`GET /sdp`) with extracted
  parameter sets, so the downstream player needs no manual codec arguments.
- **Sealed key feed** — a wfb key can be delivered encrypted (libsodium `crypto_box_seal`), opened
  only in locked memory, **never written to disk**, and applied as a live reseed.
- **Token-authenticated**, localhost-only control surface.

## Requirements

- Windows 10/11 (x64).
- An RTL8812AU adapter bound to **WinUSB** via [Zadig](https://github.com/pbatard/libwdi/releases).
- An OpenIPC air unit on a known channel, and its wfb **ground-station key** (`gs.key`).

## Build

Visual Studio 2022 ("Desktop development with C++") + [vcpkg](https://github.com/microsoft/vcpkg).
Dependencies (libusb, libsodium) are pinned in [`vcpkg.json`](vcpkg.json) and linked **statically**
(`x64-windows-static` + static CRT), so `openprlx.exe` ships as a single self-contained binary — no
DLLs and no VC++ redistributable required on the target.

```sh
vcpkg install libusb:x64-windows-static libsodium:x64-windows-static

cmake -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_MODE=OFF -S . -B build

cmake --build build --config Release --target openprlx
```

`-DVCPKG_MANIFEST_MODE=OFF` builds against the classic-installed static deps above (this is what CI
does); `vcpkg.json` pins the same versions for manifest-mode / CI reproducibility. A single
self-contained `openprlx.exe` lands in `build/Release/` — verify with `dumpbin /dependents`, which
should list only system DLLs. The RTL8812AU userspace driver is vendored under [`3rd/`](3rd/) — no
submodule fetch required.

## Quick start

```sh
# list WinUSB-bound adapters
openprlx --list-devices

# start a capture session: adapter 0bda:8812, channel 149 (20 MHz), forwarding to udp:127.0.0.1:5600
openprlx --vidpid 0bda:8812 --channel 149 --key gs.key --control-token mytoken
```

With `--vidpid` the session auto-starts; without it the daemon idles and waits for `POST /start`.
Point any RTP consumer at `udp://127.0.0.1:5600` (an SDP is available at `GET /sdp`).

## CLI

| Flag | Default | Meaning |
|---|---|---|
| `--vidpid VID:PID` | — | Adapter to open (e.g. `0bda:8812`). If omitted, daemon idles awaiting `POST /start`. |
| `--channel N` | `161` | WiFi channel (validated `1`–`177`). |
| `--width W` | `0` | Channel width ordinal: `0`=20 MHz, `1`=40 MHz, `2`=80 MHz (validated `0`–`2`). |
| `--key PATH` | `gs.key` | wfb ground-station key file. |
| `--out-port N` | `5600` | RTP egress UDP port on `127.0.0.1`. |
| `--health-port N` | `9301` | Control + health HTTP port on `127.0.0.1`. |
| `--codec AUTO\|H264\|H265` | `AUTO` | Codec hint; `AUTO` is sniffed from the stream. |
| `--control-token TOKEN` | env `OPENPRLX_CONTROL_TOKEN` | Bearer token for the control API. If unset, auth is **disabled** (a warning is logged). |
| `--list-devices` | — | Print detected adapters as a JSON array and exit. |
| `--seal-key PUBKEY` | — | Utility: seal `--key`'s 64-byte key to a base64 X25519 pubkey and print the blob (see below). |

## Control API

Localhost only, `127.0.0.1:<health-port>`. Every route requires the token (when configured) as
`Authorization: Bearer <token>` or `?token=<token>`; missing/wrong → `401`.

| Method · Path | Body | Result |
|---|---|---|
| `GET /health` | — | `200` status JSON: `state`, `streaming`, `captureAlive`, counters, `codec`, `rtpPayloadType`, `ssrc`, `sdpReady`, `paramSets`, `pubkey`, `port` (+`lastError` when `state=error`). |
| `POST /start` | `{vidpid?,channel?,width?,key?}` | `202` (merges into config); `409` if busy (config left **unchanged**); `400` on out-of-range `channel`/`width`. |
| `POST /stop` | — | `202` (idempotent). |
| `POST /tune` | `{channel, width?}` | `202` — live retune, no restart; `409` if not streaming; `400` if `channel` is missing or `channel`/`width` out of range. |
| `POST /key` | `{"sealed":"<base64>"}` | `202` — open sealed key + live reseed; `400` on a bad/missing seal. |
| `GET /sdp` | — | `200 application/sdp` once the codec is detected (progressive: gains the `a=fmtp` line when parameter sets arrive); `503` before detection. |
| `GET /pubkey` | — | `200 {"pubkey":"<base64>"}` — the daemon's X25519 public key for sealing. |

Session `state`: `idle → starting → streaming`, `→ stopping → idle` on stop, or `→ error` on a fault
(process stays alive). Control acks are `202 Accepted`; observe the result via `GET /health`.

## Sealed key feed

To keep the wfb key off disk, the daemon publishes an ephemeral X25519 public key; a client seals the
64-byte key to it with libsodium `crypto_box_seal`. Only the daemon can open it, and it does so only
in locked memory.

```sh
# 1) fetch the daemon's public key
curl -s -H "Authorization: Bearer mytoken" http://127.0.0.1:9301/pubkey

# 2) seal your key to that pubkey (OpenPRLX ships a reference sealer)
openprlx --seal-key <PUBKEY_FROM_STEP_1> --key gs.key      # prints a base64 blob

# 3) feed it
curl -H "Authorization: Bearer mytoken" -X POST \
     --data '{"sealed":"<BLOB_FROM_STEP_2>"}' http://127.0.0.1:9301/key
```

The reseed rebuilds the decryptor in place — no restart. (`--key` remains required at startup; the
sealed feed reseeds a running session.)

## Security model

- Control and RTP egress bind **`127.0.0.1` only** — nothing is exposed off-host.
- The control API is **token-authenticated** (set `--control-token` / `OPENPRLX_CONTROL_TOKEN`).
- The wfb key arrives **sealed** (encrypted to the daemon's key), is opened only in
  `sodium_malloc`/`sodium_mlock` memory, and is **never written to disk**; transient copies are zeroed.

## Acknowledgements

- [OpenIPC](https://github.com/OpenIPC) and [FPV4Win](https://github.com/openipc/fpv4win) — the project this is forked from.
- The [devourer](https://github.com/openipc/devourer) userspace RTL8812AU driver — originally by
  [buldo](https://github.com/buldo), converted to C by [josephnef](https://github.com/josephnef) —
  vendored under [`3rd/`](3rd/).
- [wfb-ng](https://github.com/svpcom/wfb-ng) and [zfec](https://github.com/tahoe-lafs/zfec).

## License

**GPL-2.0-only** — see [LICENSE](LICENSE). OpenPRLX bundles the vendored RTL8812AU driver
([`3rd/rtl8812au-monitor-pcap`](3rd/rtl8812au-monitor-pcap), GPL-2.0-only) and zfec (GPLv2-or-later);
the GPL-2.0-only driver is what fixes the combined work at GPL-2.0.

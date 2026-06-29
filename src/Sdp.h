// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 tyler22250
//
// OpenPRLX — SDP generation from the wfb RTP egress (RFC 6184 H.264 / RFC 7798 H.265).
//
// Parameter sets (SPS/PPS, plus VPS for H.265) are scanned out of the live RTP
// payloads on the capture thread and base64-encoded into an SDP served by GET /sdp,
// so a downstream consumer can decode the forwarded stream with no manual codec args.
//
#ifndef OPENPRLX_SDP_H
#define OPENPRLX_SDP_H

#include "Context.h" // for Codec (Context.h must NOT include this header — avoid the cycle)

#include <cstddef>
#include <cstdint>
#include <string>

// Scan ONE RTP payload (the bytes AFTER the RTP header) for parameter-set NAL units
// and append any newly-found ones (raw NAL bytes, no Annex-B start code) into the
// vps/sps/pps out-strings. Handles single NAL units and aggregation packets
// (H.264 STAP-A / H.265 AP); FU fragments are skipped (param sets are ~never
// fragmented). Returns true if a parameter set was captured (or refreshed) this call.
bool sdpExtractParamSets(Codec codec, const uint8_t *payload, size_t len, std::string &vps, std::string &sps,
                         std::string &pps);

// True once the parameter sets required for `codec` are present
// (H.264: sps+pps; H.265: vps+sps+pps).
bool sdpParamsComplete(Codec codec, const std::string &vps, const std::string &sps, const std::string &pps);

// Build an SDP for 127.0.0.1:port with payload type `payloadType` (the true wire PT).
// The a=fmtp sprop line is emitted only when the required parameter sets are present
// (progressive: rtpmap is always there, fmtp appears once the sets have been seen).
std::string sdpBuild(Codec codec, int payloadType, int port, const std::string &vps, const std::string &sps,
                     const std::string &pps);

#endif // OPENPRLX_SDP_H

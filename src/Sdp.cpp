// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 tyler22250
//
// OpenPRLX — SDP generation (see Sdp.h).
//
#include "Sdp.h"

#include <sodium.h>

#include <cstring>

namespace {

// Standard padded base64 (what SDP sprop fields expect), via the already-linked
// libsodium. sodium_base64_encoded_len() counts the trailing NUL, so trim it off.
std::string b64(const std::string &raw) {
    if (raw.empty()) {
        return "";
    }
    const size_t cap = sodium_base64_encoded_len(raw.size(), sodium_base64_VARIANT_ORIGINAL);
    std::string out(cap, '\0');
    sodium_bin2base64(out.data(), cap, reinterpret_cast<const unsigned char *>(raw.data()), raw.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    out.resize(std::strlen(out.c_str()));
    return out;
}

// Store a raw NAL (with its 1- or 2-byte header, no start code) if it's a parameter
// set for `codec`. Returns true when a set is seen for the first time or changes.
bool storeIfParamSet(Codec codec, const uint8_t *nal, size_t n, std::string &vps, std::string &sps,
                     std::string &pps) {
    if (n == 0) {
        return false;
    }
    if (codec == Codec::H264) {
        const int t = nal[0] & 0x1F;
        const std::string val(reinterpret_cast<const char *>(nal), n);
        if (t == 7 && sps != val) { sps = val; return true; } // SPS
        if (t == 8 && pps != val) { pps = val; return true; } // PPS
    } else if (codec == Codec::H265) {
        if (n < 2) {
            return false;
        }
        const int t = (nal[0] >> 1) & 0x3F;
        const std::string val(reinterpret_cast<const char *>(nal), n);
        if (t == 32 && vps != val) { vps = val; return true; } // VPS
        if (t == 33 && sps != val) { sps = val; return true; } // SPS
        if (t == 34 && pps != val) { pps = val; return true; } // PPS
    }
    return false;
}

// Walk an aggregation packet body: repeated [u16 big-endian size][NAL of that size],
// starting at `off`. Classifies each contained NAL via storeIfParamSet.
bool walkAggregation(Codec codec, const uint8_t *p, size_t len, size_t off, std::string &vps, std::string &sps,
                     std::string &pps) {
    bool captured = false;
    while (off + 2 <= len) {
        const size_t nalSize = (static_cast<size_t>(p[off]) << 8) | p[off + 1];
        off += 2;
        if (nalSize == 0 || off + nalSize > len) {
            break; // truncated/malformed — stop
        }
        captured |= storeIfParamSet(codec, p + off, nalSize, vps, sps, pps);
        off += nalSize;
    }
    return captured;
}

} // namespace

bool sdpExtractParamSets(Codec codec, const uint8_t *payload, size_t len, std::string &vps, std::string &sps,
                         std::string &pps) {
    if (!payload || len == 0) {
        return false;
    }

    if (codec == Codec::H264) {
        const int t = payload[0] & 0x1F;
        if (t == 24) { // STAP-A: [1B hdr] then [u16 size][NAL]...
            return walkAggregation(codec, payload, len, 1, vps, sps, pps);
        }
        if (t >= 1 && t <= 23) { // single NAL unit
            return storeIfParamSet(codec, payload, len, vps, sps, pps);
        }
        return false; // FU-A (28) / other: param sets not carried here
    }

    if (codec == Codec::H265) {
        if (len < 2) {
            return false;
        }
        const int t = (payload[0] >> 1) & 0x3F;
        if (t == 48) { // AP: [2B payload hdr] then [u16 size][NAL]...  (no DONL)
            return walkAggregation(codec, payload, len, 2, vps, sps, pps);
        }
        if (t <= 47) { // single NAL unit
            return storeIfParamSet(codec, payload, len, vps, sps, pps);
        }
        return false; // FU (49) / other
    }

    return false;
}

bool sdpParamsComplete(Codec codec, const std::string &vps, const std::string &sps, const std::string &pps) {
    if (codec == Codec::H264) {
        return !sps.empty() && !pps.empty();
    }
    if (codec == Codec::H265) {
        return !vps.empty() && !sps.empty() && !pps.empty();
    }
    return false;
}

std::string sdpBuild(Codec codec, int payloadType, int port, const std::string &vps, const std::string &sps,
                     const std::string &pps) {
    const char *enc = (codec == Codec::H265) ? "H265" : "H264";
    const int pt = (payloadType >= 0) ? payloadType : 96;
    const std::string p = std::to_string(pt);

    std::string s;
    s += "v=0\r\n";
    s += "o=- 0 0 IN IP4 127.0.0.1\r\n";
    s += "s=OpenPRLX\r\n";
    s += "c=IN IP4 127.0.0.1\r\n";
    s += "t=0 0\r\n";
    s += "m=video " + std::to_string(port) + " RTP/AVP " + p + "\r\n";
    s += "a=rtpmap:" + p + " " + enc + "/90000\r\n";

    if (sdpParamsComplete(codec, vps, sps, pps)) {
        if (codec == Codec::H264) {
            s += "a=fmtp:" + p + " packetization-mode=1; sprop-parameter-sets=" + b64(sps) + "," + b64(pps)
                + "\r\n";
        } else {
            s += "a=fmtp:" + p + " sprop-vps=" + b64(vps) + "; sprop-sps=" + b64(sps) + "; sprop-pps=" + b64(pps)
                + "\r\n";
        }
    }
    return s;
}

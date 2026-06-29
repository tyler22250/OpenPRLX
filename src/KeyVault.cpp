// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 tyler22250
//
// OpenPRLX — daemon key vault (see KeyVault.h).
//
#include "KeyVault.h"

#include <sodium.h>

#include <cstring>
#include <vector>

KeyVault::KeyVault() {
    if (sodium_init() < 0) {
        return; // sk_ stays null -> ok() == false
    }
    sk_ = static_cast<uint8_t *>(sodium_malloc(crypto_box_SECRETKEYBYTES));
    if (sk_ == nullptr) {
        return;
    }
    sodium_mlock(sk_, crypto_box_SECRETKEYBYTES); // sodium_malloc already locks; belt-and-suspenders
    crypto_box_keypair(pk_, sk_);
}

KeyVault::~KeyVault() {
    if (sk_ != nullptr) {
        sodium_munlock(sk_, crypto_box_SECRETKEYBYTES);
        sodium_free(sk_);
        sk_ = nullptr;
    }
}

std::string KeyVault::pubkeyBase64() const {
    const size_t cap = sodium_base64_encoded_len(sizeof(pk_), sodium_base64_VARIANT_ORIGINAL);
    std::string out(cap, '\0');
    sodium_bin2base64(out.data(), cap, pk_, sizeof(pk_), sodium_base64_VARIANT_ORIGINAL);
    out.resize(std::strlen(out.c_str())); // encoded_len includes the trailing NUL
    return out;
}

bool KeyVault::openSealedBase64(const std::string &b64, uint8_t out64[64]) const {
    if (sk_ == nullptr) {
        return false;
    }
    std::vector<uint8_t> sealed(b64.size()); // decoded length <= input length
    size_t sealedLen = 0;
    if (sodium_base642bin(sealed.data(), sealed.size(), b64.c_str(), b64.size(), nullptr, &sealedLen, nullptr,
                          sodium_base64_VARIANT_ORIGINAL)
        != 0) {
        return false;
    }
    if (sealedLen != crypto_box_SEALBYTES + 64) {
        return false; // expect 48-byte seal overhead + the 64-byte wfb key
    }
    return crypto_box_seal_open(out64, sealed.data(), sealedLen, pk_, sk_) == 0;
}

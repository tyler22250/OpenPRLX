// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 tyler22250
//
// OpenPRLX — daemon key vault for the sealed in-memory wfb-key feed.
//
// Holds an ephemeral X25519 keypair created once at startup. The public key is
// published (safe); the secret key lives in a sodium_malloc + sodium_mlock buffer
// and never leaves it. A control client uses crypto_box_seal to encrypt a 64-byte
// wfb key to the daemon's public key; only the daemon (holding the secret) can
// crypto_box_seal_open it. The opened key is used in memory and never written to disk.
//
#ifndef OPENPRLX_KEYVAULT_H
#define OPENPRLX_KEYVAULT_H

#include <cstdint>
#include <string>

class KeyVault {
public:
    KeyVault();
    ~KeyVault();
    KeyVault(const KeyVault &) = delete;
    KeyVault &operator=(const KeyVault &) = delete;

    // True once the keypair is ready (sodium_init + sodium_malloc succeeded).
    bool ok() const { return sk_ != nullptr; }

    // Base64 (standard, padded) of the 32-byte X25519 public key.
    std::string pubkeyBase64() const;

    // Decode a base64 crypto_box_seal blob and open it with the daemon keypair. On
    // success writes exactly 64 bytes to out64 and returns true; false on bad base64,
    // wrong length, or authentication failure. The caller owns/zeros out64.
    bool openSealedBase64(const std::string &b64, uint8_t out64[64]) const;

private:
    uint8_t pk_[32];        // X25519 public key (crypto_box_PUBLICKEYBYTES); plain memory OK
    uint8_t *sk_ = nullptr; // secret key, sodium_malloc + sodium_mlock; freed in dtor
};

#endif // OPENPRLX_KEYVAULT_H

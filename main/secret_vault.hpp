/*
 * Secret Vault
 * Encrypted storage for saved WiFi passwords, in NVS alongside the other settings.
 *
 * A random 256-bit data key encrypts every secret with AES-256-GCM (the secret's id is bound
 * to its ciphertext as additional data, so records can't be swapped). The data key is stored
 * wrapped (AES-256-GCM) under a random per-device secret kept in its own NVS namespace, so the
 * vault unlocks by itself at boot and saved WiFi reconnects with no prompts, while a copy of
 * the "vault" namespace alone decrypts nothing. Plaintext is wiped from RAM after use.
 * Adapted from SimplrSSH's vault (same record format, without its optional PIN).
 */

#ifndef SECRET_VAULT_HPP
#define SECRET_VAULT_HPP

#include "esp_err.h"
#include <string>

namespace vault
{
    // Opens the vault, creating it on first use. Returns true if it had to discard secrets it
    // couldn't decrypt (the device secret was lost).
    bool init();
    bool is_available();

    // Secrets are addressed by id, e.g. "wifi:HomeNet"
    esp_err_t put(const std::string& id, const std::string& secret);
    esp_err_t get(const std::string& id, std::string& secret);
    esp_err_t remove(const std::string& id);
    esp_err_t reset();      // Delete every secret

    // Best-effort overwrite of a plaintext secret before it is released
    void wipe(std::string& s);
}

#endif

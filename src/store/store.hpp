#pragma once

// Persistent state under $HOME/.config/cpz-baby-monitor/ (store guideline).
//  identity.bin : random device id, created on first run
//  pairing.bin  : current pairing (absent when unpaired), mode 0600

#include <optional>
#include <string>

#include "proto/link.hpp"

namespace cpzbm::store {

std::string configDir();  // creates it if missing
Identity loadOrCreateIdentity(const std::string& dir, const char* default_name);
std::optional<Pairing> loadPairing(const std::string& dir);
bool savePairing(const std::string& dir, const Pairing& p);  // p.peer_id==0 -> delete

// Exposed for tests.
std::string serialize(const Pairing& p);
std::optional<Pairing> deserialize(const std::string& data);

}  // namespace cpzbm::store

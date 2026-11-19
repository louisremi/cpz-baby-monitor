#pragma once

// Persistent state under $HOME/.config/cpz-baby-monitor/ (store guideline).
//  identity.bin : random device id, created on first run
//  pairing.bin  : current pairing (absent when unpaired), mode 0600

#include <optional>
#include <string>

#include "proto/link.hpp"
#include "audio/codec_mode.hpp"
#include "radio/radio_config.hpp"

namespace cpzbm::store {

std::string configDir();  // creates it if missing
Identity loadOrCreateIdentity(const std::string& dir, const char* default_name);
std::optional<Pairing> loadPairing(const std::string& dir);
bool savePairing(const std::string& dir, const Pairing& p);  // p.peer_id==0 -> delete

// region.txt: "EU868" or "US915". Missing/invalid -> fallback.
Region loadRegion(const std::string& dir, Region fallback);
bool saveRegion(const std::string& dir, Region r);
std::optional<Region> parseRegion(const std::string& text);

// codec.txt: "1600" or "700C" (baby-side codec2 mode). Missing -> fallback.
audio::Mode loadCodecMode(const std::string& dir, audio::Mode fallback);
bool saveCodecMode(const std::string& dir, audio::Mode m);
std::optional<audio::Mode> parseCodecMode(const std::string& text);

// Exposed for tests.
std::string serialize(const Pairing& p);
std::optional<Pairing> deserialize(const std::string& data);

}  // namespace cpzbm::store

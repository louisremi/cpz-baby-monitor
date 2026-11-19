#pragma once

// Radio link profiles. US915 at 500 kHz bandwidth: FCC 15.247 treats >=500 kHz
// digital modulation as wideband, so there is no dwell-time limit and audio
// can be streamed continuously at up to +22 dBm (the SX1262 maximum).

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace cpzbm {

struct LinkProfile {
    const char* name         = "sf9";
    float frequency_mhz      = 915.0f;
    float bandwidth_khz      = 500.0f;
    uint8_t spreading_factor = 9;
    uint8_t coding_rate      = 5;  // 4/5
    uint8_t sync_word        = 0x12;
    int8_t power_dbm         = 22;
    uint16_t preamble        = 8;
};

inline constexpr LinkProfile kProfileSf9{};
inline constexpr LinkProfile kProfileSf7{"sf7", 915.0f, 500.0f, 7, 5, 0x12, 22, 8};

// LoRa time-on-air (Semtech AN1200.13), explicit header, CRC on.
// Used for planning/tests; the radio thread uses RadioLib's own value.
inline uint32_t loraTimeOnAirUs(const LinkProfile& p, std::size_t payload_len)
{
    const double sf    = p.spreading_factor;
    const double t_sym = std::pow(2.0, sf) / (p.bandwidth_khz * 1000.0) * 1e6;
    const bool ldro    = t_sym > 16000.0;
    const double de    = ldro ? 1.0 : 0.0;
    const double num   = 8.0 * payload_len - 4.0 * sf + 28.0 + 16.0;  // CRC on, explicit header
    const double den   = 4.0 * (sf - 2.0 * de);
    double n_payload   = std::ceil(num / den) * (p.coding_rate);
    if (n_payload < 0) n_payload = 0;
    n_payload += 8.0;
    const double t_preamble = (p.preamble + 4.25) * t_sym;
    return static_cast<uint32_t>(t_preamble + n_payload * t_sym);
}

}  // namespace cpzbm

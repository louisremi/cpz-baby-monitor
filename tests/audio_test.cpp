#include <chrono>
#include <cmath>
#include <cstdio>

#include "audio/codec.hpp"
#include "audio/dsp.hpp"
#include "radio/radio_config.hpp"
#include "test_support.hpp"

using namespace cpzbm;
using namespace cpzbm::audio;

int main()
{
    // Levels.
    std::vector<int16_t> silence(320, 0), loud(320);
    for (int i = 0; i < 320; ++i) loud[i] = static_cast<int16_t>(16000 * std::sin(i * 2 * M_PI * 440 / 8000));
    CHECK(rmsDbfs(silence.data(), silence.size()) <= -99.0f);
    const float l = rmsDbfs(loud.data(), loud.size());
    CHECK(l > -10.0f && l < -5.0f);  // 16000/32768 sine ~ -9.2 dBFS
    CHECK(peak(loud.data(), loud.size()) > 0.48f);

    // VOX: open on loud, hang over N blocks, close.
    VoxGate g(-45.0f, 3);
    CHECK(!g.update(-60));
    CHECK(g.update(-30));
    CHECK(g.update(-60));
    CHECK(g.update(-60));
    CHECK(g.update(-60));
    CHECK(!g.update(-60));

    // AGC: a quiet tone is brought up toward the target without clipping.
    {
        Agc agc;
        std::vector<int16_t> q(320);
        float last = 0;
        for (int b = 0; b < 200; ++b) {
            for (int i = 0; i < 320; ++i) q[i] = static_cast<int16_t>(100 * std::sin(i * 2 * M_PI * 440 / 8000));
            agc.process(q.data(), q.size());
            last = peak(q.data(), q.size());
        }
        CHECK(last > 0.2f && last <= 0.6f);
        // Sudden loud input is pulled down within a few blocks.
        for (int b = 0; b < 6; ++b) {
            for (int i = 0; i < 320; ++i) q[i] = static_cast<int16_t>(12000 * std::sin(i * 2 * M_PI * 440 / 8000));
            agc.process(q.data(), q.size());
        }
        CHECK(peak(q.data(), q.size()) <= 0.6f);
    }

    // Jitter buffer: reorder, duplicate, gap.
    JitterBuffer jb(2);
    std::vector<int16_t> out;
    bool gap = false;
    CHECK(jb.push(10, {10}));
    CHECK(!jb.pop(&out, &gap));  // below depth
    CHECK(jb.push(12, {12}));
    CHECK(!jb.push(12, {12}));  // dup
    CHECK(jb.pop(&out, &gap) && !gap && out[0] == 10);
    CHECK(jb.push(13, {13}));
    CHECK(jb.pop(&out, &gap) && gap);  // 11 lost
    CHECK(jb.pop(&out, &gap) && !gap && out[0] == 12);
    CHECK(!jb.push(11, {11}));  // too late
    CHECK(jb.pop(&out, &gap, true) && out[0] == 13);
    // Sequence wraparound.
    JitterBuffer jw(1);
    CHECK(jw.push(65535, {1}));
    CHECK(jw.pop(&out, &gap) && out[0] == 1);
    CHECK(jw.push(0, {2}));
    CHECK(jw.pop(&out, &gap) && !gap && out[0] == 2);

    // Codec2 round trip.
    for (Mode m : {Mode::M1600, Mode::M700C}) {
        Codec c(m);
        CHECK(c.ok());
        CHECK(c.samplesPerFrame() == 320);
        std::printf("codec mode %d: %d samples, %d bytes/frame\n", static_cast<int>(m), c.samplesPerFrame(),
                    c.bytesPerFrame());
        std::vector<uint8_t> bits;
        std::vector<int16_t> pcm(320), dec(320);
        double in_energy = 0, out_energy = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < 50; ++f) {
            for (int i = 0; i < 320; ++i) {
                // Voiced-ish signal: 200 Hz pulse train through a crude formant.
                const int n = f * 320 + i;
                pcm[i] = static_cast<int16_t>(8000 * std::sin(n * 2 * M_PI * 200 / 8000) +
                                              4000 * std::sin(n * 2 * M_PI * 700 / 8000));
            }
            bits.clear();
            c.encode(pcm.data(), &bits);
            CHECK(static_cast<int>(bits.size()) == c.bytesPerFrame());
            c.decode(bits.data(), dec.data());
            if (f >= 5) {
                for (int i = 0; i < 320; ++i) {
                    in_energy += pcm[i] * static_cast<double>(pcm[i]);
                    out_energy += dec[i] * static_cast<double>(dec[i]);
                }
            }
        }
        const double us =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 50.0;
        const double ratio = out_energy / in_energy;
        std::printf("  energy ratio %.2f, %.0f us per frame enc+dec\n", ratio, us);
        CHECK(ratio > 0.02 && ratio < 10.0);
    }
    CHECK(Codec(Mode::M1600).bytesPerFrame() == 8);

    // Air budget for the default profile: 8 frames of 1600 = 87 byte packet.
    const std::size_t pkt = 7 + 4 + 8 * 8 + 16;
    const uint32_t toa    = loraTimeOnAirUs(kProfileSf9, pkt);
    std::printf("SF9/BW500 ToA for %zu bytes: %u us (budget 320000 us)\n", pkt, toa);
    CHECK(toa < 140000);
    CHECK(loraTimeOnAirUs(kProfileSf7, pkt) < 45000);
    const uint32_t toa_eu = loraTimeOnAirUs(kProfileEu, pkt);
    std::printf("EU868 SF7/BW250 ToA for %zu bytes: %u us\n", pkt, toa_eu);
    CHECK(toa_eu < 90000);  // ~25% of a 320 ms packet interval
    CHECK(kProfileEu.frequency_mhz - kProfileEu.bandwidth_khz / 2000.0f >= 869.4f);
    CHECK(kProfileEu.frequency_mhz + kProfileEu.bandwidth_khz / 2000.0f <= 869.65f);

    std::puts("audio_test OK");
    return 0;
}

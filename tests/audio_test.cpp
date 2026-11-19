#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "audio/codec.hpp"
#include "audio/dsp.hpp"
#include "audio/tone.hpp"
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

    // Packetizer: pre-roll is sent when the gate opens at ANY frame of a
    // packet (regression: it used to be lost 7 times out of 8).
    for (int open_at = 0; open_at < 8; ++open_at) {
        Packetizer pk(8, 1);
        std::vector<std::vector<uint8_t>> sent;
        uint8_t n = 0;
        for (int f = 0; f < 16; ++f) {  // two closed packets -> pre-roll = frames 8..15
            const uint8_t b = n++;
            pk.push(&b, false, &sent);
        }
        CHECK(sent.empty());
        for (int f = 0; f < 8; ++f) {
            const uint8_t b = n++;
            pk.push(&b, f >= open_at, &sent);
        }
        CHECK(sent.size() == 2);                        // pre-roll + the packet the sound started in
        CHECK(sent[0].size() == 8 && sent[0][0] == 8);  // the previous 320 ms
        CHECK(sent[1].size() == 8 && sent[1][0] == 16);
    }
    {
        // Gate stays open: one packet per 8 frames, no duplicate pre-roll.
        Packetizer pk(8, 1);
        std::vector<std::vector<uint8_t>> sent;
        for (int f = 0; f < 32; ++f) {
            const uint8_t b = static_cast<uint8_t>(f);
            pk.push(&b, true, &sent);
        }
        CHECK(sent.size() == 4);
    }

    // Jitter buffer: reorder, duplicate, gap.
    JitterBuffer jb(2);
    std::vector<int16_t> out;
    bool gap = false;
    CHECK(jb.push(1, 10, {10}));
    CHECK(!jb.pop(&out, &gap));  // below depth
    CHECK(jb.push(1, 12, {12}));
    CHECK(!jb.push(1, 12, {12}));  // dup
    CHECK(jb.pop(&out, &gap) && !gap && out[0] == 10);
    CHECK(jb.push(1, 13, {13}));
    CHECK(jb.pop(&out, &gap) && gap);  // 11 lost
    CHECK(jb.pop(&out, &gap) && !gap && out[0] == 12);
    CHECK(!jb.push(1, 11, {11}));  // too late
    CHECK(jb.pop(&out, &gap, true) && out[0] == 13);
    // Sequence wraparound.
    JitterBuffer jw(1);
    CHECK(jw.push(1, 65535, {1}));
    CHECK(jw.pop(&out, &gap) && out[0] == 1);
    CHECK(jw.push(1, 0, {2}));
    CHECK(jw.pop(&out, &gap) && !gap && out[0] == 2);

    // Sender restart: new stream id, seq back to 0 -> accepted immediately.
    {
        JitterBuffer jr(1);
        for (uint16_t q = 500; q < 505; ++q) {
            CHECK(jr.push(1, q, {static_cast<int16_t>(q)}));
            CHECK(jr.pop(&out, &gap));
        }
        CHECK(!jr.push(1, 0, {0}));  // same stream: stale
        CHECK(jr.push(2, 0, {42}));  // new stream: resync
        CHECK(jr.pop(&out, &gap) && !gap && out[0] == 42);
        CHECK(jr.push(2, 1, {43}));
        CHECK(jr.pop(&out, &gap) && !gap && out[0] == 43);
    }

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
    CHECK(Codec(Mode::M700C).bytesPerFrame() == 4);
    CHECK(otherMode(Mode::M1600) == Mode::M700C && otherMode(Mode::M700C) == Mode::M1600);
    CHECK(modeFromByte(static_cast<uint8_t>(Mode::M700C)) == Mode::M700C && !modeFromByte(0));

    // Air budget for the default profile: 8 frames of 1600 = 87 byte packet.
    const std::size_t pkt = 7 + 5 + 8 * 8 + 16;  // sec header + audio header + 8 frames + tag
    const uint32_t toa    = loraTimeOnAirUs(kProfileSf9, pkt);
    std::printf("SF9/BW500 ToA for %zu bytes: %u us (budget 320000 us)\n", pkt, toa);
    CHECK(toa < 140000);
    // 700C range fallback: half the codec payload, so less airtime per packet
    // (a less busy channel, more room for retries) on both bands.
    const std::size_t pkt700 = 7 + 5 + 8 * 4 + 16;
    for (const auto* prof : {&kProfileSf9, &kProfileEu}) {
        const uint32_t t1600 = loraTimeOnAirUs(*prof, pkt), t700 = loraTimeOnAirUs(*prof, pkt700);
        std::printf("%s: 1600 %u us, 700C %u us per 320 ms packet\n", prof->name, t1600, t700);
        CHECK(t700 < t1600 * 3 / 4);
    }
    const uint32_t toa_eu = loraTimeOnAirUs(kProfileEu, pkt);
    std::printf("EU868 SF7/BW250 ToA for %zu bytes: %u us\n", pkt, toa_eu);
    CHECK(toa_eu < 90000);  // ~25% of a 320 ms packet interval
    CHECK(kProfileEu.frequency_mhz - kProfileEu.bandwidth_khz / 2000.0f >= 869.4f);
    CHECK(kProfileEu.frequency_mhz + kProfileEu.bandwidth_khz / 2000.0f <= 869.65f);

    // Volume-key confirmation tone: 25 ms, ramped to zero at both ends, never
    // louder than requested, and the three directions are distinguishable.
    {
        std::vector<int16_t> tone;
        renderTone(tone, 990, 25, kSampleRate, 0.35f);
        CHECK(tone.size() == static_cast<std::size_t>(kSampleRate * 25 / 1000));
        CHECK(tone.front() == 0 && tone.back() == 0);
        int loudest = 0;
        for (const auto sample : tone) loudest = std::max(loudest, std::abs(static_cast<int>(sample)));
        const int ceiling = static_cast<int>(0.35f * 32767);
        CHECK(loudest <= ceiling + 1);
        CHECK(loudest > ceiling * 9 / 10);
        std::vector<int16_t> down, mute;
        renderTone(down, 660, 25, kSampleRate, 0.35f);
        renderTone(mute, 440, 25, kSampleRate, 0.35f);
        CHECK(down.size() == tone.size() && mute.size() == tone.size());
        CHECK(down != tone && mute != tone);
        // A zero-length request is harmless.
        std::vector<int16_t> empty;
        renderTone(empty, 990, 0, kSampleRate, 0.35f);
        CHECK(empty.empty());
    }

    std::puts("audio_test OK");
    return 0;
}

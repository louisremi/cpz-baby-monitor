#include "proto/wire.hpp"
#include "test_support.hpp"

using namespace cpzbm::wire;

int main()
{
    Beacon b{0x11223344, {1, 2, 3, 4, 5, 6, 7, 8}, makeName("cardputerzero0")};
    auto pkt = encode(b);
    CHECK(pkt.size() == 1 + 4 + 8 + kNameLen);
    CHECK(peekType(pkt) == Type::Beacon);
    auto b2 = decodeBeacon(pkt);
    CHECK(b2 && b2->dev_id == b.dev_id && b2->session == b.session && b2->name == b.name);

    // Truncated / extended / wrong-type / wrong-version packets are rejected.
    auto shortpkt = pkt;
    shortpkt.pop_back();
    CHECK(!decodeBeacon(shortpkt));
    auto longpkt = pkt;
    longpkt.push_back(0);
    CHECK(!decodeBeacon(longpkt));
    CHECK(!decodePairReq(pkt));
    auto badver = pkt;
    badver[0]   = static_cast<uint8_t>((2 << 4) | 1);
    CHECK(!peekType(badver));
    CHECK(!peekType(Bytes{}));
    CHECK(!peekType(Bytes{static_cast<uint8_t>((kVersion << 4) | 7)}));

    PairReq rq{1, 2, {9, 9, 9, 9, 9, 9, 9, 9}, {}, makeName("b")};
    rq.y[0]  = 0xAA;
    auto rq2 = decodePairReq(encode(rq));
    CHECK(rq2 && rq2->to == 1 && rq2->from == 2 && rq2->y[0] == 0xAA && rq2->name == rq.name);

    PairResp rs{3, 4, {}, {}, {}};
    rs.tag[15] = 7;
    auto rs2   = decodePairResp(encode(rs));
    CHECK(rs2 && rs2->tag[15] == 7 && rs2->from == 4);

    PairConfirm pc{5, 6, {}};
    CHECK(decodePairConfirm(encode(pc)) && decodePairConfirm(encode(pc))->to == 5);
    CHECK(decodePairNack(encode(PairNack{7, 8}))->from == 8);

    SecHeader h{Type::Audio, 0xBEEF, 0x01020304};
    auto hb = encodeSecHeader(h);
    CHECK(hb.size() == kSecHdrLen);
    hb.resize(kSecHdrLen + kAeadTagLen);
    auto h2 = decodeSecHeader(hb);
    CHECK(h2 && h2->type == Type::Audio && h2->pair_id == 0xBEEF && h2->counter == 0x01020304);
    hb.resize(kSecHdrLen + kAeadTagLen - 1);
    CHECK(!decodeSecHeader(hb));

    Audio a{513, 2, 8, Bytes(64, 0x5A)};
    auto a2 = decodeAudio(encode(a));
    CHECK(a2 && a2->seq == 513 && a2->mode == 2 && a2->nframes == 8 && a2->frames.size() == 64);
    Audio bad{1, 2, 3, Bytes(64, 0)};  // 64 % 3 != 0
    CHECK(!decodeAudio(encode(bad)));

    Heartbeat hbm{-42, 80, 1};
    auto hbm2 = decodeHeartbeat(encode(hbm));
    CHECK(hbm2 && hbm2->level_dbfs == -42 && hbm2->battery == 80 && hbm2->flags == 1);

    // Largest audio packet fits a LoRa frame.
    CHECK(kSecHdrLen + 4 + 8 * 8 + kAeadTagLen <= kMaxPacket);
    std::puts("wire_test OK");
    return 0;
}

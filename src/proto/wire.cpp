#include "proto/wire.hpp"

#include <cstring>

namespace cpzbm::wire {
namespace {

uint8_t byte0(Type t) { return static_cast<uint8_t>((kVersion << 4) | static_cast<uint8_t>(t)); }

class Writer {
public:
    explicit Writer(Type t) { out_.push_back(byte0(t)); }
    Writer& u8(uint8_t v)
    {
        out_.push_back(v);
        return *this;
    }
    Writer& u16(uint16_t v)
    {
        out_.push_back(static_cast<uint8_t>(v));
        out_.push_back(static_cast<uint8_t>(v >> 8));
        return *this;
    }
    Writer& u32(uint32_t v)
    {
        for (int i = 0; i < 4; ++i) out_.push_back(static_cast<uint8_t>(v >> (8 * i)));
        return *this;
    }
    template <typename A>
    Writer& arr(const A& a)
    {
        const auto* p = reinterpret_cast<const uint8_t*>(a.data());
        out_.insert(out_.end(), p, p + a.size());
        return *this;
    }
    Bytes take() { return std::move(out_); }

private:
    Bytes out_;
};

class Reader {
public:
    Reader(const Bytes& b, std::size_t pos) : b_(b), pos_(pos) {}
    bool u8(uint8_t* v)
    {
        if (!need(1)) return false;
        *v = b_[pos_++];
        return true;
    }
    bool u16(uint16_t* v)
    {
        if (!need(2)) return false;
        *v = static_cast<uint16_t>(b_[pos_] | (b_[pos_ + 1] << 8));
        pos_ += 2;
        return true;
    }
    bool u32(uint32_t* v)
    {
        if (!need(4)) return false;
        *v = 0;
        for (int i = 0; i < 4; ++i) *v |= static_cast<uint32_t>(b_[pos_ + i]) << (8 * i);
        pos_ += 4;
        return true;
    }
    template <typename A>
    bool arr(A* a)
    {
        if (!need(a->size())) return false;
        std::memcpy(a->data(), &b_[pos_], a->size());
        pos_ += a->size();
        return true;
    }
    bool rest(Bytes* out)
    {
        out->assign(b_.begin() + static_cast<long>(pos_), b_.end());
        pos_ = b_.size();
        return true;
    }
    bool done() const { return pos_ == b_.size(); }

private:
    bool need(std::size_t n) const { return pos_ + n <= b_.size(); }
    const Bytes& b_;
    std::size_t pos_;
};

bool checkHeader(const Bytes& pkt, Type t) { return !pkt.empty() && pkt.size() <= kMaxPacket && pkt[0] == byte0(t); }

}  // namespace

std::optional<Type> peekType(const Bytes& pkt)
{
    if (pkt.empty() || pkt.size() > kMaxPacket) return std::nullopt;
    if ((pkt[0] >> 4) != kVersion) return std::nullopt;
    const uint8_t t = pkt[0] & 0x0F;
    if ((t >= 1 && t <= 5) || (t >= 8 && t <= 15)) return static_cast<Type>(t);
    return std::nullopt;
}

Bytes encode(const Beacon& m) { return Writer(Type::Beacon).u32(m.dev_id).arr(m.session).arr(m.name).take(); }
Bytes encode(const PairReq& m) { return Writer(Type::PairReq).u32(m.to).u32(m.from).arr(m.nonce).arr(m.y).arr(m.name).take(); }
Bytes encode(const PairResp& m)
{
    return Writer(Type::PairResp).u32(m.to).u32(m.from).arr(m.nonce).arr(m.y).arr(m.tag).take();
}
Bytes encode(const PairConfirm& m) { return Writer(Type::PairConfirm).u32(m.to).u32(m.from).arr(m.tag).take(); }
Bytes encode(const PairNack& m) { return Writer(Type::PairNack).u32(m.to).u32(m.from).take(); }

std::optional<Beacon> decodeBeacon(const Bytes& pkt)
{
    if (!checkHeader(pkt, Type::Beacon)) return std::nullopt;
    Reader r(pkt, 1);
    Beacon m;
    if (!r.u32(&m.dev_id) || !r.arr(&m.session) || !r.arr(&m.name) || !r.done()) return std::nullopt;
    return m;
}

std::optional<PairReq> decodePairReq(const Bytes& pkt)
{
    if (!checkHeader(pkt, Type::PairReq)) return std::nullopt;
    Reader r(pkt, 1);
    PairReq m;
    if (!r.u32(&m.to) || !r.u32(&m.from) || !r.arr(&m.nonce) || !r.arr(&m.y) || !r.arr(&m.name) || !r.done())
        return std::nullopt;
    return m;
}

std::optional<PairResp> decodePairResp(const Bytes& pkt)
{
    if (!checkHeader(pkt, Type::PairResp)) return std::nullopt;
    Reader r(pkt, 1);
    PairResp m;
    if (!r.u32(&m.to) || !r.u32(&m.from) || !r.arr(&m.nonce) || !r.arr(&m.y) || !r.arr(&m.tag) || !r.done())
        return std::nullopt;
    return m;
}

std::optional<PairConfirm> decodePairConfirm(const Bytes& pkt)
{
    if (!checkHeader(pkt, Type::PairConfirm)) return std::nullopt;
    Reader r(pkt, 1);
    PairConfirm m;
    if (!r.u32(&m.to) || !r.u32(&m.from) || !r.arr(&m.tag) || !r.done()) return std::nullopt;
    return m;
}

std::optional<PairNack> decodePairNack(const Bytes& pkt)
{
    if (!checkHeader(pkt, Type::PairNack)) return std::nullopt;
    Reader r(pkt, 1);
    PairNack m;
    if (!r.u32(&m.to) || !r.u32(&m.from) || !r.done()) return std::nullopt;
    return m;
}

Bytes encodeSecHeader(const SecHeader& h) { return Writer(h.type).u16(h.pair_id).u32(h.counter).take(); }

std::optional<SecHeader> decodeSecHeader(const Bytes& pkt)
{
    const auto t = peekType(pkt);
    if (!t || !isEncrypted(*t) || pkt.size() < kSecHdrLen + kAeadTagLen) return std::nullopt;
    Reader r(pkt, 1);
    SecHeader h;
    h.type = *t;
    if (!r.u16(&h.pair_id) || !r.u32(&h.counter)) return std::nullopt;
    return h;
}

Bytes encode(const Audio& m)
{
    Bytes out;
    out.reserve(4 + m.frames.size());
    out.push_back(static_cast<uint8_t>(m.seq));
    out.push_back(static_cast<uint8_t>(m.seq >> 8));
    out.push_back(m.mode);
    out.push_back(m.nframes);
    out.insert(out.end(), m.frames.begin(), m.frames.end());
    return out;
}

std::optional<Audio> decodeAudio(const Bytes& plain)
{
    if (plain.size() < 4) return std::nullopt;
    Audio m;
    m.seq     = static_cast<uint16_t>(plain[0] | (plain[1] << 8));
    m.mode    = plain[2];
    m.nframes = plain[3];
    m.frames.assign(plain.begin() + 4, plain.end());
    if (m.nframes == 0 || m.frames.empty() || m.frames.size() % m.nframes != 0) return std::nullopt;
    return m;
}

Bytes encode(const Heartbeat& m)
{
    return {static_cast<uint8_t>(m.level_dbfs), m.battery, m.flags};
}

std::optional<Heartbeat> decodeHeartbeat(const Bytes& plain)
{
    if (plain.size() != 3) return std::nullopt;
    Heartbeat m;
    m.level_dbfs = static_cast<int8_t>(plain[0]);
    m.battery    = plain[1];
    m.flags      = plain[2];
    return m;
}

Name makeName(const char* text)
{
    Name n{};
    if (!text) return n;
    for (std::size_t i = 0; i < n.size() && text[i]; ++i) n[i] = text[i];
    return n;
}

}  // namespace cpzbm::wire

#include "store/store.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "proto/crypto.hpp"

namespace cpzbm::store {
namespace {

constexpr char kMagic[4] = {'C', 'Z', 'B', '1'};

void mkdirs(const std::string& path)
{
    std::string cur;
    for (std::size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' && cur.size() > 1) ::mkdir(cur.c_str(), 0700);
    }
    ::mkdir(path.c_str(), 0700);
}

bool readFile(const std::string& path, std::string* out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

// Atomic replace: write tmp, fsync, rename. A power cut never leaves a
// truncated pairing file (which would silently unpair the device).
bool writeFileAtomic(const std::string& path, const std::string& data)
{
    const std::string tmp = path + ".tmp";
    const int fd          = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
    return ::rename(tmp.c_str(), path.c_str()) == 0;
}

void putU32(std::string* s, uint32_t v)
{
    for (int i = 0; i < 4; ++i) s->push_back(static_cast<char>(v >> (8 * i)));
}

uint32_t getU32(const std::string& s, std::size_t at)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(s[at + i])) << (8 * i);
    return v;
}

}  // namespace

std::string configDir()
{
    const char* xdg  = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::string dir  = xdg && xdg[0] ? std::string(xdg) : std::string(home && home[0] ? home : "/tmp") + "/.config";
    dir += "/cpz-baby-monitor";
    mkdirs(dir);
    return dir;
}

Identity loadOrCreateIdentity(const std::string& dir, const char* default_name)
{
    Identity id;
    std::string data;
    const std::string path = dir + "/identity.bin";
    if (readFile(path, &data) && data.size() == 4 + 4 + wire::kNameLen && data.compare(0, 4, kMagic, 4) == 0) {
        id.dev_id = getU32(data, 4);
        std::memcpy(id.name.data(), data.data() + 8, wire::kNameLen);
        if (id.dev_id != 0) return id;
    }
    do id.dev_id = crypto::randomU32();
    while (id.dev_id == 0);
    id.name = wire::makeName(default_name);
    data.assign(kMagic, 4);
    putU32(&data, id.dev_id);
    data.append(id.name.data(), wire::kNameLen);
    writeFileAtomic(path, data);
    return id;
}

std::string serialize(const Pairing& p)
{
    std::string s(kMagic, 4);
    s.append(reinterpret_cast<const char*>(p.key.data()), p.key.size());
    s.push_back(static_cast<char>(p.pair_id));
    s.push_back(static_cast<char>(p.pair_id >> 8));
    putU32(&s, p.peer_id);
    s.append(p.peer_name.data(), p.peer_name.size());
    s.push_back(static_cast<char>(p.role));
    putU32(&s, p.tx_counter);
    putU32(&s, p.rx_counter);
    return s;
}

std::optional<Pairing> deserialize(const std::string& s)
{
    constexpr std::size_t kLen = 4 + 32 + 2 + 4 + wire::kNameLen + 1 + 4 + 4;
    if (s.size() != kLen || s.compare(0, 4, kMagic, 4) != 0) return std::nullopt;
    Pairing p;
    std::size_t at = 4;
    std::memcpy(p.key.data(), s.data() + at, 32);
    at += 32;
    p.pair_id = static_cast<uint16_t>(static_cast<uint8_t>(s[at]) | (static_cast<uint8_t>(s[at + 1]) << 8));
    at += 2;
    p.peer_id = getU32(s, at);
    at += 4;
    std::memcpy(p.peer_name.data(), s.data() + at, wire::kNameLen);
    at += wire::kNameLen;
    const auto role = static_cast<uint8_t>(s[at++]);
    if (role > 2) return std::nullopt;
    p.role       = static_cast<Role>(role);
    p.tx_counter = getU32(s, at);
    at += 4;
    p.rx_counter = getU32(s, at);
    if (p.peer_id == 0 || p.pair_id != crypto::pairIdFromKey(p.key)) return std::nullopt;
    return p;
}

std::optional<Region> parseRegion(const std::string& text)
{
    std::string t;
    for (char c : text)
        if (c != '\n' && c != '\r' && c != ' ') t += c;
    if (t == "EU868") return Region::EU868;
    if (t == "US915") return Region::US915;
    return std::nullopt;
}

Region loadRegion(const std::string& dir, Region fallback)
{
    std::string data;
    if (!readFile(dir + "/region.txt", &data)) return fallback;
    return parseRegion(data).value_or(fallback);
}

bool saveRegion(const std::string& dir, Region r)
{
    return writeFileAtomic(dir + "/region.txt", std::string(regionName(r)) + "\n");
}

std::optional<Pairing> loadPairing(const std::string& dir)
{
    std::string data;
    if (!readFile(dir + "/pairing.bin", &data)) return std::nullopt;
    return deserialize(data);
}

bool savePairing(const std::string& dir, const Pairing& p)
{
    const std::string path = dir + "/pairing.bin";
    if (p.peer_id == 0) return ::unlink(path.c_str()) == 0 || errno == ENOENT;
    return writeFileAtomic(path, serialize(p));
}

}  // namespace cpzbm::store

// platform/nativeee (host side) -- naser949's two-player mod, ONLINE: te_netplay.h. One TCP connection between the two
// machines, everything on the game thread at the pads' boundary (no thread of its own): the lobby's rows on Options > Mods,
// the state sent from the host to the joiner, the pads exchanged every boundary, the memory's hash compared.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
#elif !defined(__vita__)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "te_netplay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "savestate.h"
#include "te_host.h"
#include "te_input.h"
#include "te_mods.h"
#include "te_services.h"
#include "te_texts.h"

namespace te::netplay {

#if defined(__vita__)

void Install(const std::string&) {}
bool Lockstep() { return false; }
int LocalPad() { return -1; }
Pads::Sample Pad0(const Pads::Sample& local) { return local; }
bool Pad1(Pads::Sample&) { return false; }
void Saved(const std::string&, bool, const std::string&) {}
void Loaded(const std::string&, bool, const std::string&) {}

#else

namespace {

// ---- sockets ------------------------------------------------------------------------------------------------------------
#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
bool WouldBlock() { const int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY; }
void NonBlocking(Socket s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
bool StartSockets() {
    static int started = -1;
    if (started < 0) { WSADATA data; started = WSAStartup(MAKEWORD(2, 2), &data) == 0 ? 1 : 0; }
    return started == 1;
}
using SockLen = int;
constexpr int kSendFlags = 0;
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void CloseSocket(Socket s) { close(s); }
bool WouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EINTR; }
void NonBlocking(Socket s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
bool StartSockets() { return true; }
using SockLen = socklen_t;
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
#endif

void NoDelay(Socket s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

// Waits until the socket can be read (or written) for at most `ms`
bool WaitSocket(Socket s, bool write, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
#if defined(_WIN32)
    const int n = select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
#else
    const int n = select(s + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
#endif
    return n > 0;
}

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// ---- the lobby's code: the host's IPv4 address as seven letters and digits (none easily mistaken for another) -----------
constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

std::string CodeOf(uint32_t address) {   // host byte order
    std::string code;
    uint64_t v = address;
    for (int i = 0; i < 7; i++) {
        code.insert(code.begin(), kAlphabet[v & 31]);
        v >>= 5;
    }
    code.insert(4, "-");
    return code;
}

// A code ("ABCD-EFG", any case, spaces and dashes ignored) or a dotted address ("26.1.2.3"); false: neither
bool AddressOf(const std::string& text, uint32_t& address) {
    std::string t;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '-') t.push_back(c);
    if (t.empty() || t.size() > 32) return false;
    if (t.find('.') != std::string::npos) {
        unsigned a = 0, b = 0, c = 0, d = 0;
        char tail = 0;
        if (std::sscanf(t.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
        address = (a << 24) | (b << 16) | (c << 8) | d;
        return true;
    }
    if (t.size() != 7) return false;
    uint64_t v = 0;
    for (char c : t) {
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (c == '0') c = 'O';
        if (c == '1') c = 'I';
        if (c == 'O') c = 'Q';   // (O and I are not in the alphabet: read as the letters they are mistaken for)
        if (c == 'I') c = 'J';
        const char* at = std::strchr(kAlphabet, c);
        if (!at || !*at) return false;
        v = (v << 5) | uint64_t(at - kAlphabet);
    }
    if (v > 0xFFFFFFFFull) return false;
    address = uint32_t(v);
    return true;
}

std::string Dotted(uint32_t a) {
    char text[32];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    return text;
}

// This machine's IPv4 addresses, the likeliest to be the one the friend reaches first: Radmin VPN's (26.x), Hamachi's (25.x),
// then the private ranges (ZeroTier's, a LAN's), then whatever else
std::vector<uint32_t> LocalAddresses() {
    std::vector<uint32_t> found;
    char name[256] = {};
    if (gethostname(name, sizeof name - 1) != 0) return found;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &list) != 0 || !list) return found;
    for (addrinfo* a = list; a; a = a->ai_next) {
        if (!a->ai_addr || a->ai_family != AF_INET) continue;
        const uint32_t address = ntohl(reinterpret_cast<const sockaddr_in*>(a->ai_addr)->sin_addr.s_addr);
        if ((address >> 24) == 127 || address == 0) continue;
        if (std::find(found.begin(), found.end(), address) == found.end()) found.push_back(address);
    }
    freeaddrinfo(list);
    auto rank = [](uint32_t a) {
        const unsigned top = a >> 24, next = (a >> 16) & 255;
        if (top == 26) return 0;
        if (top == 25) return 1;
        if (top == 10 || (top == 172 && next >= 16 && next < 32)) return 2;
        if (top == 192 && next == 168) return 3;
        if (top == 169 && next == 254) return 9;
        return 5;
    };
    std::stable_sort(found.begin(), found.end(), [&](uint32_t x, uint32_t y) { return rank(x) < rank(y); });
    return found;
}

// ---- the state's bytes: LZ4's block format (written here: the build has no compression library) ------------------------
void Pack(const uint8_t* s, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(n / 3 + 64);
    auto lengths = [&](size_t v) {
        while (v >= 255) { out.push_back(255); v -= 255; }
        out.push_back(uint8_t(v));
    };
    auto sequence = [&](size_t from, size_t literals, size_t match, size_t offset) {
        const size_t m = match ? match - 4 : 0;
        out.push_back(uint8_t((literals >= 15 ? 15 : literals) << 4 | (m >= 15 ? 15 : m)));
        if (literals >= 15) lengths(literals - 15);
        out.insert(out.end(), s + from, s + from + literals);
        if (!match) return;
        out.push_back(uint8_t(offset & 255));
        out.push_back(uint8_t(offset >> 8));
        if (m >= 15) lengths(m - 15);
    };
    size_t i = 0, anchor = 0;
    if (n >= 16) {
        std::vector<uint32_t> table(1u << 16, 0xFFFFFFFFu);
        const size_t limit = n - 12, last = n - 5;
        while (i < limit) {
            uint32_t v;
            std::memcpy(&v, s + i, 4);
            const uint32_t h = (v * 2654435761u) >> 16;
            const uint32_t candidate = table[h];
            table[h] = uint32_t(i);
            uint32_t w = 0;
            if (candidate != 0xFFFFFFFFu && i - candidate <= 65535) std::memcpy(&w, s + candidate, 4);
            if (candidate != 0xFFFFFFFFu && i - candidate <= 65535 && w == v) {
                size_t m = 4;
                while (i + m < last && s[candidate + m] == s[i + m]) m++;
                sequence(anchor, i - anchor, m, i - candidate);
                i += m;
                anchor = i;
            } else {
                i += 1 + ((i - anchor) >> 6);   // (bytes that do not repeat: longer steps the longer they go on)
            }
        }
    }
    sequence(anchor, n - anchor, 0, 0);
}

bool Unpack(const uint8_t* s, size_t n, std::vector<uint8_t>& out, size_t size) {
    out.clear();
    out.reserve(size);
    size_t i = 0;
    while (i < n) {
        const uint8_t token = s[i++];
        size_t literals = token >> 4;
        if (literals == 15) {
            uint8_t b;
            do {
                if (i >= n) return false;
                b = s[i++];
                literals += b;
            } while (b == 255);
        }
        if (literals > n - i || out.size() + literals > size) return false;
        out.insert(out.end(), s + i, s + i + literals);
        i += literals;
        if (i >= n) break;   // the last sequence: literals only
        if (n - i < 2) return false;
        const size_t offset = size_t(s[i]) | size_t(s[i + 1]) << 8;
        i += 2;
        size_t match = token & 15;
        if (match == 15) {
            uint8_t b;
            do {
                if (i >= n) return false;
                b = s[i++];
                match += b;
            } while (b == 255);
        }
        match += 4;
        if (offset == 0 || offset > out.size() || out.size() + match > size) return false;
        size_t from = out.size() - offset;
        for (size_t k = 0; k < match; k++) out.push_back(out[from + k]);
    }
    return out.size() == size;
}

uint64_t Hash(const uint8_t* s, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, s + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ s[i]) * 0x100000001B3ull;
    return h;
}

// ---- the protocol -------------------------------------------------------------------------------------------------------
constexpr uint32_t kProtocol = 1;
constexpr uint16_t kPort = 26950;
enum Message : uint8_t { MsgHello = 1, MsgPing, MsgPong, MsgSync, MsgState, MsgReady, MsgFail, MsgBye, MsgInput, MsgHash };
constexpr size_t kMostMessage = 512u * 1024u * 1024u;

enum class State {
    Idle,
    Hosting,      // listening, nobody connected
    Connecting,   // the joiner: the connection being made
    Hello,        // connected: waiting for the other's hello
    Saving,       // the host: its state asked for (the boundary's checkpoint writes it)
    Loading,      // the state being loaded in place (the boundary's checkpoint)
    Waiting,      // the joiner: the host's state on its way
    Running,      // lockstep
};

constexpr uint32_t kRing = 256;
constexpr uint32_t kHashEvery = 600;          // boundaries between two comparisons of the memory's hash (10 s)
constexpr double kResyncGapMs = 20000.0;      // the least time between two states sent for a hash that differed
constexpr double kInputTimeoutMs = 15000.0;   // the other's pad not arriving for this long: the session is over
constexpr int kNoticeFrames = 60 * 8;

struct Wire { uint16_t buttons = 0; uint8_t lx = 128, ly = 128, rx = 128, ry = 128; };

struct Global {
    bool installed = false;
    std::string root, state_path;
    State state = State::Idle;
    bool host = false;
    bool session = false;      // a session started and not ended (through a state sent again too)
    Socket listener = kNoSocket, peer = kNoSocket;
    std::vector<uint8_t> in;   // bytes received, not yet a whole message
    double since = 0;          // when the state began (time-outs)
    bool hello_got = false;
    double ping_sent = 0, rtt_ms = -1;
    // the states
    int saved = 0, loaded = 0;   // 0 pending, 1 done, -1 failed
    std::string state_error;
    std::vector<uint8_t> base;   // the last state both machines loaded (the next is sent as its difference)
    bool peer_has_base = false;
    std::vector<uint8_t> pending_base;
    double last_sync = 0;
    bool want_sync = false;
    // lockstep
    uint32_t frame = 0, delay = 3;
    Wire mine[kRing], theirs[kRing];
    uint32_t theirs_upto = 0;    // frames [0, theirs_upto) of the other's pad are here
    Wire pad1;
    bool have_pad1 = false;
    uint32_t my_hash_frame = 0, their_hash_frame = 0;
    uint64_t my_hash = 0, their_hash = 0;
    uint32_t checks = 0, differed = 0;
    // the screen
    std::string notice, code;
    int notice_frames = 0;
    int text_source = -1;
};
Global g;

void Notice(const std::string& text, int frames = kNoticeFrames) {
    g.notice = text;
    g.notice_frames = frames;
    std::string flat = text;
    std::replace(flat.begin(), flat.end(), '~', ' ');
    std::fprintf(stderr, "[twinsanityevolved] online: %s\n", flat.c_str());
}

void Texts(std::vector<texts::Line>& out) {
    if (g.notice_frames <= 0 || g.notice.empty()) return;
    --g.notice_frames;
    texts::Line line;
    line.x = 0.5f;
    line.y = 0.2f;
    line.scale = 0.5f;
    line.colour = 0x80FFFFFFu;
    line.alignment = texts::Top | texts::Centre;
    line.text = g.notice;
    out.push_back(line);
}

void Enter(State s) {
    g.state = s;
    g.since = NowMs();
}

void DropPeer() {
    if (g.peer != kNoSocket) CloseSocket(g.peer);
    g.peer = kNoSocket;
    g.in.clear();
    g.hello_got = false;
    g.session = false;
    g.peer_has_base = false;
    g.want_sync = false;
    g.have_pad1 = false;
    g.rtt_ms = -1;
    Enter(g.host && g.listener != kNoSocket ? State::Hosting : State::Idle);
}

void Lost(const std::string& why) {
    const bool was = g.session;
    DropPeer();
    const std::string words = why.empty() ? std::string("the connection closed") : why;
    Notice(was ? "online: the other player left~" + words : "online: " + words);
}

// Every byte sent, the window kept alive while a long one goes (false: the connection is gone)
bool SendAll(const uint8_t* bytes, size_t n) {
    size_t sent = 0;
    double progress = NowMs();
    while (sent < n) {
        const int chunk = int(std::min<size_t>(n - sent, 256u * 1024u));
        const auto r = send(g.peer, reinterpret_cast<const char*>(bytes + sent), chunk, kSendFlags);
        if (r > 0) {
            sent += size_t(r);
            progress = NowMs();
            continue;
        }
        if (r < 0 && !WouldBlock()) return false;
        if (NowMs() - progress > 20000.0) return false;
        WaitSocket(g.peer, true, 50);
        if (n > 65536) input::KeepAlive();
    }
    return true;
}

bool Send(Message type, const void* payload = nullptr, size_t n = 0) {
    if (g.peer == kNoSocket) return false;
    uint8_t head[5];
    head[0] = type;
    const uint32_t len = uint32_t(n);
    std::memcpy(head + 1, &len, 4);
    if (!SendAll(head, 5) || (n && !SendAll(static_cast<const uint8_t*>(payload), n))) {
        Lost("the connection broke");
        return false;
    }
    return true;
}

// What the two machines must agree on before they play from one state
std::string Fingerprint() {
    const Settings& s = GetSettings();
    std::string f = "hz " + std::to_string(s.field_hz);
    std::error_code ec;
    const auto size = std::filesystem::file_size(s.iso_path, ec);
    f += " disc " + std::to_string(ec ? 0ull : static_cast<unsigned long long>(size));
    f += std::string(" slide ") + (s.slide_steering_fixed ? "1" : "0") + " explosions " + (s.invincible_to_explosions ? "1" : "0");
    f += " mods";
    for (uint32_t i = 0; i < mods::Count(); i++) {
        const mods::Mod* m = mods::At(i);
        if (m && m->id != "online" && mods::Enabled(i)) f += " " + m->id;
    }
    return f;
}

void SendHello() {
    std::vector<uint8_t> p;
    const uint32_t magic = 0x314E4554u, protocol = kProtocol;   // "TEN1"
    p.resize(8);
    std::memcpy(p.data(), &magic, 4);
    std::memcpy(p.data() + 4, &protocol, 4);
    const std::string f = Fingerprint();
    p.insert(p.end(), f.begin(), f.end());
    Send(MsgHello, p.data(), p.size());
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0) return false;
    f.seekg(0);
    out.resize(size_t(size));
    f.read(reinterpret_cast<char*>(out.data()), size);
    return bool(f);
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(f);
}

void StartRunning() {
    g.frame = 0;
    g.theirs_upto = g.delay;   // (the first `delay` boundaries: both pads neutral on both machines)
    for (uint32_t i = 0; i < kRing; i++) { g.mine[i] = Wire{}; g.theirs[i] = Wire{}; }
    g.my_hash_frame = g.their_hash_frame = 0;
    g.session = true;
    g.last_sync = NowMs();
    Enter(State::Running);
    Notice(g.host ? "online: player 2 is in~player 2: press start to join" : "online: you are in~press start to join as player 2");
    std::fprintf(stderr, "[twinsanityevolved] online: lockstep from here, the pads %u boundaries late (round trip %.0f ms)\n", g.delay, g.rtt_ms);
}

// The host: its state made, sent and (once the joiner has it) loaded here too
void BeginSync() {
    if (!Send(MsgSync)) return;
    g.saved = 0;
    g.want_sync = false;
    savestate::RequestSave(g.state_path);
    Enter(State::Saving);
    Notice(g.session ? "online: the two games differ~sending the game again" : "online: player 2 is joining~sending the game");
}

bool Pump(bool wait_ready);

void SendState() {
    std::vector<uint8_t> raw;
    if (g.saved < 0 || !ReadFile(g.state_path, raw)) {
        Send(MsgBye);
        Lost("the game's state could not be saved: " + g.state_error);
        return;
    }
    const bool delta = g.peer_has_base && !g.base.empty();
    std::vector<uint8_t> body = raw;
    if (delta) {
        const size_t n = std::min(body.size(), g.base.size());
        for (size_t i = 0; i < n; i++) body[i] ^= g.base[i];
    }
    std::vector<uint8_t> packed;
    Pack(body.data(), body.size(), packed);
    body.clear();
    body.shrink_to_fit();
    // the pads' delay from the round trip: half of it and a field to spare, in fields
    const double field = 1000.0 / double(GetSettings().field_hz ? GetSettings().field_hz : 60);
    const double rtt = g.rtt_ms < 0 ? 60.0 : g.rtt_ms;
    const uint32_t delay = uint32_t(std::min(12.0, std::max(2.0, std::ceil((rtt * 0.5 + 6.0) / field) + 1.0)));
    g.delay = delay;
    std::vector<uint8_t> head(8);
    const uint32_t raw_size = uint32_t(raw.size());
    std::memcpy(head.data(), &raw_size, 4);
    head[4] = delta ? 1 : 0;
    head[5] = uint8_t(delay);
    std::fprintf(stderr, "[twinsanityevolved] online: the state %.1f MB sent as %.1f MB (%s)\n", raw.size() / 1048576.0, packed.size() / 1048576.0,
                 delta ? "its difference from the last" : "whole");
    uint8_t lead[5];
    lead[0] = MsgState;
    const uint32_t len = uint32_t(head.size() + packed.size());
    std::memcpy(lead + 1, &len, 4);
    const double began = NowMs();
    if (!SendAll(lead, 5) || !SendAll(head.data(), head.size()) || !SendAll(packed.data(), packed.size())) {
        Lost("the connection broke while the game was sent");
        return;
    }
    std::fprintf(stderr, "[twinsanityevolved] online: sent in %.1f s\n", (NowMs() - began) / 1000.0);
    // the joiner's answer: it has loaded the state (or could not)
    g.loaded = 0;
    const double deadline = NowMs() + 180000.0;
    bool ready = false;
    while (g.peer != kNoSocket && !ready && NowMs() < deadline) {
        ready = Pump(true);
        if (!ready && g.peer != kNoSocket) {
            WaitSocket(g.peer, false, 50);
            input::KeepAlive();
        }
    }
    if (g.peer == kNoSocket) return;
    if (!ready) {
        Send(MsgBye);
        Lost("player 2 did not load the game");
        return;
    }
    g.pending_base.swap(raw);
    savestate::RequestLoad(g.state_path);
    Enter(State::Loading);
}

// The joiner: a state arrived
void TakeState(const uint8_t* p, size_t n) {
    if (n < 8) { Lost("a broken state arrived"); return; }
    uint32_t raw_size;
    std::memcpy(&raw_size, p, 4);
    const bool delta = p[4] != 0;
    g.delay = std::min<uint32_t>(12, std::max<uint32_t>(1, p[5]));
    std::vector<uint8_t> raw;
    if (raw_size > kMostMessage || !Unpack(p + 8, n - 8, raw, raw_size) || (delta && g.base.empty())) {
        Send(MsgFail, "unpack", 6);
        Lost("the game's state arrived broken");
        return;
    }
    if (delta) {
        const size_t m = std::min(raw.size(), g.base.size());
        for (size_t i = 0; i < m; i++) raw[i] ^= g.base[i];
    }
    if (!WriteFile(g.state_path, raw)) {
        Send(MsgFail, "write", 5);
        Lost("the game's state could not be written to " + g.state_path);
        return;
    }
    g.pending_base.swap(raw);
    g.loaded = 0;
    savestate::RequestLoad(g.state_path);
    Enter(State::Loading);
    Notice("online: loading the host's game");
}

void CompareHashes() {
    if (g.my_hash_frame == 0 || g.my_hash_frame != g.their_hash_frame) return;
    ++g.checks;
    const bool same = g.my_hash == g.their_hash;
    if (!same) ++g.differed;
    std::fprintf(stderr, "[twinsanityevolved] online: boundary %u of the session: the two memories %s (%u checks, %u differed)\n", g.my_hash_frame,
                 same ? "agree" : "DIFFER", g.checks, g.differed);
    g.my_hash_frame = g.their_hash_frame = 0;
    if (!same && g.host && NowMs() - g.last_sync > kResyncGapMs) g.want_sync = true;
}

// One whole message. True: the answer SendState waits for (the joiner is ready)
bool Handle(uint8_t type, const uint8_t* p, size_t n) {
    switch (type) {
    case MsgHello: {
        uint32_t magic = 0, protocol = 0;
        if (n >= 8) { std::memcpy(&magic, p, 4); std::memcpy(&protocol, p + 4, 4); }
        const std::string theirs(reinterpret_cast<const char*>(p) + std::min<size_t>(n, 8), n > 8 ? n - 8 : 0);
        const std::string ours = Fingerprint();
        if (magic != 0x314E4554u || protocol != kProtocol || theirs != ours) {
            std::fprintf(stderr, "[twinsanityevolved] online: refused -- here: %s | there: %s\n", ours.c_str(), theirs.c_str());
            Send(MsgBye);
            Lost("the two games are not the same~both need the same disc, mods and settings");
            return false;
        }
        g.hello_got = true;
        return false;
    }
    case MsgPing: Send(MsgPong, p, n); return false;
    case MsgPong:
        if (g.ping_sent > 0) g.rtt_ms = NowMs() - g.ping_sent;
        return false;
    case MsgSync:
        if (!g.host) {
            Enter(State::Waiting);
            Notice(g.session ? "online: the two games differ~receiving the host's game again" : "online: receiving the host's game", 60 * 120);
        }
        return false;
    case MsgState:
        if (!g.host) TakeState(p, n);
        return false;
    case MsgReady: return g.host;
    case MsgFail:
        Lost("player 2 could not load the game");
        return false;
    case MsgBye:
        Lost("");
        return false;
    case MsgInput: {
        if (n < 10) return false;
        uint32_t frame;
        std::memcpy(&frame, p, 4);
        Wire w;
        std::memcpy(&w.buttons, p + 4, 2);
        w.lx = p[6]; w.ly = p[7]; w.rx = p[8]; w.ry = p[9];
        if (g.state == State::Running && frame == g.theirs_upto) {
            g.theirs[frame % kRing] = w;
            g.theirs_upto = frame + 1;
        }
        return false;
    }
    case MsgHash:
        if (n >= 12 && g.state == State::Running) {
            std::memcpy(&g.their_hash_frame, p, 4);
            std::memcpy(&g.their_hash, p + 4, 8);
            CompareHashes();
        }
        return false;
    default: return false;
    }
}

// What has arrived, read and handled. True: MsgReady was among it (wait_ready stops there)
bool Pump(bool wait_ready) {
    for (;;) {
        if (g.peer == kNoSocket) return false;
        // the whole messages already here
        size_t at = 0;
        bool ready = false;
        const Socket peer = g.peer;
        while (g.in.size() - at >= 5) {
            // (the other's pads for boundaries well ahead wait their turn: the ring holds only so many)
            if (g.state == State::Running && g.theirs_upto > g.frame + kRing / 2) break;
            uint32_t len;
            std::memcpy(&len, g.in.data() + at + 1, 4);
            if (len > kMostMessage) { Lost("a broken message arrived"); return false; }
            if (g.in.size() - at - 5 < len) break;
            const uint8_t type = g.in[at];
            // (handled from a copy: a handler may drop the connection, and the buffer with it)
            const std::vector<uint8_t> payload(g.in.begin() + long(at + 5), g.in.begin() + long(at + 5 + len));
            at += 5 + len;
            if (Handle(type, payload.data(), payload.size())) ready = true;
            if (g.peer != peer) return false;
            if (ready && wait_ready) break;
        }
        g.in.erase(g.in.begin(), g.in.begin() + long(at));
        if (ready && wait_ready) return true;
        if (g.state == State::Running && g.theirs_upto > g.frame + kRing / 2) return false;
        if (g.state == State::Loading) return false;   // (what follows a state is for the game it starts)
        // a state on its way: how much of it is here
        if (!g.host && g.state == State::Waiting && g.in.size() >= 5 && g.in[0] == MsgState) {
            uint32_t len;
            std::memcpy(&len, g.in.data() + 1, 4);
            char text[96];
            std::snprintf(text, sizeof text, "online: receiving the host's game~%u of %u mb", unsigned(g.in.size() >> 20), unsigned(len >> 20));
            g.notice = text;
            g.notice_frames = 60 * 120;
        }
        uint8_t buffer[65536];
        const auto r = recv(g.peer, reinterpret_cast<char*>(buffer), sizeof buffer, 0);
        if (r == 0) { Lost(""); return false; }
        if (r < 0) {
            if (!WouldBlock()) Lost("the connection broke");
            return false;
        }
        g.in.insert(g.in.end(), buffer, buffer + r);
    }
}

Wire WireOf(const Pads::Sample& s) {
    Wire w;
    w.buttons = s.buttons;
    w.lx = s.lx; w.ly = s.ly; w.rx = s.rx; w.ry = s.ry;
    return w;
}

Pads::Sample SampleOf(const Wire& w) {
    Pads::Sample s;
    s.connected = true;
    s.buttons = w.buttons;
    s.lx = w.lx; s.ly = w.ly; s.rx = w.rx; s.ry = w.ry;
    return s;
}

// ---- the rows of Options > Mods > online ---------------------------------------------------------------------------------
void Leave() {
    const bool any = g.state != State::Idle;
    if (g.peer != kNoSocket) Send(MsgBye);
    if (g.listener != kNoSocket) CloseSocket(g.listener);
    g.listener = kNoSocket;
    g.host = false;
    DropPeer();
    g.base.clear();
    g.code.clear();
    Enter(State::Idle);
    Notice(any ? "online: the lobby is closed" : "online: no lobby is open");
}

uint16_t Port() {
    const char* v = std::getenv("TE_ONLINE_PORT");
    const long n = v ? std::atol(v) : 0;
    return n > 0 && n < 65536 ? uint16_t(n) : kPort;
}

void HostLobby() {
    if (g.state != State::Idle && !(g.host && g.state == State::Hosting)) {
        Notice("online: a lobby is already open~choose leave first");
        return;
    }
    if (!StartSockets()) { Notice("online: the network could not be started"); return; }
    if (g.listener == kNoSocket) {
        const Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == kNoSocket) { Notice("online: the network could not be started"); return; }
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        any.sin_port = htons(Port());
        if (bind(s, reinterpret_cast<const sockaddr*>(&any), sizeof any) != 0 || listen(s, 1) != 0) {
            CloseSocket(s);
            Notice("online: the lobby could not be opened~is another copy of the game hosting?");
            return;
        }
        NonBlocking(s);
        g.listener = s;
    }
    g.host = true;
    Enter(State::Hosting);
    const std::vector<uint32_t> addresses = LocalAddresses();
    for (uint32_t a : addresses)
        std::fprintf(stderr, "[twinsanityevolved] online: this machine's address %s is the code %s\n", Dotted(a).c_str(), CodeOf(a).c_str());
    if (addresses.empty()) {
        g.code.clear();
        Notice("online: the lobby is open~give your friend this machine's vpn address", 60 * 30);
        return;
    }
    g.code = CodeOf(addresses[0]);
    input::SetClipboard(g.code);
    Notice("online: the lobby is open  code " + g.code + "~copied: send it to your friend", 60 * 30);
}

void JoinLobby() {
    if (g.state != State::Idle) {
        Notice("online: a lobby is already open~choose leave first");
        return;
    }
    std::string text = input::Clipboard();
    uint32_t address = 0;
    if (!AddressOf(text, address)) {
        // (beside the game's settings: a file with the code, for a machine whose clipboard cannot be read)
        std::ifstream f((std::filesystem::path(g.root) / "online_code.txt").string());
        std::string line;
        if (f && std::getline(f, line)) text = line;
        if (!AddressOf(text, address)) {
            Notice("online: no lobby code is copied~copy your friend's code, then choose join");
            return;
        }
    }
    if (!StartSockets()) { Notice("online: the network could not be started"); return; }
    const Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) { Notice("online: the network could not be started"); return; }
    NonBlocking(s);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(address);
    to.sin_port = htons(Port());
    const int r = connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    if (r != 0 && !WouldBlock()) {
        CloseSocket(s);
        Notice("online: the lobby " + CodeOf(address) + " could not be reached");
        return;
    }
    g.host = false;
    g.peer = s;
    g.code = CodeOf(address);
    Enter(State::Connecting);
    Notice("online: joining the lobby " + g.code, 60 * 20);
    std::fprintf(stderr, "[twinsanityevolved] online: connecting to %s\n", Dotted(address).c_str());
}

// What the boundary does for the state the session is in (before the pads)
void Step() {
    switch (g.state) {
    case State::Idle: break;
    case State::Hosting: {
        sockaddr_in from{};
        SockLen len = sizeof from;
        const Socket s = accept(g.listener, reinterpret_cast<sockaddr*>(&from), &len);
        if (s == kNoSocket) break;
        NonBlocking(s);
        NoDelay(s);
        g.peer = s;
        g.in.clear();
        g.hello_got = false;
        g.peer_has_base = false;
        Enter(State::Hello);
        std::fprintf(stderr, "[twinsanityevolved] online: a connection from %s\n", Dotted(ntohl(from.sin_addr.s_addr)).c_str());
        SendHello();
        if (g.peer != kNoSocket) {
            g.ping_sent = NowMs();
            Send(MsgPing);
        }
        break;
    }
    case State::Connecting: {
        if (WaitSocket(g.peer, true, 0)) {
            int error = 0;
            SockLen len = sizeof error;
            getsockopt(g.peer, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len);
            if (error != 0) { Lost("the lobby " + g.code + " did not answer~is the code right, and the vpn on?"); break; }
            NoDelay(g.peer);
            Enter(State::Hello);
            SendHello();
        } else if (NowMs() - g.since > 10000.0) {
            Lost("the lobby " + g.code + " did not answer~is the code right, and the vpn on?");
        }
        break;
    }
    case State::Hello:
        Pump(false);
        if (g.state != State::Hello) break;
        if (g.hello_got && g.host && (g.rtt_ms >= 0 || NowMs() - g.since > 2000.0)) BeginSync();
        else if (g.hello_got && !g.host) { Enter(State::Waiting); Notice("online: connected~waiting for the host's game", 60 * 120); }
        else if (NowMs() - g.since > 10000.0) Lost("the other game did not answer");
        break;
    case State::Saving:
        if (g.saved != 0) SendState();
        else if (NowMs() - g.since > 10000.0) { Send(MsgBye); Lost("the game's state was not saved"); }
        break;
    case State::Loading:
        if (g.loaded > 0) {
            g.base.swap(g.pending_base);
            g.pending_base.clear();
            g.pending_base.shrink_to_fit();
            g.peer_has_base = true;
            if (!g.host && !Send(MsgReady)) break;
            StartRunning();
        } else if (g.loaded < 0) {
            if (!g.host) Send(MsgFail, "load", 4); else Send(MsgBye);
            Lost("the game's state could not be loaded: " + g.state_error);
        } else if (NowMs() - g.since > 30000.0) {
            Send(MsgBye);
            Lost("the game's state was not loaded");
        }
        break;
    case State::Waiting:
        Pump(false);
        if (g.state == State::Waiting && NowMs() - g.since > 300000.0) { Send(MsgBye); Lost("the host's game did not arrive"); }
        break;
    case State::Running: break;
    }
}

}  // namespace

bool Lockstep() { return g.session || g.state == State::Loading; }

int LocalPad() { return g.session ? (g.host ? 0 : 1) : -1; }

Pads::Sample Pad0(const Pads::Sample& local) {
    g.have_pad1 = false;
    if (g.state == State::Idle) return local;
    Step();
    if (g.state != State::Running) {
        if (!g.session && g.state != State::Loading && g.state != State::Waiting) return local;
        // (a state on its way or being loaded: both pads at rest until the two play from it)
        g.have_pad1 = true;
        g.pad1 = Wire{};
        Pads::Sample rest;
        rest.connected = true;
        return rest;
    }
    if (g.want_sync && g.host) {
        BeginSync();
        g.have_pad1 = true;
        g.pad1 = Wire{};
        Pads::Sample rest;
        rest.connected = true;
        return rest;
    }
    const uint32_t f = g.frame;
    // this machine's pad, played `delay` boundaries from now on both machines
    {
        const Wire w = WireOf(local);
        g.mine[(f + g.delay) % kRing] = w;
        uint8_t p[10];
        const uint32_t at = f + g.delay;
        std::memcpy(p, &at, 4);
        std::memcpy(p + 4, &w.buttons, 2);
        p[6] = w.lx; p[7] = w.ly; p[8] = w.rx; p[9] = w.ry;
        if (!Send(MsgInput, p, sizeof p)) return local;
    }
    if (f != 0 && f % kHashEvery == 0) {
        g.my_hash = Hash(GuestMemory(), GuestMemorySize());
        g.my_hash_frame = f;
        uint8_t p[12];
        std::memcpy(p, &f, 4);
        std::memcpy(p + 4, &g.my_hash, 8);
        if (!Send(MsgHash, p, sizeof p)) return local;
        CompareHashes();
    }
    // the other's pad for this boundary: the game waits for it
    const double began = NowMs();
    for (;;) {
        Pump(false);
        if (g.state != State::Running) {   // the connection went, or the host sends its state again
            if (!g.session) return local;
            g.have_pad1 = true;
            g.pad1 = Wire{};
            Pads::Sample rest;
            rest.connected = true;
            return rest;
        }
        if (g.theirs_upto > f) break;
        const double waited = NowMs() - began;
        if (waited > kInputTimeoutMs) {
            Send(MsgBye);
            Lost("the connection is too slow or gone");
            return local;
        }
        WaitSocket(g.peer, false, waited > 100.0 ? 20 : 2);
        if (waited > 250.0) input::KeepAlive();
    }
    const Wire mine = g.mine[f % kRing], theirs = g.theirs[f % kRing];
    g.frame = f + 1;
    g.have_pad1 = true;
    g.pad1 = g.host ? theirs : mine;
    return SampleOf(g.host ? mine : theirs);
}

bool Pad1(Pads::Sample& out) {
    if (!g.have_pad1) return false;
    out = SampleOf(g.pad1);
    return true;
}

void Saved(const std::string& path, bool ok, const std::string& error) {
    if (path != g.state_path || g.state != State::Saving) return;
    g.saved = ok ? 1 : -1;
    g.state_error = error;
}

void Loaded(const std::string& path, bool ok, const std::string& error) {
    if (path != g.state_path || g.state != State::Loading) return;
    g.loaded = ok ? 1 : -1;
    g.state_error = error;
}

void Install(const std::string& root) {
    if (g.installed) return;
    g.installed = true;
    g.root = root;
    std::error_code ec;
    const std::filesystem::path folder = std::filesystem::path(root.empty() ? std::string(".") : root) / "online";
    std::filesystem::create_directories(folder, ec);
    g.state_path = (folder / "session.ps2st").generic_string();
    g.text_source = texts::Register("online", Texts);
}

#endif   // !__vita__

}  // namespace te::netplay

// ---- the mod's row on Options > Mods ------------------------------------------------------------------------------------
namespace te::mods {

void RegisterOnline() {
    Mod m;
    m.id = "online";
    m.name = "online";
    m.version = "0.1.0";
    m.has_switch = false;
    m.page_title = "online";
#if defined(__vita__)
    m.page_rows = [] { return uint32_t(0); };
#else
    m.page_rows = [] { return uint32_t(3); };
    // (row 100 is not a row of the page: the game's two-player mod asks it every frame -- "1" while a session runs, and each
    // player's view then fills that player's own screen: game/src/game/coop.cpp)
    m.page_row_label = [](uint32_t row) -> std::string {
        switch (row) {
        case 0: return "host a lobby";
        case 1: return "join with the copied code";
        case 2: return "leave";
        case 100: return te::netplay::LocalPad() >= 0 ? "1" : "0";
        default: return "";
        }
    };
    m.page_row_chosen = [](uint32_t row, WarpTarget&) {
        if (row == 0) te::netplay::HostLobby();
        else if (row == 1) te::netplay::JoinLobby();
        else if (row == 2) te::netplay::Leave();
        return ActionNone;
    };
#endif
    Register(m);
}

}  // namespace te::mods

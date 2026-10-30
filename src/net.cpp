#include "net.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
using nativesock = SOCKET;
static constexpr long long kInvalid = (long long)INVALID_SOCKET;
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
using nativesock = int;
static constexpr long long kInvalid = -1;
#define CLOSESOCK ::close
#endif

// Recover the platform-native socket handle from the stored int64 without
// truncating on 64-bit Windows (SOCKET is a 64-bit handle there).
#define SK(x) ((nativesock)(x))

namespace {

// One-time Winsock startup (no-op on POSIX). Reference-counted teardown is not
// needed for a single short-lived process, so we leave WSACleanup to exit.
bool ensure_started() {
#ifdef _WIN32
    static bool started = [] {
        WSADATA d{};
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return started;
#else
    return true;
#endif
}

sockaddr_in loopback_addr(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    return a;
}

#ifdef _WIN32
// Stop Windows reporting ICMP "port unreachable" (nobody listening yet) as a
// WSAECONNRESET error on the next send/recv of this UDP socket.
void disable_connreset(long long sock) {
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(SK(sock), SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
}
#endif

int last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// Ask whoever is bound to `port` (normally a stale overlay) to quit.
void send_quit(uint16_t port) {
    const nativesock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ((long long)s == kInvalid) return;
    sockaddr_in a = loopback_addr(port);
    const unsigned char quit[4] = {0, 0, 0, 200};
    sendto(s, (const char*)quit, 4, 0, (sockaddr*)&a, sizeof(a));
    CLOSESOCK(s);
}

}  // namespace

UdpNet::~UdpNet() { close(); }

bool UdpNet::init_send(uint16_t send_port) {
    if (!ensure_started()) {
        std::cout << "[ERROR] socket startup failed\n";
        return false;
    }
    long long s = (long long)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) {
        std::cout << "[ERROR] failed to create send socket (" << last_error() << ")\n";
        return false;
    }
    send_sock_ = s;
#ifdef _WIN32
    disable_connreset(send_sock_);
#endif
    // connect() on a UDP socket only sets the default peer, so plain send() works.
    sockaddr_in addr = loopback_addr(send_port);
    if (connect(SK(send_sock_), (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::cout << "[ERROR] failed to connect send socket (" << last_error() << ")\n";
        close();
        return false;
    }
    return true;
}

bool UdpNet::init_recv(uint16_t recv_port, double wait_s) {
    if (!ensure_started()) return false;
    long long s = (long long)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) {
        std::cout << "[ERROR] failed to create recv socket (" << last_error() << ")\n";
        return false;
    }
    recv_sock_ = s;

#ifdef _WIN32
    disable_connreset(recv_sock_);
    // Exclusive: nobody (e.g. a second overlay) may silently share the port and
    // steal commands. The old build used SO_REUSEADDR, which on Windows let a
    // second instance bind the same port with undefined delivery.
    int excl = 1;
    setsockopt(SK(recv_sock_), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl));
#endif

    sockaddr_in addr = loopback_addr(recv_port);
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds((int)(wait_s * 1000.0));
    bool asked = false;
    while (bind(SK(recv_sock_), (sockaddr*)&addr, sizeof(addr)) != 0) {
        const int err = last_error();
        if (!asked) {
            std::cout << "[WARN] Command port " << recv_port << " is busy (" << err
                      << "); asking a leftover overlay instance to quit\n";
            asked = true;
        }
        if (clock::now() >= deadline) {
            std::cout << "[ERROR] failed to bind recv socket on port " << recv_port
                      << " — is another overlay still running?\n";
            CLOSESOCK(SK(recv_sock_));
            recv_sock_ = kInvalid;
            return false;
        }
        send_quit(recv_port);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    if (asked) std::cout << "[INFO] Command port " << recv_port << " acquired\n";

    // Non-blocking: polled once per frame.
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(SK(recv_sock_), FIONBIO, &nb);
#else
    int flags = fcntl(SK(recv_sock_), F_GETFL, 0);
    fcntl(SK(recv_sock_), F_SETFL, flags | O_NONBLOCK);
#endif
    return true;
}

void UdpNet::send_int32(int32_t v) {
    if (send_sock_ == kInvalid) return;
    uint32_t nv = htonl((uint32_t)v);
    if (send(SK(send_sock_), (const char*)&nv, 4, 0) != 4) {
        // With nobody listening, Windows reports the ICMP "port unreachable"
        // from a previous datagram here (WSAECONNRESET); harmless for UDP.
        const int err = last_error();
#ifdef _WIN32
        if (err == WSAECONNRESET) {
            if (send(SK(send_sock_), (const char*)&nv, 4, 0) == 4) return;
        }
#else
        if (err == ECONNREFUSED) {
            if (send(SK(send_sock_), (const char*)&nv, 4, 0) == 4) return;
        }
#endif
        std::cout << "[WARN] UDP send of " << v << " failed (" << err << ")\n";
    }
}

int UdpNet::recv(void* buf, int buflen) {
    if (recv_sock_ == kInvalid) return 0;
    for (;;) {
        int n = (int)recvfrom(SK(recv_sock_), (char*)buf, buflen, 0, nullptr, nullptr);
        if (n >= 0) return n;
        const int err = last_error();
#ifdef _WIN32
        if (err == WSAEWOULDBLOCK) return 0;
        // Oversized datagram: truncated into buf; treat as received.
        if (err == WSAEMSGSIZE) return buflen;
        // ICMP port-unreachable echoes surface on Windows UDP sockets; skip.
        if (err == WSAECONNRESET) continue;
#else
        if (err == EAGAIN || err == EWOULDBLOCK) return 0;
        if (err == EINTR || err == ECONNREFUSED) continue;
#endif
        return -1;
    }
}

void UdpNet::close() {
    if (send_sock_ != kInvalid) {
        CLOSESOCK(SK(send_sock_));
        send_sock_ = kInvalid;
    }
    if (recv_sock_ != kInvalid) {
        CLOSESOCK(SK(recv_sock_));
        recv_sock_ = kInvalid;
    }
}

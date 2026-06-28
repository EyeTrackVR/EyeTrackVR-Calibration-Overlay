#include "net.h"

#include <cstring>
#include <iostream>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socklen_t = int;
   using nativesock = SOCKET;
   static constexpr long long kInvalid = (long long)INVALID_SOCKET;
#  define CLOSESOCK closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
   using nativesock = int;
   static constexpr long long kInvalid = -1;
#  define CLOSESOCK ::close
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
    a.sin_port   = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    return a;
}

} // namespace

UdpNet::~UdpNet() { close(); }

bool UdpNet::init_send(uint16_t send_port) {
    if (!ensure_started()) {
        std::cout << "[ERROR] socket startup failed\n";
        return false;
    }
    long long s = (long long)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) {
        std::cout << "[ERROR] failed to create send socket\n";
        return false;
    }
    send_sock_ = s;
    // The target address is fixed; store it lazily per send via connect() so we
    // can use plain send(). connect() on a UDP socket only sets the default peer.
    sockaddr_in addr = loopback_addr(send_port);
    if (connect(SK(send_sock_), (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::cout << "[ERROR] failed to connect send socket\n";
        close();
        return false;
    }
    return true;
}

bool UdpNet::init_recv(uint16_t recv_port) {
    if (!ensure_started()) return false;
    long long s = (long long)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) {
        std::cout << "[ERROR] failed to create recv socket\n";
        return false;
    }
    recv_sock_ = s;

    int reuse = 1;
    setsockopt(SK(recv_sock_), SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = loopback_addr(recv_port);
    if (bind(SK(recv_sock_), (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::cout << "[ERROR] failed to bind recv socket on port " << recv_port << "\n";
        CLOSESOCK(SK(recv_sock_));
        recv_sock_ = kInvalid;
        return false;
    }

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
    if (send(SK(send_sock_), (const char*)&nv, 4, 0) != 4)
        std::cout << "[WARN] UDP send failed\n";
}

int UdpNet::recv(void* buf, int buflen) {
    if (recv_sock_ == kInvalid) return 0;
    int n = (int)recvfrom(SK(recv_sock_), (char*)buf, buflen, 0, nullptr, nullptr);
    if (n >= 0) return n;
#ifdef _WIN32
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) return 0;
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
#endif
    return -1;
}

void UdpNet::close() {
    if (send_sock_ != kInvalid) { CLOSESOCK(SK(send_sock_)); send_sock_ = kInvalid; }
    if (recv_sock_ != kInvalid) { CLOSESOCK(SK(recv_sock_)); recv_sock_ = kInvalid; }
}

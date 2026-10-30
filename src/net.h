#pragma once
#include <cstdint>

// ── Cross-platform UDP transport ──────────────────────────────────────────────
//
// Replaces the winsock-specific glue from the OpenVR build. The wire protocol is
// unchanged: big-endian int32 signals are sent to 127.0.0.1:PORT_SIGNAL and
// commands are received on PORT_CMD. recv is non-blocking so it can be polled
// once per render frame. send_int32/recv are virtual so tests can substitute an
// in-memory transport.

class UdpNet {
public:
    UdpNet() = default;
    virtual ~UdpNet();

    UdpNet(const UdpNet&) = delete;
    UdpNet& operator=(const UdpNet&) = delete;

    // Create the send socket targeting 127.0.0.1:send_port. Returns false on error.
    bool init_send(uint16_t send_port);

    // Create and bind a non-blocking recv socket on 127.0.0.1:recv_port. The
    // port is bound exclusively: if a leftover overlay instance still holds it,
    // that instance is asked to quit (command 200) and the bind is retried for
    // up to `wait_s` seconds. Returns false on error.
    bool init_recv(uint16_t recv_port, double wait_s = 3.0);

    // Send a big-endian int32 signal.
    virtual void send_int32(int32_t v);

    // Non-blocking receive into buf. Returns the datagram length, 0 if none is
    // available, or -1 on a hard error. Safe to call even if init_recv() was
    // never called (always returns 0).
    virtual int recv(void* buf, int buflen);

    void close();

private:
    // Stored as int64 so the header stays free of platform socket headers.
    long long send_sock_ = -1;
    long long recv_sock_ = -1;
};

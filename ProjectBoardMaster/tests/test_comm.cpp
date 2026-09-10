// End-to-end check of the framing and the register messages, against a fake
// MiniGPIO that speaks the same protocol over TCP loopback.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <thread>
#include <vector>

#include "Comm.h"

static int failures = 0;
static void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++failures;
}

// ------------------------------------------------------------- codec tests

static void CodecTests() {
  std::printf("codec:\n");

  // A payload containing 0x55 must survive escaping.
  const u8 payload[] = {42, 0x55, 0x00, 0x55, 0x33, 0xAA, 7};
  std::vector<u8> frame = EncodeFrame(payload, sizeof(payload), 0xFF);

  // 0x55 in the body is followed by 0xAA on the wire.
  int escaped = 0;
  for (size_t i = 2; i + 1 < frame.size(); ++i) {
    if (frame[i] == 0x55 && frame[i + 1] == 0xAA) ++escaped;
  }
  check(escaped >= 2, "0x55 bytes in the payload are escaped");
  check(frame[0] == 0x55 && frame[1] == 0x33, "frame starts with the sync pair");

  FrameDecoder dec;
  dec.reset();
  bool got = false;
  for (u8 b : frame) {
    if (dec.feed(b)) { got = true; break; }
  }
  check(got, "a full frame decodes");
  check(dec.payload().size() == sizeof(payload), "payload length round-trips");
  bool same = dec.payload().size() == sizeof(payload);
  for (size_t i = 0; same && i < sizeof(payload); ++i) {
    if (dec.payload()[i] != payload[i]) same = false;
  }
  check(same, "payload bytes round-trip byte for byte");

  // Leading noise before the sync pair must be ignored.
  {
    FrameDecoder d2;
    d2.reset();
    const u8 noise[] = {0x00, 0x12, 0xFF, 0x33};
    for (u8 b : noise) d2.feed(b);
    bool ok2 = false;
    for (u8 b : frame) {
      if (d2.feed(b)) { ok2 = true; break; }
    }
    check(ok2, "noise before a frame is skipped");
  }

  // A corrupted checksum must be rejected.
  {
    FrameDecoder d3;
    d3.reset();
    std::vector<u8> bad = frame;
    bad.back() = u8(bad.back() ^ 0xFF);
    bool ok3 = false;
    for (u8 b : bad) {
      if (d3.feed(b)) ok3 = true;
    }
    check(!ok3, "a bad checksum is rejected");
    check(!d3.lastError().empty(), "the checksum failure is reported");
  }
}

// ------------------------------------------------------------- fake board

struct FakeBoard {
  std::atomic<bool>       stop{false};
  std::atomic<int>        port{0};
  std::map<int, unsigned> regs8;
  std::map<int, unsigned> regs16;
  std::thread             th;

  // Fault injection, to reproduce what real ports do.
  std::atomic<bool> echoRequest{false};   // half-duplex transceiver echo
  std::atomic<bool> sendStaleAck{false};  // a late reply from an earlier timeout
  std::atomic<bool> goSilent{false};      // powered-off board

  void run() {
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;   // let the OS pick
    bind(srv, (sockaddr*)&a, sizeof(a));
    int alen = sizeof(a);
    getsockname(srv, (sockaddr*)&a, &alen);
    port = ntohs(a.sin_port);
    listen(srv, 1);

    SOCKET cl = accept(srv, nullptr, nullptr);
    closesocket(srv);
    if (cl == INVALID_SOCKET) return;

    FrameDecoder dec;
    dec.reset();
    while (!stop) {
      u8  buf[256];
      int n = recv(cl, (char*)buf, sizeof(buf), 0);
      if (n <= 0) break;
      for (int i = 0; i < n; ++i) {
        if (!dec.feed(buf[size_t(i)])) continue;
        const std::vector<u8>& p = dec.payload();
        if (p.empty()) continue;

        // Echo the request back first, the way a half-duplex transceiver does.
        if (echoRequest) {
          std::vector<u8> e = EncodeFrame(p.data(), p.size(), 0xFF);
          send(cl, (const char*)e.data(), int(e.size()), 0);
        }
        // A stale ACK arriving before the real answer.
        if (sendStaleAck) {
          const u8        ack[2] = {MSG_ACK, 0};
          std::vector<u8> s      = EncodeFrame(ack, sizeof(ack), 0xFF);
          send(cl, (const char*)s.data(), int(s.size()), 0);
        }
        if (goSilent) continue;

        std::vector<u8> reply;
        const int       reg = (p.size() >= 3) ? int(p[1] | (p[2] << 8)) : 0;

        switch (p[0]) {
          case MSG_DEVICE_GET_STATUS: {
            const u8 flags = u8(MINI_GPIO_CARD_ID << 2);   // app running, card id 7
            reply = {MSG_HERE_IS_DEVICE_STATUS, flags, 0x00, 0x01, 4, 0, 12, 0};
            break;
          }
          case MSG_UC_REG8_READ:
            reply = {MSG_HERE_IS_UC_REG8_VALUE, u8(regs8.count(reg) ? regs8[reg] : 0)};
            break;
          case MSG_UC_REG16_READ: {
            const unsigned v = regs16.count(reg) ? regs16[reg] : 0;
            reply = {MSG_HERE_IS_UC_REG16_VALUE, u8(v & 0xFF), u8((v >> 8) & 0xFF)};
            break;
          }
          case MSG_UC_REG8_WRITE:
            if (p.size() >= 4) {
              regs8[reg] = p[3];
              reply      = {MSG_ACK, 0};
            } else {
              reply = {MSG_NAK, 4};
            }
            break;
          case MSG_UC_REG16_WRITE:
            if (p.size() >= 5) {
              regs16[reg] = unsigned(p[3] | (p[4] << 8));
              reply       = {MSG_ACK, 0};
            } else {
              reply = {MSG_NAK, 4};
            }
            break;
          default:
            reply = {MSG_NAK, 1};   // NAK_INVALID_CMD
            break;
        }

        std::vector<u8> out = EncodeFrame(reply.data(), reply.size(), 0xFF);
        send(cl, (const char*)out.data(), int(out.size()), 0);
      }
    }
    closesocket(cl);
  }
};

// ------------------------------------------------------------- session tests

static void SessionTests() {
  std::printf("session over TCP against a fake board:\n");

  FakeBoard fb;
  fb.regs8[88 + 1]   = 1;      // a DI pin reading high
  fb.regs16[16 + 2]  = 812;    // an AI pin
  fb.th              = std::thread([&fb] { fb.run(); });
  for (int i = 0; i < 200 && fb.port == 0; ++i) Sleep(5);
  check(fb.port != 0, "fake board is listening");

  TcpLink link("127.0.0.1", fb.port);
  std::string err;
  check(link.open(err), "TcpLink connects");
  if (!err.empty()) std::printf("      err: %s\n", err.c_str());

  Session s;
  s.setLink(&link);
  s.setStation(0xFF);
  s.setTimeout(1000);
  s.setTag("T1");

  DeviceStatus st;
  check(s.identify(st, err), "identify succeeds");
  check(st.cardId == MINI_GPIO_CARD_ID, "card id is 7");
  check(!st.isBootloader, "bootloader bit is clear");
  check(st.major == 4 && st.minor == 12, "firmware version decodes as 4.12");

  u8 v8 = 0;
  check(s.readReg8(89, v8, err), "readReg8 succeeds");
  check(v8 == 1, "DI value reads back as 1");

  u16 v16 = 0;
  check(s.readReg16(18, v16, err), "readReg16 succeeds");
  check(v16 == 812, "AI value reads back as 812");

  check(s.writeReg8(26, 1, err), "writeReg8 is acknowledged");
  check(fb.regs8[26] == 1, "the board stored the DO value");

  check(s.writeReg16(260, 2048, err), "writeReg16 is acknowledged");
  check(fb.regs16[260] == 2048, "the board stored the AO value");

  // Read back what we wrote, through the real code path.
  u16 back = 0;
  check(s.readReg16(260, back, err) && back == 2048, "AO value reads back after the write");

  // A value containing 0x55 must survive escaping end to end.
  check(s.writeReg16(0x55, 0x5555, err), "write with 0x55 in both reg and value");
  check(fb.regs16[0x55] == 0x5555, "0x55-laden write arrived intact");

  // ---- the faults that produced "Unexpected reply id 0 to DEVICE_GET_STATUS"

  std::printf("\nwith a transceiver echoing our own request back:\n");
  fb.echoRequest = true;
  DeviceStatus st2;
  check(s.identify(st2, err), "identify still succeeds through the echo");
  check(st2.cardId == MINI_GPIO_CARD_ID, "card id still 7 through the echo");
  u16 v2 = 0;
  check(s.readReg16(18, v2, err) && v2 == 812, "readReg16 still works through the echo");
  fb.echoRequest = false;

  std::printf("\nwith a stale ACK arriving before the real reply:\n");
  fb.sendStaleAck = true;
  DeviceStatus st3;
  check(s.identify(st3, err), "identify skips the stale ACK");
  check(st3.cardId == MINI_GPIO_CARD_ID, "card id read past the stale ACK");
  u8 v3 = 0;
  check(s.readReg8(89, v3, err) && v3 == 1, "readReg8 skips the stale ACK");
  fb.sendStaleAck = false;

  std::printf("\nwith a board that echoes but never answers:\n");
  fb.echoRequest = true;
  fb.goSilent    = true;
  s.setTimeout(200);
  DeviceStatus st4;
  check(!s.identify(st4, err), "identify fails when only the echo comes back");
  check(err.find("echoed") != std::string::npos, "the error names the echo as the cause");
  std::printf("      message: %s\n", err.c_str());
  fb.echoRequest = false;
  fb.goSilent    = false;
  s.setTimeout(1000);

  std::printf("\nafter a timeout, the next call must not read the late reply:\n");
  // Make one call time out while the board still answers it late, then check
  // that the following call gets its own answer and not the leftover.
  fb.goSilent = true;
  s.setTimeout(150);
  u16 ignored = 0;
  check(!s.readReg16(18, ignored, err), "the first call times out as expected");
  fb.goSilent = false;
  s.setTimeout(1000);
  u8 after = 0;
  check(s.readReg8(89, after, err) && after == 1,
        "the next call gets its own reply, not the leftover");

  fb.stop = true;
  link.close();
  if (fb.th.joinable()) fb.th.join();
}

int main() {
  CommGlobalInit();
  CodecTests();
  std::printf("\n");
  SessionTests();
  CommGlobalShutdown();
  std::printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "all checks passed", failures,
              failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}

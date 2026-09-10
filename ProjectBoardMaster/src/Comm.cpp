#include "Comm.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdarg>
#include <cstdio>

// ---------------------------------------------------------------- constants

static const u8 SYNC1     = 0x55;
static const u8 SYNC2     = 0x33;
static const u8 DATA_SYNC = 0xAA;

static const int I_AM_BOOT_LOADER             = 1 << 0;
static const int BLDR_STATUS_BAD_APP_CHECKSUM = 1 << 1;
static const int CARD_ID_BIT_POS              = 2;

// ---------------------------------------------------------------- Winsock

static bool g_wsaUp = false;

void CommGlobalInit() {
  if (g_wsaUp) return;
  WSADATA d;
  if (WSAStartup(MAKEWORD(2, 2), &d) == 0) g_wsaUp = true;
}

void CommGlobalShutdown() {
  if (!g_wsaUp) return;
  WSACleanup();
  g_wsaUp = false;
}

static std::string LastErrText(DWORD e) {
  char* msg = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, e, 0, (LPSTR)&msg, 0, nullptr);
  std::string out = msg ? msg : "unknown error";
  if (msg) LocalFree(msg);
  while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
  return out;
}

// ---------------------------------------------------------------- ComLink

bool ComLink::open(std::string& err) {
  close();
  if (m_port.empty()) {
    err = "No COM port set.";
    return false;
  }
  // \\.\ prefix is required for COM10 and above and harmless below it.
  std::string dev = "\\\\.\\" + m_port;

  HANDLE h = CreateFileA(dev.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED) {
      err = m_port + " is already open in another process (the Print Engine holds "
                     "it, or another BoardMaster instance).";
    } else {
      err = "Cannot open " + m_port + ": " + LastErrText(e);
    }
    return false;
  }

  DCB dcb = {};
  dcb.DCBlength = sizeof(dcb);
  if (!GetCommState(h, &dcb)) {
    err = "GetCommState failed on " + m_port + ": " + LastErrText(GetLastError());
    CloseHandle(h);
    return false;
  }
  dcb.BaudRate        = DWORD(m_baud);
  dcb.ByteSize        = 8;
  dcb.Parity          = NOPARITY;
  dcb.StopBits        = ONESTOPBIT;
  dcb.fBinary         = TRUE;
  dcb.fParity         = FALSE;
  dcb.fOutxCtsFlow    = FALSE;
  dcb.fOutxDsrFlow    = FALSE;
  dcb.fDtrControl     = DTR_CONTROL_DISABLE;
  dcb.fRtsControl     = RTS_CONTROL_DISABLE;
  dcb.fOutX           = FALSE;
  dcb.fInX            = FALSE;
  dcb.fAbortOnError   = FALSE;
  if (!SetCommState(h, &dcb)) {
    err = "SetCommState failed on " + m_port + ": " + LastErrText(GetLastError());
    CloseHandle(h);
    return false;
  }

  // Per-read timeouts are applied in read(); these are the defaults.
  COMMTIMEOUTS to = {};
  to.ReadIntervalTimeout        = 20;
  to.ReadTotalTimeoutConstant   = 200;
  to.ReadTotalTimeoutMultiplier = 0;
  to.WriteTotalTimeoutConstant  = 500;
  SetCommTimeouts(h, &to);

  PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
  m_h = h;
  err.clear();
  return true;
}

void ComLink::close() {
  if (m_h) {
    CloseHandle((HANDLE)m_h);
    m_h = nullptr;
  }
}

bool ComLink::write(const u8* data, size_t len, std::string& err) {
  if (!m_h) { err = "Port is not open."; return false; }
  DWORD written = 0;
  if (!WriteFile((HANDLE)m_h, data, DWORD(len), &written, nullptr) || written != len) {
    err = "Write to " + m_port + " failed: " + LastErrText(GetLastError());
    return false;
  }
  return true;
}

int ComLink::read(u8* buf, size_t len, int timeoutMs, std::string& err) {
  if (!m_h) { err = "Port is not open."; return -1; }

  COMMTIMEOUTS to = {};
  to.ReadIntervalTimeout        = 20;
  to.ReadTotalTimeoutConstant   = DWORD(timeoutMs);
  to.ReadTotalTimeoutMultiplier = 0;
  to.WriteTotalTimeoutConstant  = 500;
  SetCommTimeouts((HANDLE)m_h, &to);

  DWORD got = 0;
  if (!ReadFile((HANDLE)m_h, buf, DWORD(len), &got, nullptr)) {
    err = "Read from " + m_port + " failed: " + LastErrText(GetLastError());
    return -1;
  }
  return int(got);
}

// ---------------------------------------------------------------- TcpLink

bool TcpLink::open(std::string& err) {
  close();
  CommGlobalInit();

  addrinfo hints = {};
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo*   res = nullptr;
  std::string portStr = std::to_string(m_port);
  if (getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
    err = "Cannot resolve " + describe() + ".";
    return false;
  }

  SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s == INVALID_SOCKET) {
    freeaddrinfo(res);
    err = "socket() failed.";
    return false;
  }
  if (connect(s, res->ai_addr, int(res->ai_addrlen)) == SOCKET_ERROR) {
    int e = WSAGetLastError();
    closesocket(s);
    freeaddrinfo(res);
    err = "Cannot connect to " + describe() + " (WSA error " + std::to_string(e) +
          "). Is the board simulator running?";
    return false;
  }
  freeaddrinfo(res);

  BOOL nodelay = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

  m_sock = (unsigned long long)s;
  err.clear();
  return true;
}

void TcpLink::close() {
  if (m_sock != ~0ull) {
    closesocket((SOCKET)m_sock);
    m_sock = ~0ull;
  }
}

bool TcpLink::write(const u8* data, size_t len, std::string& err) {
  if (m_sock == ~0ull) { err = "Socket is not open."; return false; }
  size_t sent = 0;
  while (sent < len) {
    int n = send((SOCKET)m_sock, (const char*)data + sent, int(len - sent), 0);
    if (n <= 0) {
      err = "send() to " + describe() + " failed (WSA error " +
            std::to_string(WSAGetLastError()) + ").";
      return false;
    }
    sent += size_t(n);
  }
  return true;
}

int TcpLink::read(u8* buf, size_t len, int timeoutMs, std::string& err) {
  if (m_sock == ~0ull) { err = "Socket is not open."; return -1; }

  // SO_RCVTIMEO of 0 means "block forever", which is never what a caller asking
  // for a zero timeout wants.
  DWORD tv = DWORD(timeoutMs <= 0 ? 1 : timeoutMs);
  setsockopt((SOCKET)m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

  int n = recv((SOCKET)m_sock, (char*)buf, int(len), 0);
  if (n > 0) return n;
  if (n == 0) {
    err = describe() + " closed the connection.";
    return -1;
  }
  int e = WSAGetLastError();
  if (e == WSAETIMEDOUT) return 0;
  err = "recv() from " + describe() + " failed (WSA error " + std::to_string(e) + ").";
  return -1;
}

// ---------------------------------------------------------------- framing

std::vector<u8> EncodeFrame(const u8* payload, size_t len, u8 station) {
  std::vector<u8> out;
  out.reserve(len * 2 + 8);

  u8 sum = 0;
  auto put = [&out, &sum](u8 b) {
    sum = u8(sum + b);
    out.push_back(b);
    if (b == SYNC1) out.push_back(DATA_SYNC);   // escape
  };

  out.push_back(SYNC1);   // header bytes are raw and not summed
  out.push_back(SYNC2);

  put(station);
  put(u8(len & 0xFF));
  put(u8((len >> 8) & 0xFF));
  for (size_t i = 0; i < len; ++i) put(payload[i]);

  // The Print Engine writes the checksum unescaped; match that exactly.
  out.push_back(sum);
  return out;
}

void FrameDecoder::reset() {
  m_state       = State::Sync1;
  m_pendingSync = false;
  m_station     = 0;
  m_len         = 0;
  m_sum         = 0;
  m_payload.clear();
}

bool FrameDecoder::consume(u8 b) {
  switch (m_state) {
    case State::Sync1:
      // handled by feed()
      return false;

    case State::Sync2:
      return false;

    case State::Station:
      m_station = b;
      m_sum     = b;
      m_state   = State::Len1;
      return false;

    case State::Len1:
      m_len   = b;
      m_sum   = u8(m_sum + b);
      m_state = State::Len2;
      return false;

    case State::Len2:
      m_len   = u16(m_len | (u16(b) << 8));
      m_sum   = u8(m_sum + b);
      m_payload.clear();
      if (m_len == 0 || m_len > 4096) {
        // A zero or absurd length means we are not really in a frame.
        m_lastError = "bad frame length";
        m_state     = State::Sync1;
        return false;
      }
      m_payload.reserve(m_len);
      m_state = State::Payload;
      return false;

    case State::Payload:
      m_payload.push_back(b);
      m_sum = u8(m_sum + b);
      if (m_payload.size() >= m_len) m_state = State::Checksum;
      return false;

    case State::Checksum: {
      const bool good = (b == m_sum);
      m_state         = State::Sync1;
      if (!good) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "checksum mismatch (got 0x%02X, expected 0x%02X)",
                      b, m_sum);
        m_lastError = buf;
        return false;
      }
      m_lastError.clear();
      return true;
    }
  }
  return false;
}

bool FrameDecoder::feed(u8 raw) {
  // The checksum byte is transmitted unescaped - both the Print Engine
  // (AddFooterToOutgoingBuffer) and the board firmware (SendPacketFooter) write
  // it raw, and the Print Engine's receiver reads it raw in its rdChecksum
  // state. So a 0x55 arriving here is the checksum, never an escape lead-in.
  // Getting this wrong loses roughly one frame in 256.
  if (m_state == State::Checksum && !m_pendingSync) return consume(raw);

  // Escape handling: 0x55 0xAA is a literal 0x55, 0x55 0x33 restarts a frame.
  if (m_pendingSync) {
    m_pendingSync = false;
    if (raw == SYNC2) {
      // resynchronise: a fresh header just started
      m_state = State::Station;
      return false;
    }
    if (raw == DATA_SYNC) {
      return (m_state == State::Sync1) ? false : consume(SYNC1);
    }
    // Unexpected pair - treat the 0x55 as data, then fall through to this byte.
    if (m_state != State::Sync1) {
      if (consume(SYNC1)) return true;
    }
    // continue below with raw
  }

  if (raw == SYNC1) {
    m_pendingSync = true;
    return false;
  }

  if (m_state == State::Sync1) {
    // Waiting for a header; anything but 0x55 is noise between frames.
    return false;
  }

  return consume(raw);
}

std::string HexDump(const u8* data, size_t len, size_t maxBytes) {
  std::string out;
  const size_t n = (len < maxBytes) ? len : maxBytes;
  char         buf[8];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(buf, sizeof(buf), "%02X ", data[i]);
    out += buf;
  }
  if (len > n) out += "... ";
  return out;
}

// ---------------------------------------------------------------- Session

void Session::logLine(const char* dir, const std::string& text) {
  if (!m_log) return;
  m_log(m_tag + " " + dir + "  " + text);
}

static std::string FmtS(const char* fmt, ...) {
  char    buf[256];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

// Throws away whatever is already sitting in the receive buffer. A reply to a
// request that timed out earlier arrives late and would otherwise be read as
// the answer to this one, putting every later exchange one reply behind.
int Session::drainInput() {
  if (!m_link || !m_link->isOpen()) return 0;
  int dropped = 0;
  for (;;) {
    u8          junk[256];
    std::string derr;
    const int   n = m_link->read(junk, sizeof(junk), 1, derr);
    if (n <= 0) break;
    dropped += n;
    if (dropped > 8192) break;   // something is streaming at us; stop draining
  }
  if (dropped > 0) {
    logLine("--", FmtS("discarded %d stale byte(s) left in the receive buffer", dropped));
  }
  return dropped;
}

bool Session::transact(const std::vector<u8>& payload, u8 expectMsgId,
                       std::vector<u8>& reply, const char* what, std::string& err) {
  if (!m_link || !m_link->isOpen()) {
    err = "Board is not connected.";
    return false;
  }

  const std::vector<u8> frame = EncodeFrame(payload.data(), payload.size(), m_station);

  bool sawEcho    = false;
  bool sawOther   = false;
  u8   otherMsgId = 0;

  for (int attempt = 1; attempt <= 2; ++attempt) {
    drainInput();

    std::string werr;
    logLine("TX", HexDump(frame.data(), frame.size()) + "   " + what);
    if (!m_link->write(frame.data(), frame.size(), werr)) {
      err = werr;
      logLine("!!", werr);
      return false;
    }

    FrameDecoder dec;
    dec.reset();

    const DWORD     started = GetTickCount();
    std::vector<u8> rawAll;
    bool            done = false;
    bool            ok   = false;

    while (!done) {
      const DWORD elapsed = GetTickCount() - started;
      if (elapsed >= DWORD(m_timeoutMs)) break;

      u8          buf[256];
      std::string rerr;
      const int   n = m_link->read(buf, sizeof(buf), int(DWORD(m_timeoutMs) - elapsed), rerr);
      if (n < 0) {
        err = rerr;
        logLine("!!", rerr);
        return false;
      }
      if (n == 0) continue;

      rawAll.insert(rawAll.end(), buf, buf + n);

      for (int i = 0; i < n && !done; ++i) {
        if (!dec.feed(buf[size_t(i)])) continue;

        const std::vector<u8> got = dec.payload();
        dec.reset();

        // Our own request coming back: a half-duplex RS-422 transceiver or a
        // loopback adapter echoes the line. Skip it and keep listening.
        if (got == payload) {
          sawEcho = true;
          logLine("--", "ignored an echo of our own request");
          continue;
        }
        if (got.empty()) continue;

        if (got[0] == MSG_NAK) {
          const int reason = got.size() > 1 ? got[1] : -1;
          err = FmtS("Board replied NAK (reason %d) to %s.", reason, what);
          logLine("!!", err);
          return false;
        }
        if (got[0] != expectMsgId) {
          // Not ours - a stale reply or a different device on the line.
          sawOther   = true;
          otherMsgId = got[0];
          logLine("--", FmtS("ignored a frame with message id %u (waiting for %u)",
                             unsigned(got[0]), unsigned(expectMsgId)));
          continue;
        }

        reply = got;
        logLine("RX", HexDump(rawAll.data(), rawAll.size()) + "  ok  " +
                          std::to_string(reply.size()) + " bytes  " +
                          std::to_string(GetTickCount() - started) + " ms");
        ok   = true;
        done = true;
      }
    }

    if (ok) {
      err.clear();
      return true;
    }

    if (!rawAll.empty() && !dec.lastError().empty()) {
      logLine("--", "incomplete reply: " + dec.lastError());
    }
    if (attempt == 1) {
      logLine("--", "timeout after " + std::to_string(m_timeoutMs) + " ms, retry 1 of 1");
    }
  }

  // Say what was actually seen: the difference between silence, an echo and a
  // foreign device is the difference between three quite different faults.
  err = FmtS("No reply to %s after %d ms and one retry. ", what, m_timeoutMs);
  if (sawEcho) {
    err += "The port echoed our own request back and nothing else answered - the "
           "transceiver is echoing, or this is a loopback adapter, or no board is "
           "powered on the other end.";
  } else if (sawOther) {
    err += FmtS("Something on the port answered with message id %u instead - it is "
                "probably not a MiniGPIO, or another program is talking on the same "
                "line.", unsigned(otherMsgId));
  } else {
    err += "Nothing came back at all - check that the board is powered, that the port "
           "is the right one, and that it is wired for 115200 8-N-1.";
  }
  logLine("!!", err);
  return false;
}

bool Session::identify(DeviceStatus& out, std::string& err) {
  std::vector<u8> payload{MSG_DEVICE_GET_STATUS};
  std::vector<u8> reply;
  if (!transact(payload, MSG_HERE_IS_DEVICE_STATUS, reply, "DEVICE_GET_STATUS", err)) {
    return false;
  }
  if (reply.size() < 8) {
    err = "Short device-status reply (" + std::to_string(reply.size()) + " bytes).";
    return false;
  }
  const u8 flags = reply[1];
  out.cardId       = flags >> CARD_ID_BIT_POS;
  out.isBootloader = (flags & I_AM_BOOT_LOADER) != 0;
  out.badChecksum  = (flags & BLDR_STATUS_BAD_APP_CHECKSUM) != 0;
  out.pageSize     = int(reply[2] | (reply[3] << 8));
  out.major        = int(std::int16_t(reply[4] | (reply[5] << 8)));
  out.minor        = int(std::int16_t(reply[6] | (reply[7] << 8)));
  return true;
}

bool Session::readReg8(int regNum, u8& out, std::string& err) {
  std::vector<u8> payload{MSG_UC_REG8_READ, u8(regNum & 0xFF), u8((regNum >> 8) & 0xFF)};
  std::vector<u8> reply;
  char            what[64];
  std::snprintf(what, sizeof(what), "UC_REG8_READ reg %d (0x%X)", regNum, regNum);
  if (!transact(payload, MSG_HERE_IS_UC_REG8_VALUE, reply, what, err)) return false;
  if (reply.size() < 2) { err = "Short reg8 reply."; return false; }
  out = reply[1];
  return true;
}

bool Session::readReg16(int regNum, u16& out, std::string& err) {
  std::vector<u8> payload{MSG_UC_REG16_READ, u8(regNum & 0xFF), u8((regNum >> 8) & 0xFF)};
  std::vector<u8> reply;
  char            what[64];
  std::snprintf(what, sizeof(what), "UC_REG16_READ reg %d (0x%X)", regNum, regNum);
  if (!transact(payload, MSG_HERE_IS_UC_REG16_VALUE, reply, what, err)) return false;
  if (reply.size() < 3) { err = "Short reg16 reply."; return false; }
  out = u16(reply[1] | (u16(reply[2]) << 8));
  return true;
}

bool Session::writeReg8(int regNum, u8 value, std::string& err) {
  std::vector<u8> payload{MSG_UC_REG8_WRITE, u8(regNum & 0xFF), u8((regNum >> 8) & 0xFF),
                          value};
  std::vector<u8> reply;
  char            what[80];
  std::snprintf(what, sizeof(what), "UC_REG8_WRITE reg %d (0x%X) = %u", regNum, regNum,
                unsigned(value));
  return transact(payload, MSG_ACK, reply, what, err);
}

bool Session::writeReg16(int regNum, u16 value, std::string& err) {
  std::vector<u8> payload{MSG_UC_REG16_WRITE, u8(regNum & 0xFF), u8((regNum >> 8) & 0xFF),
                          u8(value & 0xFF), u8((value >> 8) & 0xFF)};
  std::vector<u8> reply;
  char            what[80];
  std::snprintf(what, sizeof(what), "UC_REG16_WRITE reg %d (0x%X) = %u", regNum, regNum,
                unsigned(value));
  return transact(payload, MSG_ACK, reply, what, err);
}

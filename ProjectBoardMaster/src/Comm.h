// Talking to a MiniGPIO board: the byte pipe, the sync framing, and the typed
// register operations layered on top.
//
// Frame layout (CSyncSerialProtocol in the Print Engine):
//
//   0x55 0x33 <station> <len lo> <len hi> <payload ...> <checksum>
//
//   * 0x55 inside station/length/payload is escaped as 0x55 0xAA
//   * 0x55 0x33 is a resynchronise marker and never data
//   * checksum is the 8-bit sum of station + length + payload, pre-escape,
//     and is written to the wire unescaped (matching the Print Engine)
//   * 115200 8-N-1, station 0xFF, one board per port
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using u8  = std::uint8_t;
using u16 = std::uint16_t;

// -------------------------------------------------------------- message ids

enum : u8 {
  // basic protocol
  MSG_DEVICE_GET_STATUS      = 0,
  MSG_ACK                    = 0,   // reply direction
  MSG_NAK                    = 1,
  MSG_HERE_IS_DEVICE_STATUS  = 2,

  // serial device protocol: SERIAL_DEV_MSG_START == 30
  MSG_UC_REG8_WRITE          = 41,
  MSG_UC_REG8_READ           = 42,
  MSG_UC_REG16_WRITE         = 44,
  MSG_UC_REG16_READ          = 45,

  // serial device replies: SERIAL_DEV_REPLY_START == 30
  MSG_HERE_IS_UC_REG8_VALUE  = 35,
  MSG_HERE_IS_UC_REG16_VALUE = 36,
};

const int MINI_GPIO_CARD_ID = 7;

// -------------------------------------------------------------- byte pipe

class ILink {
public:
  virtual ~ILink() = default;
  virtual bool open(std::string& err) = 0;
  virtual void close() = 0;
  virtual bool isOpen() const = 0;
  virtual bool write(const u8* data, size_t len, std::string& err) = 0;
  // Reads whatever is available, up to len, within timeoutMs. Returns the byte
  // count (0 on timeout) or -1 on error.
  virtual int  read(u8* buf, size_t len, int timeoutMs, std::string& err) = 0;
  virtual std::string describe() const = 0;
};

class ComLink : public ILink {
public:
  ComLink(const std::string& port, int baud) : m_port(port), m_baud(baud) {}
  ~ComLink() override { close(); }

  bool open(std::string& err) override;
  void close() override;
  bool isOpen() const override { return m_h != nullptr; }
  bool write(const u8* data, size_t len, std::string& err) override;
  int  read(u8* buf, size_t len, int timeoutMs, std::string& err) override;
  std::string describe() const override { return m_port; }

private:
  std::string m_port;
  int         m_baud;
  void*       m_h = nullptr;   // HANDLE
};

class TcpLink : public ILink {
public:
  TcpLink(const std::string& host, int port) : m_host(host), m_port(port) {}
  ~TcpLink() override { close(); }

  bool open(std::string& err) override;
  void close() override;
  bool isOpen() const override { return m_sock != ~0ull; }
  bool write(const u8* data, size_t len, std::string& err) override;
  int  read(u8* buf, size_t len, int timeoutMs, std::string& err) override;
  std::string describe() const override { return m_host + ":" + std::to_string(m_port); }

private:
  std::string        m_host;
  int                m_port;
  unsigned long long m_sock = ~0ull;   // SOCKET
};

// -------------------------------------------------------------- framing

std::vector<u8> EncodeFrame(const u8* payload, size_t len, u8 station);

class FrameDecoder {
public:
  void reset();
  // Feeds one raw byte. Returns true when a complete, checksum-valid frame is
  // available in payload().
  bool feed(u8 raw);
  const std::vector<u8>& payload() const { return m_payload; }
  const std::string&     lastError() const { return m_lastError; }

private:
  enum class State { Sync1, Sync2, Station, Len1, Len2, Payload, Checksum };

  State           m_state = State::Sync1;
  bool            m_pendingSync = false;
  u8              m_station = 0;
  u16             m_len = 0;
  u8              m_sum = 0;
  std::vector<u8> m_payload;
  std::string     m_lastError;

  bool consume(u8 b);   // b is already unescaped
};

std::string HexDump(const u8* data, size_t len, size_t maxBytes = 24);

// -------------------------------------------------------------- session

struct DeviceStatus {
  int  cardId       = -1;
  bool isBootloader = false;
  bool badChecksum  = false;
  int  pageSize     = 0;
  int  major        = 0;
  int  minor        = 0;
};

// One session per board. Every call is synchronous: send, wait for the reply,
// retry once on timeout or a bad checksum.
class Session {
public:
  using LogFn = std::function<void(const std::string& line)>;

  void setLink(ILink* link) { m_link = link; }   // not owned
  void setStation(u8 station) { m_station = station; }
  void setTimeout(int ms) { m_timeoutMs = ms; }
  void setLogger(LogFn fn) { m_log = std::move(fn); }
  void setTag(const std::string& tag) { m_tag = tag; }

  bool identify(DeviceStatus& out, std::string& err);
  bool readReg8(int regNum, u8& out, std::string& err);
  bool readReg16(int regNum, u16& out, std::string& err);
  bool writeReg8(int regNum, u8 value, std::string& err);
  bool writeReg16(int regNum, u16 value, std::string& err);

private:
  // Sends payload and waits for a reply whose first byte is expectMsgId,
  // skipping an echo of the request and any frame that is not the one we asked
  // for. A NAK reply is reported through err.
  bool transact(const std::vector<u8>& payload, u8 expectMsgId,
                std::vector<u8>& reply, const char* what, std::string& err);
  // Discards buffered input before a request; returns the byte count dropped.
  int  drainInput();
  void logLine(const char* dir, const std::string& text);

  ILink*      m_link      = nullptr;
  u8          m_station   = 0xFF;
  int         m_timeoutMs = 500;
  LogFn       m_log;
  std::string m_tag = "B1";
};

// Winsock needs one process-wide startup/cleanup.
void CommGlobalInit();
void CommGlobalShutdown();

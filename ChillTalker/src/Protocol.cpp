#include "Protocol.h"

#include <cstdio>

// ---------------------------------------------------------------- identifiers

static const IdInfo ID_TABLE[] = {
  // id         name                    kind              write  min    max
  { ID_PV,     "measured temperature", ValueKind::Tenths, false, -1999, 5000 },
  { ID_SV,     "target temperature",   ValueKind::Tenths, true,    100,  600 },
  { ID_OFFSET, "temperature offset",   ValueKind::Tenths, true,    -99,   99 },
  { ID_MODE,   "operation mode",       ValueKind::Mode,   true,      0,    2 },
  { ID_ALARM,  "alarm flags",          ValueKind::Alarm,  false,     0,  255 },
  { ID_STORE,  "store settings",       ValueKind::None,   true,      0,    0 },
};

const IdInfo* FindId(const std::string& id)
{
  for (const IdInfo& info : ID_TABLE) {
    if (id == info.id) return &info;
  }
  return nullptr;
}

// ---------------------------------------------------------------- building

u8 Bcc(const u8* data, size_t len)
{
  u8 x = 0;
  for (size_t i = 0; i < len; i++) x ^= data[i];
  return x;
}

Bytes BuildRequest(int address, char content, const std::string& id,
                   const std::string& data, bool bcc)
{
  char addr[16];
  std::snprintf(addr, sizeof addr, "%02d", address);

  Bytes f;
  f.push_back(STX);
  f.push_back(u8(addr[0]));
  f.push_back(u8(addr[1]));
  f.push_back(u8(content));
  f.insert(f.end(), id.begin(), id.end());
  f.insert(f.end(), data.begin(), data.end());
  f.push_back(ETX);
  if (bcc) f.push_back(Bcc(f.data(), f.size()));
  return f;
}

Bytes BuildRead(int address, const std::string& id, bool bcc)
{
  return BuildRequest(address, 'R', id, std::string(), bcc);
}

Bytes BuildWrite(int address, const std::string& id, const std::string& data, bool bcc)
{
  return BuildRequest(address, 'W', id, data, bcc);
}

Bytes BuildStore(int address, bool bcc)
{
  return BuildRequest(address, 'W', ID_STORE, std::string(), bcc);
}

std::string FormatData(int value)
{
  if (value < -9999 || value > 9999) return std::string();
  char buf[16];
  if (value < 0) std::snprintf(buf, sizeof buf, "-%04d", -value);
  else           std::snprintf(buf, sizeof buf, "%05d", value);
  return buf;
}

bool ParseData(const std::string& data, int& value)
{
  if (data.size() != 5) return false;
  if (data[0] != '0' && data[0] != '-') return false;
  int v = 0;
  for (size_t i = 1; i < 5; i++) {
    if (data[i] < '0' || data[i] > '9') return false;
    v = v * 10 + (data[i] - '0');
  }
  value = (data[0] == '-') ? -v : v;
  return true;
}

std::string FormatTenths(int tenths)
{
  int  a = tenths < 0 ? -tenths : tenths;
  char buf[24];
  std::snprintf(buf, sizeof buf, "%s%d.%d", tenths < 0 ? "-" : "", a / 10, a % 10);
  return buf;
}

// ---------------------------------------------------------------- reading

Message ParseFrame(const Bytes& f, bool bccEnabled)
{
  Message m;
  m.hasBcc = bccEnabled;

  const size_t tail = bccEnabled ? 2 : 1;   // ETX [BCC]
  if (f.size() < 1 + 2 + 1 + tail) { m.problem = "frame too short"; return m; }
  if (f[0] != STX)                 { m.problem = "does not start with STX"; return m; }
  const size_t etx = f.size() - tail;
  if (f[etx] != ETX)               { m.problem = bccEnabled ? "no ETX before the BCC byte"
                                                            : "no ETX at the end"; return m; }

  if (bccEnabled) {
    m.bccGot  = f.back();
    m.bccWant = Bcc(f.data(), etx + 1);
    m.bccOk   = m.bccGot == m.bccWant;
  }

  if (f[1] < '0' || f[1] > '9' || f[2] < '0' || f[2] > '9') {
    m.problem = "address is not two digits";
    return m;
  }
  m.address = (f[1] - '0') * 10 + (f[2] - '0');

  const std::string body(f.begin() + 3, f.begin() + long(etx));
  if (body.empty()) {
    m.problem = "nothing between the address and ETX";
  } else if (body[0] == 'R' && body.size() == 4) {
    m.type = MsgType::ReadRequest;
    m.id   = body.substr(1, 3);
  } else if (body[0] == 'W' && body.size() == 4 && body.compare(1, 3, ID_STORE) == 0) {
    m.type = MsgType::StoreRequest;
    m.id   = ID_STORE;
  } else if (body[0] == 'W' && body.size() == 9) {
    m.type = MsgType::WriteRequest;
    m.id   = body.substr(1, 3);
    m.data = body.substr(4, 5);
  } else if (u8(body[0]) == ACK && body.size() == 1) {
    m.type = MsgType::Ack;
  } else if (u8(body[0]) == ACK && body.size() == 9) {
    m.type = MsgType::AckData;
    m.id   = body.substr(1, 3);
    m.data = body.substr(4, 5);
  } else if (u8(body[0]) == NAK && body.size() == 2) {
    m.type  = MsgType::Nak;
    m.error = (body[1] >= '0' && body[1] <= '9') ? body[1] - '0' : -1;
  } else {
    m.problem = "not a message this protocol defines";
  }
  return m;
}

bool IsReply(const Message& m)
{
  return m.type == MsgType::Ack || m.type == MsgType::AckData || m.type == MsgType::Nak;
}

static std::string ByteText(u8 b)
{
  switch (b) {
    case STX: return "<STX>";
    case ETX: return "<ETX>";
    case ACK: return "<ACK>";
    case NAK: return "<NAK>";
    case ' ': return "_";
  }
  if (b > ' ' && b < 0x7F) return std::string(1, char(b));
  char buf[8];
  std::snprintf(buf, sizeof buf, "<%02X>", b);
  return buf;
}

std::string Readable(const Bytes& bytes, bool hasBcc)
{
  std::string s;
  const size_t n = (hasBcc && !bytes.empty()) ? bytes.size() - 1 : bytes.size();
  for (size_t i = 0; i < n; i++) s += ByteText(bytes[i]);
  if (n < bytes.size()) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "[%02X]", bytes.back());
    s += buf;
  }
  return s;
}

std::string ErrorText(int code)
{
  switch (code) {
    case 0:  return "memory error or controller failure";
    case 1:  return "numeric data out of the range of this item";
    case 2:  return "no such item (unknown identifier)";
    case 3:  return "numeric data is not numeric, or the sign position is not 0 or -";
    case 4:  return "format error";
    case 5:  return "BCC error";
    case 6:  return "overrun error";
    case 7:  return "framing error";
    case 8:  return "parity error";
    default: return "unknown error code";
  }
}

std::string AlarmText(int flags)
{
  static const char* const NAMES[8] = {
    "AL0 memory error",
    "AL1 controller error",
    "AL2 temperature sensor disconnection",
    "AL3 temperature sensor short circuit",
    "AL4 high temperature",
    "AL5 low temperature",
    "AL6 low level",
    "AL7 thermostat",
  };
  if (flags == 0) return "no alarm";
  std::string s;
  for (int bit = 0; bit < 8; bit++) {
    if (!(flags & (1 << bit))) continue;
    if (!s.empty()) s += ", ";
    s += NAMES[bit];
  }
  if (flags >> 8) {
    if (!s.empty()) s += ", ";
    s += "unknown flags";
  }
  return s;
}

// "_" for a space, the way the manual writes identifiers.
static std::string IdText(const std::string& id)
{
  std::string s = id;
  for (char& c : s) {
    if (c == ' ') c = '_';
  }
  return s;
}

static std::string IdLabel(const std::string& id)
{
  const IdInfo* info = FindId(id);
  if (!info) return IdText(id) + " (not in the manual)";
  return IdText(id) + " (" + info->name + ")";
}

static std::string ValueText(const std::string& id, const std::string& data)
{
  int v = 0;
  if (!ParseData(data, v)) return "'" + data + "' (not a number)";

  const IdInfo* info = FindId(id);
  const ValueKind kind = info ? info->kind : ValueKind::None;
  switch (kind) {
    case ValueKind::Tenths:
      return FormatTenths(v) + " degC";
    case ValueKind::Mode:
      if (v == MODE_RUN)  return "RUN";
      if (v == MODE_STOP) return "STOP (RDY)";
      return data + " (unknown mode)";
    case ValueKind::Alarm:
      return data + ": " + AlarmText(v);
    default:
      return data;
  }
}

std::string Describe(const Message& m)
{
  std::string s;
  char        who[16];
  std::snprintf(who, sizeof who, "%02d", m.address);

  switch (m.type) {
    case MsgType::ReadRequest:
      s = "read " + IdLabel(m.id) + " from " + who;
      break;
    case MsgType::WriteRequest:
      s = "write " + IdLabel(m.id) + " = " + ValueText(m.id, m.data) + " to " + who;
      break;
    case MsgType::StoreRequest:
      s = std::string("store the set values to EEPROM, address ") + who;
      break;
    case MsgType::Ack:
      s = std::string("ACK from ") + who;
      break;
    case MsgType::AckData:
      s = std::string("ACK from ") + who + ": " + IdLabel(m.id) + " = " + ValueText(m.id, m.data);
      break;
    case MsgType::Nak:
      s = std::string("NAK from ") + who + ": error " + std::to_string(m.error) + " - " +
          ErrorText(m.error);
      break;
    default:
      return "cannot decode: " + m.problem;
  }

  if (m.hasBcc && !m.bccOk) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "  [BCC MISMATCH: got %02X, expected %02X]", m.bccGot, m.bccWant);
    s += buf;
  }
  return s;
}

// ---------------------------------------------------------------- framing

void FrameAssembler::reset()
{
  m_state = State::Idle;
  m_buf.clear();
}

void FrameAssembler::feed(const u8* data, size_t len, std::vector<Chunk>& out)
{
  for (size_t i = 0; i < len; i++) {
    const u8 b = data[i];
    switch (m_state) {
      case State::Idle:
        if (b == STX) {
          flush(out);                 // whatever came before was stray
          m_buf.push_back(b);
          m_state = State::Body;
        } else {
          m_buf.push_back(b);
        }
        break;

      case State::Body:
        if (b == STX) {               // a new frame while one was open: that one was cut short
          flush(out);
          m_buf.push_back(b);
          m_state = State::Body;
          break;
        }
        m_buf.push_back(b);
        if (b == ETX) {
          if (m_bcc) {
            m_state = State::Bcc;
          } else {
            out.push_back({ Chunk::Frame, m_buf });
            reset();
          }
        }
        break;

      case State::Bcc:                // any value is legal here, even STX or ETX
        m_buf.push_back(b);
        out.push_back({ Chunk::Frame, m_buf });
        reset();
        break;
    }
  }
}

void FrameAssembler::flush(std::vector<Chunk>& out)
{
  if (m_buf.empty()) return;
  out.push_back({ m_state == State::Idle ? Chunk::Stray : Chunk::Partial, m_buf });
  reset();
}

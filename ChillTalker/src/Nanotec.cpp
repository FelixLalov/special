#include "Nanotec.h"

#include <cstdio>

namespace nanotec {

// ---------------------------------------------------------------- the objects

static const Object* const OBJECTS[] = {
  &OD_CONTROL_WORD, &OD_STATUS_WORD,  &OD_MODE,       &OD_VELOCITY_ACTUAL,
  &OD_ACCELERATION, &OD_DECELERATION, &OD_QUICK_STOP, &OD_TARGET_VELOCITY,
};

const Object* FindObject(u16 index, u8 sub)
{
  for (const Object* o : OBJECTS) {
    if (o->index == index && o->sub == sub) return o;
  }
  return nullptr;
}

// The status word patterns of the manual's CiA 402 page; x is a bit that does not matter.
State StateOf(u16 sw)
{
  if ((sw & 0x4F) == 0x00) return State::NotReady;           // x0xx 0000
  if ((sw & 0x4F) == 0x40) return State::SwitchOnDisabled;   // x1xx 0000
  if ((sw & 0x6F) == 0x21) return State::ReadyToSwitchOn;    // x01x 0001
  if ((sw & 0x6F) == 0x23) return State::SwitchedOn;         // x01x 0011
  if ((sw & 0x6F) == 0x27) return State::OperationEnabled;   // x01x 0111
  if ((sw & 0x6F) == 0x07) return State::QuickStopActive;    // x00x 0111
  if ((sw & 0x4F) == 0x0F) return State::FaultReaction;      // x0xx 1111
  if ((sw & 0x4F) == 0x08) return State::Fault;              // x0xx 1000
  return State::Unknown;
}

const char* StateText(State s)
{
  switch (s) {
    case State::NotReady:         return "not ready to switch on";
    case State::SwitchOnDisabled: return "switch on disabled";
    case State::ReadyToSwitchOn:  return "ready to switch on";
    case State::SwitchedOn:       return "switched on";
    case State::OperationEnabled: return "operation enabled";
    case State::QuickStopActive:  return "quick stop active";
    case State::FaultReaction:    return "fault reaction active";
    case State::Fault:            return "fault";
    default:                      return "unknown state";
  }
}

// ---------------------------------------------------------------- building

static u16 CrcStep(u16 crc, u8 b)
{
  crc = u16(crc ^ b);
  for (int bit = 0; bit < 8; bit++) crc = (crc & 1) ? u16((crc >> 1) ^ 0xA001) : u16(crc >> 1);
  return crc;
}

u16 Crc16(const u8* data, size_t len)
{
  u16 crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) crc = CrcStep(crc, data[i]);
  return crc;
}

// Appends the CRC, low byte first.
static Bytes Sealed(Bytes f)
{
  const u16 crc = Crc16(f.data(), f.size());
  f.push_back(u8(crc & 0xFF));
  f.push_back(u8(crc >> 8));
  return f;
}

Bytes BuildReportId(int slave)
{
  return Sealed({ u8(slave), FN_REPORT_ID });
}

static Bytes ObjectFrame(int slave, bool write, const Object& o, int size, const Bytes& data)
{
  Bytes f = { u8(slave), FN_OBJECT, MEI_CANOPEN, u8(write ? 0x01 : 0x00), 0x00, NODE_ID,
              u8(o.index >> 8), u8(o.index & 0xFF), o.sub, 0x00, 0x00, u8(size >> 8), u8(size & 0xFF) };
  f.insert(f.end(), data.begin(), data.end());
  return Sealed(f);
}

Bytes BuildReadObject(int slave, const Object& o)
{
  return ObjectFrame(slave, false, o, o.size, Bytes());
}

Bytes BuildWriteObject(int slave, const Object& o, long long value)
{
  Bytes data;
  for (int i = 0; i < o.size; i++) data.push_back(u8((value >> (8 * i)) & 0xFF));
  return ObjectFrame(slave, true, o, o.size, data);
}

// ---------------------------------------------------------------- reading

Frame Parse(const Bytes& f)
{
  Frame m;
  if (f.size() < 4) {
    m.problem = "frame too short";
    return m;
  }
  m.slave    = f[0];
  m.function = f[1];

  const size_t n = f.size() - 2;   // the frame without its CRC
  if (Crc16(f.data(), n) != u16(f[n] | (f[n + 1] << 8))) {
    m.problem = "CRC mismatch";
    return m;
  }

  if (m.function & EXCEPTION_FLAG) {
    if (m.function == (FN_OBJECT | EXCEPTION_FLAG) && n >= 7 && f[2] == 0xFF && f[5] == MEI_CANOPEN) {
      m.kind      = Kind::ObjectException;
      m.exception = f[6];
      if (f[6] == 0xCE && n >= 11) {
        m.abort = u32(f[7]) | u32(f[8]) << 8 | u32(f[9]) << 16 | u32(f[10]) << 24;
      }
    } else if (n == 3) {
      m.kind      = Kind::Exception;
      m.exception = f[2];
    } else {
      m.problem = "an error reply in a layout this tool does not know";
    }
    return m;
  }

  if (m.function == FN_REPORT_ID) {
    if (n == 2) {
      m.kind = Kind::ReportIdRequest;
    } else if (n >= 3 && size_t(f[2]) == n - 3) {
      m.kind = Kind::ReportIdReply;
      m.data.assign(f.begin() + 3, f.begin() + long(n));
    } else {
      m.problem = "report server id frame of an unexpected length";
    }
    return m;
  }

  if (m.function == FN_OBJECT) {
    if (n < 13 || f[2] != MEI_CANOPEN) {
      m.problem = "not an object access frame";
      return m;
    }
    const bool write = (f[3] & 0x01) != 0;   // the access flag is the last bit of the protocol control byte
    m.index = u16(f[6] << 8 | f[7]);
    m.sub   = f[8];
    m.size  = f[11] << 8 | f[12];
    m.data.assign(f.begin() + 13, f.begin() + long(n));
    if (write) m.kind = m.data.empty() ? Kind::WriteReply : Kind::WriteRequest;
    else       m.kind = m.data.empty() ? Kind::ReadRequest : Kind::ReadReply;
    return m;
  }

  m.problem = "a function this tool does not use";
  return m;
}

bool Answers(const Frame& reply, const Frame& request)
{
  if (reply.slave != request.slave) return false;
  switch (reply.kind) {
    case Kind::Exception:
      return reply.function == (request.function | EXCEPTION_FLAG);
    case Kind::ObjectException:
      return request.function == FN_OBJECT;
    case Kind::ReportIdReply:
      return request.kind == Kind::ReportIdRequest;
    case Kind::ReadReply:
      return request.kind == Kind::ReadRequest && reply.index == request.index && reply.sub == request.sub;
    case Kind::WriteReply:
      return request.kind == Kind::WriteRequest && reply.index == request.index && reply.sub == request.sub;
    default:
      return false;
  }
}

bool IsOk(const Frame& f)
{
  return f.kind == Kind::ReportIdReply || f.kind == Kind::ReadReply || f.kind == Kind::WriteReply;
}

// The little endian value of d, sign extended when o is signed. d holds o.size bytes.
static long long Decode(const Object& o, const Bytes& d)
{
  unsigned long long v = 0;
  for (int i = 0; i < o.size; i++) v |= (unsigned long long)d[size_t(i)] << (8 * i);
  if (o.isSigned && (d[size_t(o.size) - 1] & 0x80)) v |= ~0ULL << (8 * o.size);
  return (long long)v;
}

bool ValueOf(const Frame& f, const Object& o, long long& value)
{
  if (int(f.data.size()) != o.size) return false;
  value = Decode(o, f.data);
  return true;
}

static std::string Hex(const Bytes& b)
{
  std::string s;
  char        buf[8];
  for (size_t i = 0; i < b.size(); i++) {
    std::snprintf(buf, sizeof buf, i ? " %02X" : "%02X", b[i]);
    s += buf;
  }
  return s;
}

static const char* ControlText(long long cw)
{
  switch (cw) {
    case 0x00:         return "disable voltage";
    case CW_SHUTDOWN:  return "shutdown";
    case CW_SWITCH_ON: return "switch on";
    case CW_ENABLE:    return "enable operation";
    case 0x80:         return "fault reset";
    default:           return nullptr;
  }
}

static const char* ModeText(long long mode)
{
  switch (mode) {
    case 0:  return "no mode";
    case 1:  return "profile position";
    case 2:  return "velocity";
    case 3:  return "profile velocity";
    case 4:  return "profile torque";
    case 6:  return "homing";
    default: return nullptr;
  }
}

static std::string ValueText(const Object& o, long long v)
{
  char buf[96];
  switch (o.fmt) {
    case Fmt::Rpm:
      std::snprintf(buf, sizeof buf, "%lld rpm", v);
      break;
    case Fmt::ControlWord: {
      const char* t = ControlText(v);
      if (t) std::snprintf(buf, sizeof buf, "0x%04llX (%s)", (unsigned long long)v, t);
      else   std::snprintf(buf, sizeof buf, "0x%04llX", (unsigned long long)v);
      break;
    }
    case Fmt::StatusWord: {
      const State s = StateOf(u16(v));
      std::snprintf(buf, sizeof buf, "0x%04llX (%s%s)", (unsigned long long)v, StateText(s),
                    (s == State::OperationEnabled && (v & 0x0400)) ? ", target reached" : "");
      break;
    }
    case Fmt::Mode: {
      const char* t = ModeText(v);
      if (t) std::snprintf(buf, sizeof buf, "%lld (%s)", v, t);
      else   std::snprintf(buf, sizeof buf, "%lld", v);
      break;
    }
    default:
      std::snprintf(buf, sizeof buf, "%lld", v);
  }
  return buf;
}

static std::string DataText(const Object* o, const Bytes& d)
{
  if (o && int(d.size()) == o->size) return ValueText(*o, Decode(*o, d));
  return "bytes " + Hex(d);
}

std::string ExceptionText(int code)
{
  switch (code) {
    case 1:  return "illegal function (the request is not supported)";
    case 2:  return "illegal data address";
    case 3:  return "illegal data value";
    case 4:  return "device failure";
    default: return "unknown exception code";
  }
}

std::string AbortText(u32 code)
{
  switch (code) {
    // the CANopen (CiA 301) codes a drive sends
    case 0x06010001: return "attempt to read a write-only object";
    case 0x06010002: return "attempt to write a read-only object";
    case 0x06020000: return "the object does not exist";
    case 0x06070010: return "data type or length does not match the object";
    case 0x06070012: return "data too long for the object";
    case 0x06070013: return "data too short for the object";
    case 0x06090011: return "the sub-index does not exist";
    case 0x06090030: return "value outside the range of the object";
    case 0x08000020: return "data cannot be transferred or stored";
    case 0x08000022: return "data cannot be transferred or stored in the present device state";
    // the manual's own table
    case 0xFFFF1003: return "service not supported";
    case 0xFFFF1004: return "gap in the counter byte, or invalid message sequence";
    case 0xFFFF0003: return "unknown or invalid command";
    case 0xFFFF0008: return "access to the object not supported";
    case 0xFFFF000E: return "general parameter error";
    case 0xFFFF0011: return "parameter length incorrect";
    case 0xFFFF0012: return "parameter too long";
    case 0xFFFF0013: return "parameter too short";
    case 0xFFFF0015: return "parameter outside the permissible range";
    case 0xFFFF0016: return "parameter above the permissible range";
    case 0xFFFF0017: return "parameter below the permissible range";
    case 0xFFFF0018: return "maximum less than minimum";
    case 0xFFFF0019: return "general error";
    case 0xFFFF001E: return "object too large for one message";
    default:         return "unknown abort code";
  }
}

std::string Describe(const Frame& f)
{
  char who[24];
  std::snprintf(who, sizeof who, "slave %d", f.slave);

  const Object* o = FindObject(f.index, f.sub);
  char          where[16];
  std::snprintf(where, sizeof where, "%04X:%02X", unsigned(f.index), unsigned(f.sub));
  const std::string object = std::string(where) + (o ? std::string(" ") + o->name : std::string(" (unknown object)"));

  char code[16];
  switch (f.kind) {
    case Kind::ReportIdRequest:
      return std::string("report server id from ") + who;

    case Kind::ReportIdReply: {
      std::string s = std::string("answer from ") + who + ": server id data " + Hex(f.data);
      if (f.data.size() >= 2 && (f.data[1] == 0x00 || f.data[1] == 0xFF)) {
        s += f.data[1] ? ", run indicator ON" : ", run indicator OFF";
      }
      return s;
    }

    case Kind::ReadRequest:
      return "read " + object + " (" + std::to_string(f.size) + " bytes) from " + who;

    case Kind::ReadReply:
      return std::string("answer from ") + who + ": " + object + " = " + DataText(o, f.data);

    case Kind::WriteRequest:
      return "write " + object + " = " + DataText(o, f.data) + " to " + who;

    case Kind::WriteReply:
      return std::string("answer from ") + who + ": " + object + " written";

    case Kind::Exception:
      std::snprintf(code, sizeof code, "%02X", f.exception);
      return std::string("exception from ") + who + ": " + code + " - " + ExceptionText(f.exception);

    case Kind::ObjectException:
      if (f.exception == 0xCE) {
        std::snprintf(code, sizeof code, "%08X", unsigned(f.abort));
        return std::string(who) + " refuses the access: abort code " + code + " - " + AbortText(f.abort);
      }
      if (f.exception == 0xAE) return std::string(who) + " does not support the protocol control bits of the request";
      std::snprintf(code, sizeof code, "%02X", f.exception);
      return std::string("error reply from ") + who + ", exception code " + code;

    default:
      return "cannot decode: " + f.problem;
  }
}

// ---------------------------------------------------------------- framing

// A Modbus RTU frame is 256 bytes at most.
static const size_t MAX_FRAME = 256;

// The length of the shortest frame at the start of p whose CRC is right, or 0. With the CRC
// appended low byte first, the CRC over the whole frame is zero.
static size_t FrameLength(const u8* p, size_t len)
{
  u16 crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc = CrcStep(crc, p[i]);
    if (i >= 3 && crc == 0) return i + 1;
  }
  return 0;
}

void Assembler::feed(const u8* data, size_t len, std::vector<Chunk>& out)
{
  m_buf.insert(m_buf.end(), data, data + len);
  for (;;) {
    const size_t n = FrameLength(m_buf.data(), m_buf.size());
    if (n == 0) break;
    out.push_back({ Chunk::Frame, Bytes(m_buf.begin(), m_buf.begin() + long(n)) });
    m_buf.erase(m_buf.begin(), m_buf.begin() + long(n));
  }
  if (m_buf.size() > MAX_FRAME) flush(out);   // noise that never makes a frame
}

void Assembler::flush(std::vector<Chunk>& out)
{
  size_t done = 0;   // everything before this is handed over
  size_t at   = 0;   // where the next frame may start
  while (at < m_buf.size()) {
    const size_t n = FrameLength(m_buf.data() + at, m_buf.size() - at);
    if (n == 0) {
      at++;
      continue;
    }
    if (at > done) out.push_back({ Chunk::Stray, Bytes(m_buf.begin() + long(done), m_buf.begin() + long(at)) });
    out.push_back({ Chunk::Frame, Bytes(m_buf.begin() + long(at), m_buf.begin() + long(at + n)) });
    at += n;
    done = at;
  }
  if (done < m_buf.size()) out.push_back({ Chunk::Stray, Bytes(m_buf.begin() + long(done), m_buf.end()) });
  m_buf.clear();
}

}  // namespace nanotec

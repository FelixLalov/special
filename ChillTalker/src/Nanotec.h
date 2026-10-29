// The Nanotec PD1-C motor drives that share the chiller's RS-485 bus: Modbus RTU frames, and
// the few entries of the drive's object dictionary that turning a motor takes.
//
// Sources: Nanotec's PD1-C Modbus RTU manual and the N5 one (V1.5.0, chapter 8.6), whose
// worked examples the self-check in tests\ compares byte for byte. MetalJet's
// NanotecDevices\NanotecLib.cpp sends the same frames.
//
//   every frame   <slave:1> <function:1> <data ...> <CRC:2>        CRC-16/MODBUS, low byte first
//   report id     <slave> 11 CRC                                   reply <slave> 11 <n> <n bytes> CRC
//   read object   <slave> 2B 0D 00 00 01 <index:2> <sub:1> 00 00 <size:2> CRC
//   write object  <slave> 2B 0D 01 00 01 <index:2> <sub:1> 00 00 <size:2> <data> CRC
//                 the reply has the same header: a read reply carries the data, a write reply has size 0
//   object error  <slave> AB FF 00 06 0D CE <abort code:4> CRC
//   plain error   <slave> <function + 80> <exception:1> CRC
//
//   * 2B 0D is Modbus "encapsulated interface transport", MEI type "CANopen general
//     reference": it reads or writes one entry of the drive's object dictionary. After 0D come
//     the protocol control byte (its last bit is the access flag, 1 = write), a reserved 00 and
//     the node id 01
//   * index and size are big endian, the data and the abort code little endian
//   * slave ids run 1..247 and a PD1-C ships as 5. Its line is 19200 baud, 8 data bits, even
//     parity, 1 stop bit, which is how MetalJet opens the drives
//   * like the chiller, a drive never speaks first and ignores a frame with a bad CRC
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Protocol.h"   // u8, Bytes

using u16 = std::uint16_t;
using u32 = std::uint32_t;

namespace nanotec {

const int MIN_SLAVE = 1;
const int MAX_SLAVE = 247;

const u8 FN_REPORT_ID   = 0x11;
const u8 FN_OBJECT      = 0x2B;   // encapsulated interface transport
const u8 MEI_CANOPEN    = 0x0D;
const u8 NODE_ID        = 0x01;
const u8 EXCEPTION_FLAG = 0x80;   // added to the function code of an error reply

// -------------------------------------------------------------- the objects used

enum class Fmt { Number, Rpm, ControlWord, StatusWord, Mode };

struct Object
{
  u16         index;
  u8          sub;
  const char* name;
  int         size;       // bytes
  bool        isSigned;
  Fmt         fmt;
};

inline constexpr Object OD_CONTROL_WORD    = { 0x6040, 0, "control word",            2, false, Fmt::ControlWord };
inline constexpr Object OD_STATUS_WORD     = { 0x6041, 0, "status word",             2, false, Fmt::StatusWord };
inline constexpr Object OD_MODE            = { 0x6060, 0, "mode of operation",       1, true,  Fmt::Mode };
inline constexpr Object OD_VELOCITY_ACTUAL = { 0x606C, 0, "velocity actual value",   4, true,  Fmt::Rpm };
inline constexpr Object OD_ACCELERATION    = { 0x6083, 0, "profile acceleration",    4, false, Fmt::Number };
inline constexpr Object OD_DECELERATION    = { 0x6084, 0, "profile deceleration",    4, false, Fmt::Number };
inline constexpr Object OD_QUICK_STOP      = { 0x6085, 0, "quick stop deceleration", 4, false, Fmt::Number };
inline constexpr Object OD_TARGET_VELOCITY = { 0x60FF, 0, "target velocity",         4, true,  Fmt::Rpm };

// Returns nullptr for an object this tool does not know.
const Object* FindObject(u16 index, u8 sub);

const int MODE_PROFILE_VELOCITY = 3;

// The control word values of the CiA 402 power state machine that turning a drive needs:
// 06 then 07 then 0F enables it from any state but a fault, 06 alone switches the power stage off.
const int CW_SHUTDOWN  = 0x0006;
const int CW_SWITCH_ON = 0x0007;
const int CW_ENABLE    = 0x000F;

enum class State
{
  NotReady, SwitchOnDisabled, ReadyToSwitchOn, SwitchedOn, OperationEnabled,
  QuickStopActive, FaultReaction, Fault, Unknown
};

State       StateOf(u16 statusWord);
const char* StateText(State s);

// -------------------------------------------------------------- building

u16 Crc16(const u8* data, size_t len);

Bytes BuildReportId(int slave);
// Asks for o.size bytes.
Bytes BuildReadObject(int slave, const Object& o);
// The value goes out little endian in o.size bytes.
Bytes BuildWriteObject(int slave, const Object& o, long long value);

// -------------------------------------------------------------- reading

enum class Kind
{
  Unknown, ReportIdRequest, ReportIdReply, ReadRequest, ReadReply, WriteRequest, WriteReply,
  Exception,         // a plain Modbus error reply
  ObjectException    // an error reply to an object access
};

struct Frame
{
  Kind        kind      = Kind::Unknown;
  int         slave     = -1;
  u8          function  = 0;
  u16         index     = 0;    // object frames
  u8          sub       = 0;
  int         size      = 0;    // the size field of an object frame
  Bytes       data;             // object data, or what a report id reply carries
  u8          exception = 0;    // Exception: 1..4. ObjectException: CE (abort code follows) or AE
  u32         abort     = 0;    // ObjectException: the CANopen abort code
  std::string problem;          // empty when the frame decoded
};

// Parses one complete frame, address through CRC. Requests and replies are both understood,
// so it also decodes what we send.
Frame Parse(const Bytes& frame);

// True when reply is the answer to request: the same slave, and either the same service or
// an error reply to it.
bool Answers(const Frame& reply, const Frame& request);

// True for a real answer, false for an error reply.
bool IsOk(const Frame& f);

// The data of a read reply as a number. False when the data is not o.size bytes.
bool ValueOf(const Frame& f, const Object& o, long long& value);

// One line of plain English for a parsed frame.
std::string Describe(const Frame& f);

std::string ExceptionText(int code);
std::string AbortText(u32 code);

// -------------------------------------------------------------- framing

// Cuts the byte stream into frames. A Modbus RTU frame has no length and no end mark, so a
// frame is taken to end at the first byte after which its CRC is right. Nothing is lost:
// bytes that make no frame come back as Stray, when flush() is told that the line went quiet.
class Assembler
{
public:
  struct Chunk
  {
    enum Type { Frame, Stray };
    Type  type;
    Bytes bytes;
  };

  void reset()
  {
    m_buf.clear();
  }

  void feed(const u8* data, size_t len, std::vector<Chunk>& out);

  // True while bytes are held back, waiting for more.
  bool pending() const
  {
    return !m_buf.empty();
  }

  // Hands back what is being held: a frame hiding behind noise comes out as a Frame, the
  // rest as Stray.
  void flush(std::vector<Chunk>& out);

private:
  Bytes m_buf;
};

}  // namespace nanotec

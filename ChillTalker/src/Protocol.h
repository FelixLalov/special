// SMC Thermo-con HEF002 serial protocol (Serial Communication Manual, DOC1009224-1).
//
// This is not Modbus. It is a plain ASCII request / reply protocol:
//
//   read         STX <addr:2> R <id:3>              ETX [BCC]
//   write        STX <addr:2> W <id:3> <data:5>     ETX [BCC]
//   store        STX <addr:2> W S T R               ETX [BCC]
//   read reply   STX <addr:2> ACK <id:3> <data:5>   ETX [BCC]
//   other reply  STX <addr:2> ACK                   ETX [BCC]
//   error reply  STX <addr:2> NAK <error:1>         ETX [BCC]
//
//   * STX 02h, ETX 03h, ACK 06h, NAK 15h; everything else is printable ASCII
//   * <addr> is two digits, 01..99. The chiller ignores requests for other addresses
//   * <data> is five characters with an implied decimal point: 00250 is 25.0 degC.
//     A negative value carries '-' in the first position: -0099 is -9.9 degC
//   * BCC is the XOR of every byte from STX to ETX inclusive. It is on the wire only
//     while "BCC check" is enabled on the chiller. The manual says so for the
//     replies; the requests are assumed to follow the same switch
//   * the chiller never speaks first, and says nothing at all to a request it cannot
//     frame (no STX / ETX), so a missing reply is a timeout, not an error message
#pragma once

#include <cstdint>
#include <string>
#include <vector>

using u8    = std::uint8_t;
using Bytes = std::vector<u8>;

// -------------------------------------------------------------- constants

enum : u8 { STX = 0x02, ETX = 0x03, ACK = 0x06, NAK = 0x15 };

// Identifiers (Table 3-2). The manual writes a leading space as "_".
const char* const ID_PV     = "PV1";   // measured temperature, read only
const char* const ID_SV     = "SV1";   // target temperature
const char* const ID_OFFSET = "PVS";   // temperature offset
const char* const ID_STORE  = "STR";   // store the set values to EEPROM (write only, no data)
const char* const ID_MODE   = " MD";   // operation mode
const char* const ID_ALARM  = " AL";   // alarm flags, read only

const int MODE_RUN  = 0;
const int MODE_STOP = 2;   // STOP (RDY)

const int MIN_ADDRESS = 1;
const int MAX_ADDRESS = 99;

enum class ValueKind { None, Tenths, Mode, Alarm };

struct IdInfo
{
  const char* id;
  const char* name;
  ValueKind   kind;
  bool        canWrite;
  int         minValue;   // documented range, in the same units as the data (Tenths only)
  int         maxValue;
};

// Returns nullptr for an identifier the manual does not list.
const IdInfo* FindId(const std::string& id);

// -------------------------------------------------------------- building

u8 Bcc(const u8* data, size_t len);

// content is 'R' or 'W'; data is empty for a read and for the store request. The
// address must be 0..99. With bcc the BCC byte is appended after ETX.
Bytes BuildRequest(int address, char content, const std::string& id,
                   const std::string& data, bool bcc);
Bytes BuildRead(int address, const std::string& id, bool bcc);
Bytes BuildWrite(int address, const std::string& id, const std::string& data, bool bcc);
Bytes BuildStore(int address, bool bcc);

// Five-character data field for a value in the data's own units (tenths of a degree
// for a temperature). Empty when the value does not fit: only -9999..9999 does,
// because the first character is the sign position.
std::string FormatData(int value);
bool        ParseData(const std::string& data, int& value);
std::string FormatTenths(int tenths);   // 250 -> "25.0", -99 -> "-9.9"

// -------------------------------------------------------------- reading

enum class MsgType { Unknown, ReadRequest, WriteRequest, StoreRequest, Ack, AckData, Nak };

struct Message
{
  MsgType     type    = MsgType::Unknown;
  int         address = -1;
  std::string id;               // requests and AckData
  std::string data;             // WriteRequest and AckData
  int         error   = -1;     // Nak
  bool        hasBcc  = false;  // the frame was parsed as carrying a BCC byte
  bool        bccOk   = true;
  u8          bccGot  = 0;
  u8          bccWant = 0;
  std::string problem;          // empty when the frame parsed cleanly
};

// Parses one complete frame, STX through ETX plus the BCC byte when bccEnabled.
// Requests and replies are both understood, so it also decodes what we send.
Message ParseFrame(const Bytes& frame, bool bccEnabled);

// True for Ack, AckData and Nak.
bool IsReply(const Message& m);

// "<STX>01<ACK>PV100250<ETX>[06]": control characters by name, a space as "_" (the
// manual's notation), the BCC byte in brackets when hasBcc.
std::string Readable(const Bytes& bytes, bool hasBcc);

// One line of plain English for a parsed message.
std::string Describe(const Message& m);

std::string ErrorText(int code);     // Table 3-1
std::string AlarmText(int flags);    // Table 3-3, the flags add up

// -------------------------------------------------------------- framing

// Cuts the byte stream into frames. Nothing is lost: bytes that do not make a frame
// come back as Partial (an STX with no end) or Stray (no STX at all).
class FrameAssembler
{
public:
  struct Chunk
  {
    enum Type { Frame, Partial, Stray };
    Type  type;
    Bytes bytes;
  };

  explicit FrameAssembler(bool bcc = false) : m_bcc(bcc)
  {
  }

  // Whether a BCC byte follows each ETX. Drops whatever was being assembled.
  void setBcc(bool on)
  {
    m_bcc = on;
    reset();
  }

  void reset();

  void feed(const u8* data, size_t len, std::vector<Chunk>& out);

  // True while bytes are held back, waiting for more.
  bool pending() const
  {
    return !m_buf.empty();
  }

  // Hands back what is being held, e.g. after a period of silence.
  void flush(std::vector<Chunk>& out);

private:
  enum class State { Idle, Body, Bcc };

  bool  m_bcc;
  State m_state = State::Idle;
  Bytes m_buf;   // the frame so far, or the stray bytes while Idle
};

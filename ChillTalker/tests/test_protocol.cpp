// Console self-check for the protocol layers. The expected bytes in TestManualExamples
// are the worked examples of the chiller's Serial Communication Manual, section 3.7, and
// those in TestNanotecManualExamples the worked examples of Nanotec's N5 Modbus RTU manual
// (V1.5.0, section 8.6, the same frames as the PD1-C); the rest are cases of our own.
// Exit code 0 means everything passed.

#include <cstdio>
#include <initializer_list>
#include <string>

#include "Nanotec.h"
#include "Protocol.h"

namespace nt = nanotec;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    g_checks++;                                                        \
    if (!(cond)) {                                                     \
      g_failed++;                                                      \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                  \
  } while (0)

static Bytes B(std::initializer_list<int> v)
{
  Bytes b;
  for (int x : v) b.push_back(u8(x));
  return b;
}

static Bytes Cat(Bytes a, const Bytes& b)
{
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

static bool Has(const std::string& s, const char* part)
{
  return s.find(part) != std::string::npos;
}

// ---------------------------------------------------------------- manual

static const Bytes READ_REQ  = B({0x02, 0x30, 0x31, 0x52, 0x50, 0x56, 0x31, 0x03, 0x65});
static const Bytes READ_ACK  = B({0x02, 0x30, 0x31, 0x06, 0x50, 0x56, 0x31, 0x30, 0x30, 0x32, 0x35, 0x30, 0x03, 0x06});
static const Bytes WRITE_REQ = B({0x02, 0x31, 0x30, 0x57, 0x53, 0x56, 0x31, 0x30, 0x30, 0x32, 0x30, 0x30, 0x03, 0x51});
static const Bytes WRITE_ACK = B({0x02, 0x31, 0x30, 0x06, 0x03, 0x06});

static void TestManualExamples()
{
  // request to read: address 1, PV1
  CHECK(BuildRead(1, ID_PV, true) == READ_REQ);
  Message m = ParseFrame(READ_REQ, true);
  CHECK(m.problem.empty() && m.type == MsgType::ReadRequest && m.address == 1 && m.id == "PV1" && m.bccOk);

  // its reply: 25.0 degC
  m = ParseFrame(READ_ACK, true);
  CHECK(m.problem.empty() && m.type == MsgType::AckData && m.address == 1 && m.id == "PV1" &&
        m.data == "00250" && m.bccOk);
  int v = 0;
  CHECK(ParseData(m.data, v) && v == 250);
  CHECK(Has(Describe(m), "25.0 degC"));

  // request to write: address 10, SV1 = 20.0 degC
  CHECK(BuildWrite(10, ID_SV, FormatData(200), true) == WRITE_REQ);
  m = ParseFrame(WRITE_REQ, true);
  CHECK(m.type == MsgType::WriteRequest && m.address == 10 && m.id == "SV1" && m.data == "00200" && m.bccOk);

  // its reply
  m = ParseFrame(WRITE_ACK, true);
  CHECK(m.type == MsgType::Ack && m.address == 10 && m.bccOk && IsReply(m));
}

// ---------------------------------------------------------------- building and data

static void TestBuilding()
{
  // no BCC: nothing after ETX
  CHECK(BuildRead(1, ID_PV, false) == B({0x02, 0x30, 0x31, 0x52, 0x50, 0x56, 0x31, 0x03}));
  // an identifier with a leading space
  CHECK(BuildRead(1, ID_MODE, false) == B({0x02, 0x30, 0x31, 0x52, 0x20, 0x4D, 0x44, 0x03}));
  // store carries no data. Its BCC happens to be 02h, the value of STX
  CHECK(BuildStore(1, true) == B({0x02, 0x30, 0x31, 0x57, 0x53, 0x54, 0x52, 0x03, 0x02}));
  CHECK(ParseFrame(BuildStore(7, true), true).type == MsgType::StoreRequest);
}

static void TestData()
{
  CHECK(FormatData(250) == "00250");
  CHECK(FormatData(0) == "00000");
  CHECK(FormatData(-99) == "-0099");
  CHECK(FormatData(9999) == "09999");
  CHECK(FormatData(10000).empty());
  CHECK(FormatData(-10000).empty());

  int v = 0;
  CHECK(ParseData("-0099", v) && v == -99);
  CHECK(ParseData("05000", v) && v == 5000);
  CHECK(!ParseData("0025", v));
  CHECK(!ParseData("1xx00", v));
  CHECK(!ParseData("10000", v));   // the first position is the sign position
  CHECK(!ParseData("-+099", v));

  CHECK(FormatTenths(250) == "25.0");
  CHECK(FormatTenths(-99) == "-9.9");
  CHECK(FormatTenths(5) == "0.5");
  CHECK(FormatTenths(-5) == "-0.5");
  CHECK(FormatTenths(0) == "0.0");
}

// ---------------------------------------------------------------- replies

static void TestReplies()
{
  // error 2 (no such item), with and without BCC
  const Bytes nak = B({0x02, 0x30, 0x31, 0x15, 0x32, 0x03, 0x27});
  Message m = ParseFrame(nak, true);
  CHECK(m.type == MsgType::Nak && m.error == 2 && m.bccOk && IsReply(m));
  CHECK(Has(Describe(m), "unknown identifier"));
  m = ParseFrame(B({0x02, 0x30, 0x31, 0x15, 0x32, 0x03}), false);
  CHECK(m.type == MsgType::Nak && m.error == 2);

  // a wrong BCC is reported and the frame is still decoded
  Bytes bad = nak;
  bad.back() = 0x28;
  m = ParseFrame(bad, true);
  CHECK(m.type == MsgType::Nak && !m.bccOk && m.bccGot == 0x28 && m.bccWant == 0x27);
  CHECK(Has(Describe(m), "BCC MISMATCH"));

  // alarm flags add up: AL6 + AL7 = 00192
  CHECK(AlarmText(0) == "no alarm");
  CHECK(AlarmText(192) == "AL6 low level, AL7 thermostat");
  m = ParseFrame(B({0x02, '0', '1', 0x06, ' ', 'A', 'L', '0', '0', '1', '9', '2', 0x03}), false);
  CHECK(m.type == MsgType::AckData && m.id == ID_ALARM && Has(Describe(m), "AL7 thermostat"));

  // operation mode
  m = ParseFrame(B({0x02, '0', '1', 0x06, ' ', 'M', 'D', '0', '0', '0', '0', '2', 0x03}), false);
  CHECK(m.type == MsgType::AckData && Has(Describe(m), "STOP (RDY)"));

  // things that are not a message
  CHECK(!ParseFrame(B({0x02, 0x30}), false).problem.empty());
  CHECK(!ParseFrame(B({0x30, 0x30, 0x31, 0x52, 0x50, 0x56, 0x31, 0x03}), false).problem.empty());
  CHECK(!ParseFrame(B({0x02, 'A', '1', 'R', 'P', 'V', '1', 0x03}), false).problem.empty());
  CHECK(ParseFrame(B({0x02, '0', '1', 'X', 'P', 'V', '1', 0x03}), false).type == MsgType::Unknown);

  CHECK(Readable(READ_REQ, true) == "<STX>01RPV1<ETX>[65]");
  CHECK(Readable(BuildRead(1, ID_MODE, false), false) == "<STX>01R_MD<ETX>");
}

// ---------------------------------------------------------------- framing

using Chunks = std::vector<FrameAssembler::Chunk>;

static void TestAssembler()
{
  // one byte at a time
  {
    FrameAssembler a(true);
    Chunks         out;
    for (u8 b : READ_ACK) a.feed(&b, 1, out);
    CHECK(out.size() == 1 && out[0].type == FrameAssembler::Chunk::Frame && out[0].bytes == READ_ACK);
    CHECK(!a.pending());
  }

  // two frames in one read, with noise in front
  {
    FrameAssembler a(true);
    Chunks         out;
    const Bytes    in = Cat(Cat(B({0x00, 0xFF}), READ_ACK), WRITE_ACK);
    a.feed(in.data(), in.size(), out);
    CHECK(out.size() == 3);
    CHECK(out[0].type == FrameAssembler::Chunk::Stray && out[0].bytes == B({0x00, 0xFF}));
    CHECK(out[1].type == FrameAssembler::Chunk::Frame && out[1].bytes == READ_ACK);
    CHECK(out[2].type == FrameAssembler::Chunk::Frame && out[2].bytes == WRITE_ACK);
  }

  // without BCC a frame ends at ETX; a byte after it is stray
  {
    FrameAssembler a(false);
    Chunks         out;
    const Bytes    in = B({0x02, 0x30, 0x31, 0x06, 0x03, 0x06});
    a.feed(in.data(), in.size(), out);
    CHECK(out.size() == 1 && out[0].type == FrameAssembler::Chunk::Frame && out[0].bytes.size() == 5);
    CHECK(a.pending());
    a.flush(out);
    CHECK(out.size() == 2 && out[1].type == FrameAssembler::Chunk::Stray && out[1].bytes == B({0x06}));
  }

  // a BCC that equals STX is still just the BCC
  {
    FrameAssembler a(true);
    Chunks         out;
    const Bytes    store = BuildStore(1, true);
    a.feed(store.data(), store.size(), out);
    CHECK(out.size() == 1 && out[0].type == FrameAssembler::Chunk::Frame && out[0].bytes == store);
    CHECK(!a.pending());
  }

  // a frame cut short by the start of the next one
  {
    FrameAssembler a(false);
    Chunks         out;
    const Bytes    in = B({0x02, 0x30, 0x31, 0x52, 0x02, 0x30, 0x31, 0x06, 0x03});
    a.feed(in.data(), in.size(), out);
    CHECK(out.size() == 2);
    CHECK(out[0].type == FrameAssembler::Chunk::Partial && out[0].bytes == B({0x02, 0x30, 0x31, 0x52}));
    CHECK(out[1].type == FrameAssembler::Chunk::Frame && out[1].bytes == B({0x02, 0x30, 0x31, 0x06, 0x03}));
  }

  // BCC expected but never sent: held back, then handed over as Partial
  {
    FrameAssembler a(true);
    Chunks         out;
    const Bytes    in = B({0x02, 0x30, 0x31, 0x06, 0x03});
    a.feed(in.data(), in.size(), out);
    CHECK(out.empty() && a.pending());
    a.flush(out);
    CHECK(out.size() == 1 && out[0].type == FrameAssembler::Chunk::Partial && out[0].bytes == in);
    CHECK(!a.pending());
  }

  // flushing nothing hands back nothing
  {
    FrameAssembler a(false);
    Chunks         out;
    a.flush(out);
    CHECK(out.empty());
  }
}

// ---------------------------------------------------------------- Nanotec: the manual

// Objects the manual's examples use that the tool itself does not.
static const nt::Object OD_VL_TARGET   = { 0x6042, 0, "vl target velocity",           2, true,  nt::Fmt::Number };
static const nt::Object OD_MAX_CURRENT = { 0x2031, 0, "max motor current",            4, false, nt::Fmt::Number };
static const nt::Object OD_MODE_SHOWN  = { 0x6061, 0, "modes of operation display",   1, true,  nt::Fmt::Mode };

static void TestNanotecManualExamples()
{
  // report server id: request and reply
  CHECK(nt::BuildReportId(5) == B({0x05, 0x11, 0xC2, 0xEC}));
  nt::Frame f = nt::Parse(B({0x05, 0x11, 0xC2, 0xEC}));
  CHECK(f.problem.empty() && f.kind == nt::Kind::ReportIdRequest && f.slave == 5);
  f = nt::Parse(B({0x05, 0x11, 0x02, 0x05, 0xFF, 0x0F, 0xEC}));
  CHECK(f.problem.empty() && f.kind == nt::Kind::ReportIdReply && f.slave == 5 && f.data == B({0x05, 0xFF}));
  CHECK(nt::IsOk(f) && Has(nt::Describe(f), "run indicator ON"));

  // read 6042:00 (16 bits): the request, and the reply carrying 0000h
  CHECK(nt::BuildReadObject(5, OD_VL_TARGET) ==
        B({0x05, 0x2B, 0x0D, 0x00, 0x00, 0x01, 0x60, 0x42, 0x00, 0x00, 0x00, 0x00, 0x02, 0x7F, 0x0F}));
  f = nt::Parse(B({0x05, 0x2B, 0x0D, 0x00, 0x00, 0x01, 0x60, 0x42, 0x00, 0x00, 0x00, 0x00, 0x02, 0x7F, 0x0F}));
  CHECK(f.problem.empty() && f.kind == nt::Kind::ReadRequest && f.index == 0x6042 && f.sub == 0 && f.size == 2);
  f = nt::Parse(B({0x05, 0x2B, 0x0D, 0x00, 0x00, 0x01, 0x60, 0x42, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x60, 0x34}));
  CHECK(f.problem.empty() && f.kind == nt::Kind::ReadReply && f.data == B({0x00, 0x00}));

  // set 6060 = 02h: the reply is the header again, with no data
  CHECK(nt::BuildWriteObject(5, nt::OD_MODE, 2) ==
        B({0x05, 0x2B, 0x0D, 0x01, 0x00, 0x01, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0xC9, 0x2F}));
  f = nt::Parse(B({0x05, 0x2B, 0x0D, 0x01, 0x00, 0x01, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA9, 0x89}));
  CHECK(f.problem.empty() && f.kind == nt::Kind::WriteReply && f.index == 0x6060 && nt::IsOk(f));

  // set 2031 = 03E8h, a 32-bit value: little endian
  CHECK(nt::BuildWriteObject(5, OD_MAX_CURRENT, 1000) ==
        B({0x05, 0x2B, 0x0D, 0x01, 0x00, 0x01, 0x20, 0x31, 0x00, 0x00, 0x00, 0x00, 0x04, 0xE8, 0x03, 0x00, 0x00, 0xC3, 0x53}));
  CHECK(nt::Parse(B({0x05, 0x2B, 0x0D, 0x01, 0x00, 0x01, 0x20, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE5, 0xCC})).kind ==
        nt::Kind::WriteReply);

  // the control word sequence of the manual: 00h, 80h, 06h, 07h, 0Fh
  struct { int value; u8 crcLo, crcHi; } const control[] = {
    { 0x00, 0x1C, 0x2E }, { 0x80, 0x7D, 0xEE }, { 0x06, 0x1F, 0x8E }, { 0x07, 0x1E, 0x1E }, { 0x0F, 0x19, 0xDE },
  };
  for (const auto& c : control) {
    CHECK(nt::BuildWriteObject(5, nt::OD_CONTROL_WORD, c.value) ==
          B({0x05, 0x2B, 0x0D, 0x01, 0x00, 0x01, 0x60, 0x40, 0x00, 0x00, 0x00, 0x00, 0x02, c.value, 0x00, c.crcLo, c.crcHi}));
  }
  CHECK(nt::Parse(B({0x05, 0x2B, 0x0D, 0x01, 0x00, 0x01, 0x60, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAE, 0xE9})).kind ==
        nt::Kind::WriteReply);

  // read 6041:00, the status word: 0637h is "operation enabled"
  CHECK(nt::BuildReadObject(5, nt::OD_STATUS_WORD) ==
        B({0x05, 0x2B, 0x0D, 0x00, 0x00, 0x01, 0x60, 0x41, 0x00, 0x00, 0x00, 0x00, 0x02, 0x7F, 0x3C}));
  f = nt::Parse(B({0x05, 0x2B, 0x0D, 0x00, 0x00, 0x01, 0x60, 0x41, 0x00, 0x00, 0x00, 0x00, 0x02, 0x37, 0x06, 0xB6, 0x13}));
  long long v = 0;
  CHECK(f.kind == nt::Kind::ReadReply && nt::ValueOf(f, nt::OD_STATUS_WORD, v) && v == 0x0637);
  CHECK(nt::StateOf(u16(v)) == nt::State::OperationEnabled);
  CHECK(Has(nt::Describe(f), "status word = 0x0637 (operation enabled, target reached)"));

  // read 6061:00, an 8-bit value
  CHECK(nt::BuildReadObject(5, OD_MODE_SHOWN) ==
        B({0x05, 0x2B, 0x0D, 0x00, 0x00, 0x01, 0x60, 0x61, 0x00, 0x00, 0x00, 0x00, 0x01, 0x38, 0x5D}));
  f = nt::Parse(B({0x05, 0x2B, 0x0D, 0x00, 0x00, 0x01, 0x60, 0x61, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x5C, 0xD2}));
  CHECK(f.kind == nt::Kind::ReadReply && nt::ValueOf(f, OD_MODE_SHOWN, v) && v == 0);

  // the error reply: the manual asks for 2 bytes of 6060:00, which is 1 byte long. Ours
  // asks for the object's own size
  f = nt::Parse(B({0x05, 0x2B, 0x0D, 0x00, 0x00, 0x01, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00, 0x02, 0x79, 0x8D}));
  CHECK(f.problem.empty() && f.kind == nt::Kind::ReadRequest && f.index == 0x6060 && f.size == 2);
  CHECK(nt::Parse(nt::BuildReadObject(5, nt::OD_MODE)).size == 1);
  f = nt::Parse(B({0x05, 0xAB, 0xFF, 0x00, 0x06, 0x0D, 0xCE, 0x12, 0x00, 0x07, 0x06, 0xAC, 0x3C}));
  CHECK(f.problem.empty() && f.kind == nt::Kind::ObjectException && f.exception == 0xCE && f.abort == 0x06070012);
  CHECK(!nt::IsOk(f) && Has(nt::Describe(f), "06070012") && Has(nt::Describe(f), "data too long"));
}

// ---------------------------------------------------------------- Nanotec: our own cases

// The expected bytes below were computed with an independent CRC-16/MODBUS (Python), which
// reproduces every CRC of the manual's examples.
static void TestNanotecBuilding()
{
  CHECK(nt::BuildReportId(12) == B({0x0C, 0x11, 0xC4, 0xBC}));
  CHECK(nt::BuildReadObject(12, nt::OD_STATUS_WORD) ==
        B({0x0C, 0x2B, 0x0D, 0x00, 0x00, 0x01, 0x60, 0x41, 0x00, 0x00, 0x00, 0x00, 0x02, 0x62, 0xF5}));
  CHECK(nt::BuildReadObject(5, nt::OD_VELOCITY_ACTUAL) ==
        B({0x05, 0x2B, 0x0D, 0x00, 0x00, 0x01, 0x60, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x04, 0xF9, 0x43}));
  // a negative 32-bit value goes out as two's complement, little endian
  CHECK(nt::BuildWriteObject(5, nt::OD_TARGET_VELOCITY, -150) ==
        B({0x05, 0x2B, 0x0D, 0x01, 0x00, 0x01, 0x60, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x04, 0x6A, 0xFF, 0xFF, 0xFF, 0x1B, 0xA3}));
  CHECK(nt::BuildWriteObject(5, nt::OD_ACCELERATION, 300) ==
        B({0x05, 0x2B, 0x0D, 0x01, 0x00, 0x01, 0x60, 0x83, 0x00, 0x00, 0x00, 0x00, 0x04, 0x2C, 0x01, 0x00, 0x00, 0xAC, 0xBF}));

  // the CRC of a whole frame, its own CRC included, is zero
  const Bytes all = nt::BuildWriteObject(7, nt::OD_TARGET_VELOCITY, 300);
  CHECK(nt::Crc16(all.data(), all.size()) == 0);

  CHECK(nt::FindObject(0x6041, 0) == &nt::OD_STATUS_WORD);
  CHECK(nt::FindObject(0x6041, 1) == nullptr);
  CHECK(nt::FindObject(0x1234, 0) == nullptr);
}

static void TestNanotecDecoding()
{
  // a velocity reply: 300 rpm, and a negative one
  nt::Frame f = nt::Parse(B({0x05, 0x2B, 0x0D, 0x00, 0x00, 0x01, 0x60, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x04, 0x2C, 0x01, 0x00, 0x00, 0x98, 0x18}));
  long long v = 0;
  CHECK(f.kind == nt::Kind::ReadReply && nt::ValueOf(f, nt::OD_VELOCITY_ACTUAL, v) && v == 300);
  CHECK(Has(nt::Describe(f), "velocity actual value = 300 rpm"));
  f = nt::Parse(B({0x05, 0x2B, 0x0D, 0x01, 0x00, 0x01, 0x60, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x04, 0x6A, 0xFF, 0xFF, 0xFF, 0x1B, 0xA3}));
  CHECK(f.kind == nt::Kind::WriteRequest && nt::ValueOf(f, nt::OD_TARGET_VELOCITY, v) && v == -150);
  CHECK(Has(nt::Describe(f), "write 60FF:00 target velocity = -150 rpm to slave 5"));

  // what a request says in English
  CHECK(Has(nt::Describe(nt::Parse(nt::BuildWriteObject(5, nt::OD_CONTROL_WORD, nt::CW_ENABLE))), "control word = 0x000F (enable operation)"));
  CHECK(Has(nt::Describe(nt::Parse(nt::BuildWriteObject(5, nt::OD_MODE, 3))), "mode of operation = 3 (profile velocity)"));
  CHECK(Has(nt::Describe(nt::Parse(nt::BuildReadObject(5, nt::OD_STATUS_WORD))), "read 6041:00 status word (2 bytes) from slave 5"));
  CHECK(Has(nt::Describe(nt::Parse(nt::BuildReportId(5))), "report server id from slave 5"));
  CHECK(Has(nt::Describe(nt::Parse(nt::BuildWriteObject(5, OD_MAX_CURRENT, 1000))), "unknown object"));

  // the power states, from the status word patterns of the manual
  CHECK(nt::StateOf(0x0000) == nt::State::NotReady);
  CHECK(nt::StateOf(0x0250) == nt::State::SwitchOnDisabled);
  CHECK(nt::StateOf(0x0231) == nt::State::ReadyToSwitchOn);
  CHECK(nt::StateOf(0x0233) == nt::State::SwitchedOn);
  CHECK(nt::StateOf(0x0637) == nt::State::OperationEnabled);
  CHECK(nt::StateOf(0x0007) == nt::State::QuickStopActive);
  CHECK(nt::StateOf(0x000F) == nt::State::FaultReaction);
  CHECK(nt::StateOf(0x0008) == nt::State::Fault);
  CHECK(nt::StateOf(0x0618) == nt::State::Fault);   // the don't-care bits do not matter

  // errors: a plain Modbus exception and the object access kind with other abort codes
  f = nt::Parse(B({0x05, 0x91, 0x01, 0xCD, 0x91}));
  CHECK(f.kind == nt::Kind::Exception && f.exception == 1 && !nt::IsOk(f));
  CHECK(Has(nt::Describe(f), "exception from slave 5: 01 - illegal function"));
  f = nt::Parse(B({0x05, 0xAB, 0xFF, 0x00, 0x06, 0x0D, 0xCE, 0x00, 0x00, 0x02, 0x06, 0xAA, 0x14}));
  CHECK(f.kind == nt::Kind::ObjectException && f.abort == 0x06020000 && Has(nt::Describe(f), "the object does not exist"));
  f = nt::Parse(B({0x05, 0xAB, 0xFF, 0x00, 0x03, 0x0D, 0xAE, 0x01, 0xA2, 0xD6}));
  CHECK(f.kind == nt::Kind::ObjectException && f.exception == 0xAE && Has(nt::Describe(f), "protocol control"));

  // frames that are not good
  Bytes bad = nt::BuildReportId(5);
  bad.back() ^= 0x01;
  CHECK(nt::Parse(bad).problem == "CRC mismatch");
  CHECK(!nt::Parse(B({0x05, 0x11})).problem.empty());
  CHECK(!nt::Parse(B({0x02, '0', '1', 'R', 'P', 'V', '1', 0x03})).problem.empty());   // a chiller frame

  // which reply answers which request
  const nt::Frame idReq   = nt::Parse(nt::BuildReportId(5));
  const nt::Frame readReq = nt::Parse(nt::BuildReadObject(5, nt::OD_STATUS_WORD));
  const nt::Frame wrReq   = nt::Parse(nt::BuildWriteObject(5, nt::OD_CONTROL_WORD, 6));
  const nt::Frame idRep   = nt::Parse(B({0x05, 0x11, 0x02, 0x05, 0xFF, 0x0F, 0xEC}));
  const nt::Frame readRep = nt::Parse(B({0x05, 0x2B, 0x0D, 0x00, 0x00, 0x01, 0x60, 0x41, 0x00, 0x00, 0x00, 0x00, 0x02, 0x37, 0x06, 0xB6, 0x13}));
  const nt::Frame wrRep   = nt::Parse(B({0x05, 0x2B, 0x0D, 0x01, 0x00, 0x01, 0x60, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAE, 0xE9}));
  CHECK(nt::Answers(idRep, idReq) && nt::Answers(readRep, readReq) && nt::Answers(wrRep, wrReq));
  CHECK(!nt::Answers(idRep, readReq) && !nt::Answers(readRep, idReq) && !nt::Answers(readRep, wrReq));
  CHECK(!nt::Answers(wrRep, readReq) && !nt::Answers(idReq, idReq));
  CHECK(!nt::Answers(idRep, nt::Parse(nt::BuildReportId(6))));   // another slave
  // a reply about another object is not the answer
  CHECK(!nt::Answers(readRep, nt::Parse(nt::BuildReadObject(5, nt::OD_VELOCITY_ACTUAL))));
  // an error reply answers a request of the same function, and no other
  CHECK(nt::Answers(nt::Parse(B({0x05, 0x91, 0x01, 0xCD, 0x91})), idReq));
  CHECK(!nt::Answers(nt::Parse(B({0x05, 0x91, 0x01, 0xCD, 0x91})), readReq));
  CHECK(nt::Answers(nt::Parse(B({0x05, 0xAB, 0xFF, 0x00, 0x06, 0x0D, 0xCE, 0x12, 0x00, 0x07, 0x06, 0xAC, 0x3C})), readReq));
}

// ---------------------------------------------------------------- Nanotec: framing

using RtuChunks = std::vector<nt::Assembler::Chunk>;

static void TestNanotecAssembler()
{
  const Bytes request = nt::BuildReadObject(5, nt::OD_STATUS_WORD);
  const Bytes reply   = B({0x05, 0x2B, 0x0D, 0x00, 0x00, 0x01, 0x60, 0x41, 0x00, 0x00, 0x00, 0x00, 0x02, 0x37, 0x06, 0xB6, 0x13});
  const Bytes idReply = B({0x05, 0x11, 0x02, 0x05, 0xFF, 0x0F, 0xEC});

  // one byte at a time: the frame appears with its last byte
  {
    nt::Assembler a;
    RtuChunks     out;
    for (size_t i = 0; i < reply.size(); i++) {
      a.feed(&reply[i], 1, out);
      CHECK(out.empty() == (i + 1 < reply.size()));
    }
    CHECK(out.size() == 1 && out[0].type == nt::Assembler::Chunk::Frame && out[0].bytes == reply);
    CHECK(!a.pending());
  }

  // the echo of the request and then the reply, in one read
  {
    nt::Assembler a;
    RtuChunks     out;
    const Bytes   in = Cat(request, reply);
    a.feed(in.data(), in.size(), out);
    CHECK(out.size() == 2 && out[0].bytes == request && out[1].bytes == reply);
    CHECK(out[0].type == nt::Assembler::Chunk::Frame && out[1].type == nt::Assembler::Chunk::Frame);
  }

  // two replies back to back
  {
    nt::Assembler a;
    RtuChunks     out;
    const Bytes   in = Cat(idReply, idReply);
    a.feed(in.data(), in.size(), out);
    CHECK(out.size() == 2 && out[0].bytes == idReply && out[1].bytes == idReply);
  }

  // noise in front of a frame: held back, then on flush the noise is stray and the frame is found
  {
    nt::Assembler a;
    RtuChunks     out;
    const Bytes   in = Cat(B({0xFF, 0x00, 0x13}), idReply);
    a.feed(in.data(), in.size(), out);
    CHECK(out.empty() && a.pending());
    a.flush(out);
    CHECK(out.size() == 2);
    CHECK(out[0].type == nt::Assembler::Chunk::Stray && out[0].bytes == B({0xFF, 0x00, 0x13}));
    CHECK(out[1].type == nt::Assembler::Chunk::Frame && out[1].bytes == idReply);
    CHECK(!a.pending());
  }

  // a frame cut short: held back, then stray
  {
    nt::Assembler a;
    RtuChunks     out;
    a.feed(reply.data(), reply.size() - 3, out);
    CHECK(out.empty() && a.pending());
    a.flush(out);
    CHECK(out.size() == 1 && out[0].type == nt::Assembler::Chunk::Stray && out[0].bytes.size() == reply.size() - 3);
  }

  // a chiller frame makes no Modbus frame
  {
    nt::Assembler a;
    RtuChunks     out;
    const Bytes   chiller = B({0x02, 0x30, 0x31, 0x06, 0x50, 0x56, 0x31, 0x30, 0x30, 0x32, 0x35, 0x30, 0x03});
    a.feed(chiller.data(), chiller.size(), out);
    a.flush(out);
    CHECK(out.size() == 1 && out[0].type == nt::Assembler::Chunk::Stray && out[0].bytes == chiller);
  }

  // a flood that never makes a frame does not grow without end
  {
    nt::Assembler a;
    RtuChunks     out;
    const Bytes   flood(300, 0xAA);
    a.feed(flood.data(), flood.size(), out);
    CHECK(!a.pending() && !out.empty() && out[0].type == nt::Assembler::Chunk::Stray);
  }

  // flushing nothing hands back nothing
  {
    nt::Assembler a;
    RtuChunks     out;
    a.flush(out);
    CHECK(out.empty());
  }
}

int main()
{
  TestManualExamples();
  TestBuilding();
  TestData();
  TestReplies();
  TestAssembler();
  TestNanotecManualExamples();
  TestNanotecBuilding();
  TestNanotecDecoding();
  TestNanotecAssembler();

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}

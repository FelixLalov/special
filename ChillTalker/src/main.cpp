// ChillTalker - talk to an SMC Thermo-con HEF002 chiller over a serial port.
//
// A menu sends the requests the Serial Communication Manual defines (Protocol.h).
// Every frame in both directions is shown as bytes and as plain English, and so is
// anything that arrives while the prompt is idle. One thread: the port is polled.

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ComPort.h"
#include "Console.h"
#include "Nanotec.h"
#include "Protocol.h"

using con::Color;
using Clock = std::chrono::steady_clock;
namespace nt = nanotec;

// ---------------------------------------------------------------- constants

static const int RESPONSE_TIMEOUT_MS = 1000;    // the chiller's own response delay is 0..250 ms
static const int STORE_TIMEOUT_MS    = 10000;   // a store is acknowledged after about 6 s
static const int IDLE_FLUSH_MS       = 200;     // a half-received frame is shown after this much silence

// The Nanotec drives, by Modbus RTU
static const int RTU_TIMEOUT_MS  = 500;         // one request to a drive that is known to be there
static const int SCAN_TIMEOUT_MS = 120;         // one probe of the bus scan; MetalJet waits 100 ms for an answer
static const int RTU_GAP_MS      = 50;          // a half-received Modbus frame is shown after this much silence
static const int TEST_RPM        = 300;         // the test turns a drive the way MetalJet runs its stirrers:
static const int TEST_ACCEL      = 300;         // 300 rpm, acceleration 300
static const int TEST_RUN_MS     = 5000;
static const int TEST_STOP_MS    = 10000;       // the longest wait for standstill
static const int MIN_TURN_RPM    = 10;          // the shaft counts as turning above this
static const int STANDSTILL_RPM  = 2;           // and as stopped at or below this

// The PD1-C factory setting, which is how MetalJet opens the drives.
static const LineSettings NANOTEC_LINE = { 19200, 8, 'E', 1 };

// Lines after the first of a message start under the hex bytes: "[hh:mm:ss.mmm] TX  ".
static const char* const INDENT = "                   ";

// ---------------------------------------------------------------- helpers

static std::string Trim(const std::string& s)
{
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

static std::string Lower(const std::string& s)
{
  std::string r = s;
  for (char& c : r) c = char(std::tolower((unsigned char)c));
  return r;
}

static bool ToInt(const std::string& t, int& v)
{
  if (t.empty()) return false;
  char* end = nullptr;
  const long n = std::strtol(t.c_str(), &end, 10);
  if (*end != '\0' || n < -1000000 || n > 1000000) return false;
  v = int(n);
  return true;
}

static long long MsSince(Clock::time_point t)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

static std::string Stamp()
{
  SYSTEMTIME t;
  GetLocalTime(&t);
  char buf[32];
  std::snprintf(buf, sizeof buf, "[%02d:%02d:%02d.%03d]", t.wHour, t.wMinute, t.wSecond,
                t.wMilliseconds);
  return buf;
}

static std::string HexText(const Bytes& b, size_t maxBytes = 32)
{
  std::string s;
  char        buf[8];
  for (size_t i = 0; i < b.size() && i < maxBytes; i++) {
    std::snprintf(buf, sizeof buf, i ? " %02X" : "%02X", b[i]);
    s += buf;
  }
  if (b.size() > maxBytes) s += " ...";
  return s;
}

// Accepts "02 30 31", "0x02,0x30" and "023031": tokens of one or two hex digits, or
// longer even-length runs that are cut into pairs.
static bool ParseHexBytes(const std::string& text, Bytes& out)
{
  out.clear();
  std::string tok;
  for (size_t i = 0; i <= text.size(); i++) {
    const char c = i < text.size() ? text[i] : ' ';
    if (c == ' ' || c == ',' || c == ';' || c == '\t') {
      if (tok.size() >= 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) tok.erase(0, 2);
      if (!tok.empty()) {
        if (tok.size() > 2 && tok.size() % 2) return false;
        for (size_t k = 0; k < tok.size(); k += 2) {
          out.push_back(u8(std::strtoul(tok.substr(k, 2).c_str(), nullptr, 16)));
        }
        tok.clear();
      }
    } else if (std::isxdigit((unsigned char)c) || ((c == 'x' || c == 'X') && tok == "0")) {
      tok += c;
    } else {
      return false;
    }
  }
  return !out.empty();
}

// "20", "20.5" or "-9,9": at most one decimal. The result is in tenths.
static bool ParseTemperature(const std::string& in, int& tenths)
{
  size_t i   = 0;
  bool   neg = false;
  if (i < in.size() && (in[i] == '-' || in[i] == '+')) neg = in[i++] == '-';

  int whole  = 0;
  int digits = 0;
  for (; i < in.size() && std::isdigit((unsigned char)in[i]); i++, digits++) {
    whole = whole * 10 + (in[i] - '0');
    if (whole > 999) return false;
  }
  int frac = 0;
  if (i < in.size() && (in[i] == '.' || in[i] == ',')) {
    i++;
    if (i < in.size() && std::isdigit((unsigned char)in[i])) {
      frac = in[i++] - '0';
      digits++;
    }
  }
  if (i != in.size() || digits == 0) return false;
  tenths = (whole * 10 + frac) * (neg ? -1 : 1);
  return true;
}

// ---------------------------------------------------------------- settings

struct Settings
{
  std::string  port;
  LineSettings line;            // 19200 8N2: the chiller's factory line, but for its 9600 baud
  int          address = 1;
  bool         bcc     = false;
};

static bool SetPort(const std::string& t, Settings& s)
{
  std::string p;
  for (char c : t) p += char(std::toupper((unsigned char)c));
  if (p.empty()) return false;
  if (p.find_first_not_of("0123456789") == std::string::npos) p = "COM" + p;   // "4" is COM4
  s.port = p;
  return true;
}

static bool SetBaud(const std::string& t, Settings& s)
{
  int v = 0;
  if (!ToInt(t, v) || (v != 2400 && v != 4800 && v != 9600 && v != 19200 && v != 38400)) 
      return false;
  s.line.baud = v;
  return true;
}

static bool SetDataBits(const std::string& t, Settings& s)
{
  int v = 0;
  if (!ToInt(t, v) || (v != 7 && v != 8)) return false;
  s.line.dataBits = v;
  return true;
}

static bool SetParity(const std::string& t, Settings& s)
{
  const std::string w = Lower(t);
  if      (w == "n" || w == "none") s.line.parity = 'N';
  else if (w == "e" || w == "even") s.line.parity = 'E';
  else if (w == "o" || w == "odd")  s.line.parity = 'O';
  else return false;
  return true;
}

static bool SetStopBits(const std::string& t, Settings& s)
{
  int v = 0;
  if (!ToInt(t, v) || (v != 1 && v != 2)) return false;
  s.line.stopBits = v;
  return true;
}

static bool SetAddress(const std::string& t, Settings& s)
{
  int v = 0;
  if (!ToInt(t, v) || v < MIN_ADDRESS || v > MAX_ADDRESS) return false;
  s.address = v;
  return true;
}

static bool SetBcc(const std::string& t, Settings& s)
{
  const std::string w = Lower(t);
  if      (w == "y" || w == "yes" || w == "on"  || w == "1") s.bcc = true;
  else if (w == "n" || w == "no"  || w == "off" || w == "0") s.bcc = false;
  else return false;
  return true;
}

// One row per question of the settings dialog.
struct Field
{
  const char* label;
  const char* choices;
  std::string (*current)(const Settings&);
  bool        (*apply)(const std::string&, Settings&);
};

static const Field FIELDS[] = 
{
  { "Port",      "e.g. COM4 or 4",              [](const Settings& s) { return s.port; },                          SetPort },
  { "Baud rate", "2400/4800/9600/19200/38400",  [](const Settings& s) { return std::to_string(s.line.baud); },     SetBaud },
  { "Data bits", "7/8",                         [](const Settings& s) { return std::to_string(s.line.dataBits); }, SetDataBits },
  { "Parity",    "N(one)/E(ven)/O(dd)",         [](const Settings& s) { return std::string(1, s.line.parity); },   SetParity },
  { "Stop bits", "1/2",                         [](const Settings& s) { return std::to_string(s.line.stopBits); }, SetStopBits },
  { "Address",   "1-99",                        [](const Settings& s) { return std::to_string(s.address); },       SetAddress },
  { "BCC check", "y/n, as set on the chiller",  [](const Settings& s) { return std::string(s.bcc ? "y" : "n"); }, SetBcc },
};

static std::string LineText(const Settings& s)
{
  char buf[96];
  std::snprintf(buf, sizeof buf, "%s  %d %d%c%d,  address %02d,  BCC %s",
                s.port.empty() ? "(no port)" : s.port.c_str(), s.line.baud, s.line.dataBits,
                s.line.parity, s.line.stopBits, s.address, s.bcc ? "on" : "off");
  return buf;
}

static bool SameLine(const LineSettings& a, const LineSettings& b)
{
  return a.baud == b.baud && a.dataBits == b.dataBits && a.parity == b.parity && a.stopBits == b.stopBits;
}

// ---------------------------------------------------------------- the session

class Talker
{
public:
  Settings settings;

  bool configure();    // the settings dialog, then (re)opens the port
  bool openPort();

  void closePort()
  {
    m_port.close();
  }

  void run();          // the menu loop

private:
  ComPort           m_port;
  FrameAssembler    m_rx;
  nt::Assembler     m_rtu;
  Clock::time_point m_lastRx = Clock::now();

  bool ask(const std::string& prompt, std::string& answer);

  // Reads what has arrived and shows it. Complete frames are also appended to frames.
  void poll(std::vector<Message>* frames = nullptr);
  void show(const FrameAssembler::Chunk& chunk, std::vector<Message>* frames);
  void showTx(const Bytes& request);
  // Sends a request and waits for the chiller's reply, showing everything that arrives.
  bool exchange(const Bytes& request, int timeoutMs);

  void readItem(const char* id);
  void writeTenths(const char* id);
  void writeMode();
  void store();
  void custom();
  void rawHex();

  // The Nanotec drives, by Modbus RTU. They use the same port and the same bus.
  struct Rx
  {
    Bytes       bytes;
    bool        isFrame = false;   // false: bytes that make no frame
    nt::Frame   frame;
    std::string stamp;
  };
  enum class Step { Done, Silent, Refused };

  void rtuPoll(std::vector<Rx>& out);
  void showRtuTx(const Bytes& frame, const std::string& stamp);
  void showRtuRx(const Rx& rx, const char* note = nullptr);
  // Sends a request and waits for the slave's answer, showing everything that arrives.
  // True when the slave answered, with an error too; the answer goes to *answer.
  bool exchangeRtu(const Bytes& request, int timeoutMs, nt::Frame* answer);
  // One request of the test. Silent: nothing answered, and it said so. Refused: the slave
  // answered with an error, which was shown with the frame.
  Step driveStep(const Bytes& request, nt::Frame* answer = nullptr);
  bool setObject(int slave, const nt::Object& o, long long value, int tries = 1);
  bool stopDrive(int slave);
  void lineHint();

  void scanBus();
  void spinDrive();
};

// True when the last byte of a frame is its BCC.
static bool EndsWithBcc(const Bytes& b, bool bccSetting)
{
  return bccSetting && b.size() >= 2 && b[b.size() - 2] == ETX;
}

bool Talker::ask(const std::string& prompt, std::string& answer)
{
  poll();
  if (!con::ReadLine(prompt, answer, [this] { poll(); })) return false;
  answer = Trim(answer);
  return true;
}

bool Talker::openPort()
{
  m_port.close();
  m_rx.setBcc(settings.bcc);

  if (settings.port.empty()) 
  {
    con::Print(Color::Warn, "No port selected - use S.");
    return false;
  }
  std::string err;
  if (!m_port.open(settings.port, settings.line, err)) 
  {
    con::Print(Color::Error, "%s  Use S to try another port.", err.c_str());
    return false;
  }
  con::Print(Color::Normal, "Opened %s", LineText(settings).c_str());
  return true;
}

bool Talker::configure()
{
  std::string known;
  for (const std::string& p : ComPort::list()) known += (known.empty() ? "" : ", ") + p;
  con::Print(Color::Normal, "Serial ports found: %s", known.empty() ? "none" : known.c_str());
  con::Print(Color::Normal, "Enter keeps the value in brackets; they start as the chiller's factory settings, "
                            "except the baud rate.");

  for (const Field& f : FIELDS) {
    for (;;) {
      const std::string cur = f.current(settings);
      char              prompt[160];
      std::snprintf(prompt, sizeof prompt, "  %-10s %-27s [%s]: ", f.label, f.choices,
                    cur.empty() ? "none" : cur.c_str());
      std::string answer;
      if (!ask(prompt, answer)) return false;
      if (answer.empty() || f.apply(answer, settings)) break;
      con::Print(Color::Warn, "  Not valid - %s.", f.choices);
    }
  }
  return openPort();
}

// ---------------------------------------------------------------- receiving

void Talker::poll(std::vector<Message>* frames)
{
  if (!m_port.isOpen()) return;

  std::vector<FrameAssembler::Chunk> chunks;
  u8                                 buf[256];
  std::string                        err;
  for (int i = 0; i < 16; i++) {   // bounded, so a flooded line cannot keep us here
    const int n = m_port.read(buf, sizeof buf, err);
    if (n < 0) {
      con::Print(Color::Error, "%s  Port closed - use S to open it again.", err.c_str());
      m_port.close();
      break;
    }
    if (n == 0) break;
    m_lastRx = Clock::now();
    m_rx.feed(buf, size_t(n), chunks);
  }
  if (m_rx.pending() && MsSince(m_lastRx) > IDLE_FLUSH_MS) m_rx.flush(chunks);

  for (const FrameAssembler::Chunk& c : chunks) show(c, frames);
}

void Talker::show(const FrameAssembler::Chunk& c, std::vector<Message>* frames)
{
  const bool bcc = EndsWithBcc(c.bytes, settings.bcc);

  if (c.type == FrameAssembler::Chunk::Frame) {
    const Message m = ParseFrame(c.bytes, settings.bcc);
    con::Print(Color::Rx, "%s RX  %s", Stamp().c_str(), HexText(c.bytes).c_str());
    con::Print(Color::Rx, "%s%s  %s", INDENT, Readable(c.bytes, bcc).c_str(), Describe(m).c_str());
    if (frames) frames->push_back(m);
    return;
  }

  const char* what;
  if (c.type == FrameAssembler::Chunk::Partial) {
    what = (settings.bcc && c.bytes.back() == ETX)
               ? "ends at ETX and no BCC byte followed - is BCC check off on the chiller? (S)"
               : "incomplete frame - nothing more arrived";
  } else if (c.bytes.size() == 1) {
    what = "one byte outside any frame. If one follows every frame, the chiller sends a BCC - turn BCC on (S)";
  } else {
    what = "bytes outside any frame (no STX before them)";
  }
  con::Print(Color::Warn, "%s RX  %s", Stamp().c_str(), HexText(c.bytes).c_str());
  con::Print(Color::Warn, "%s%s  %s", INDENT, Readable(c.bytes, false).c_str(), what);
}

void Talker::showTx(const Bytes& request)
{
  const Message m = ParseFrame(request, settings.bcc);
  con::Print(Color::Tx, "%s TX  %s", Stamp().c_str(), HexText(request).c_str());
  con::Print(Color::Tx, "%s%s  %s", INDENT, Readable(request, EndsWithBcc(request, settings.bcc)).c_str(),
             Describe(m).c_str());
}

bool Talker::exchange(const Bytes& request, int timeoutMs)
{
  if (!m_port.isOpen()) {
    con::Print(Color::Warn, "The port is not open - use S to set it up.");
    return false;
  }
  poll();      // show what is already waiting, so it is not mistaken for the reply
  Sleep(2);    // the chiller wants at least 1 ms between its reply and the next request

  showTx(request);
  std::string err;
  if (!m_port.write(request.data(), request.size(), err)) {
    con::Print(Color::Error, "%s", err.c_str());
    return false;
  }

  // The reply carries the address the request was sent to. An echo of the request
  // (some RS-485 adapters loop it back) parses as a request, so it never matches.
  const int expect = ParseFrame(request, settings.bcc).address;

  const Clock::time_point start = Clock::now();
  std::vector<Message>    frames;
  while (!con::Quit() && m_port.isOpen() && MsSince(start) < timeoutMs) {
    frames.clear();
    poll(&frames);
    for (const Message& m : frames) {
      if (IsReply(m) && (expect < 0 || m.address == expect)) return true;
    }
    Sleep(2);
  }

  if (m_port.isOpen() && !con::Quit()) {
    con::Print(Color::Warn,
               "No reply within %d ms. The chiller answers only when its address, speed, parity, data "
               "length, stop bits and BCC check equal the settings shown by S, and its communication "
               "standard fits the cable (it ships set to RS-232C). For RS-485: switch the chiller to "
               "485, wire + to pin 1, - to pin 9 and signal ground to pin 5, and try swapping + and -.",
               timeoutMs);
  }
  return false;
}

// ---------------------------------------------------------------- menu actions

void Talker::readItem(const char* id)
{
  exchange(BuildRead(settings.address, id, settings.bcc), RESPONSE_TIMEOUT_MS);
}

void Talker::writeTenths(const char* id)
{
  const IdInfo* info = FindId(id);
  char          prompt[128];
  std::snprintf(prompt, sizeof prompt, "New %s in degC (%s to %s, Enter cancels): ", info->name,
                FormatTenths(info->minValue).c_str(), FormatTenths(info->maxValue).c_str());

  std::string text;
  if (!ask(prompt, text) || text.empty()) return;

  int tenths = 0;
  if (!ParseTemperature(text, tenths)) {
    con::Print(Color::Warn, "Not a temperature with at most one decimal.");
    return;
  }
  if (tenths < info->minValue || tenths > info->maxValue) {
    con::Print(Color::Warn, "Outside the range the chiller accepts. Use C to send it anyway.");
    return;
  }
  exchange(BuildWrite(settings.address, id, FormatData(tenths), settings.bcc), RESPONSE_TIMEOUT_MS);
}

void Talker::writeMode()
{
  std::string a;
  if (!ask("Operation mode - R = RUN, S = STOP (RDY), Enter cancels: ", a) || a.empty()) return;

  const std::string w = Lower(a);
  if (w != "r" && w != "s") {
    con::Print(Color::Warn, "Answer R or S.");
    return;
  }
  const int mode = w == "r" ? MODE_RUN : MODE_STOP;
  exchange(BuildWrite(settings.address, ID_MODE, FormatData(mode), settings.bcc), RESPONSE_TIMEOUT_MS);
}

void Talker::store()
{
  con::Print(Color::Normal, "The chiller acknowledges a store after about 6 s. Do not power it off before then.");
  exchange(BuildStore(settings.address, settings.bcc), STORE_TIMEOUT_MS);
}

void Talker::custom()
{
  std::string content, id, data;
  if (!ask("Content - R (read) or W (write) [R]: ", content)) return;
  content = content.empty() ? "R" : Lower(content);
  if (content != "r" && content != "w") {
    con::Print(Color::Warn, "Answer R or W.");
    return;
  }

  if (!ask("Identifier, 3 characters, _ for a space (PV1 SV1 PVS STR _MD _AL): ", id)) return;
  for (char& c : id) {
    if (c == '_') c = ' ';
  }
  if (id.size() != 3) {
    con::Print(Color::Warn, "The identifier is exactly 3 characters.");
    return;
  }

  if (content == "w" && id != ID_STORE) {
    if (!ask("Data, 5 characters such as 00200 or -0099 (Enter for none): ", data)) return;
    if (!data.empty() && data.size() != 5) {
      con::Print(Color::Warn, "The data is exactly 5 characters. Use H to send anything else.");
      return;
    }
  }
  exchange(BuildRequest(settings.address, content == "w" ? 'W' : 'R', id, data, settings.bcc),
           id == ID_STORE ? STORE_TIMEOUT_MS : RESPONSE_TIMEOUT_MS);
}

void Talker::rawHex()
{
  std::string text;
  if (!ask("Hex bytes to send as they are, e.g. 02 30 31 52 50 56 31 03: ", text) || text.empty()) return;

  Bytes bytes;
  if (!ParseHexBytes(text, bytes)) {
    con::Print(Color::Warn, "Not a list of hex bytes.");
    return;
  }
  exchange(bytes, RESPONSE_TIMEOUT_MS);
}

// ---------------------------------------------------------------- the Nanotec drives

void Talker::rtuPoll(std::vector<Rx>& out)
{
  if (!m_port.isOpen()) return;

  std::vector<nt::Assembler::Chunk> chunks;
  u8                                buf[256];
  std::string                       err;
  for (int i = 0; i < 16; i++) {   // bounded, so a flooded line cannot keep us here
    const int n = m_port.read(buf, sizeof buf, err);
    if (n < 0) {
      con::Print(Color::Error, "%s  Port closed - use S to open it again.", err.c_str());
      m_port.close();
      break;
    }
    if (n == 0) break;
    m_lastRx = Clock::now();
    m_rtu.feed(buf, size_t(n), chunks);
  }
  if (m_rtu.pending() && MsSince(m_lastRx) > RTU_GAP_MS) m_rtu.flush(chunks);

  for (const nt::Assembler::Chunk& c : chunks) {
    Rx rx;
    rx.bytes   = c.bytes;
    rx.isFrame = c.type == nt::Assembler::Chunk::Frame;
    if (rx.isFrame) rx.frame = nt::Parse(c.bytes);
    rx.stamp = Stamp();
    out.push_back(rx);
  }
}

void Talker::showRtuTx(const Bytes& frame, const std::string& stamp)
{
  con::Print(Color::Tx, "%s TX  %s", stamp.c_str(), HexText(frame).c_str());
  con::Print(Color::Tx, "%s%s", INDENT, nt::Describe(nt::Parse(frame)).c_str());
}

void Talker::showRtuRx(const Rx& rx, const char* note)
{
  if (!rx.isFrame) {
    con::Print(Color::Warn, "%s RX  %s", rx.stamp.c_str(), HexText(rx.bytes).c_str());
    con::Print(Color::Warn, "%sbytes that make no Modbus RTU frame: cut short, a wrong CRC, or another protocol", INDENT);
    return;
  }
  con::Print(Color::Rx, "%s RX  %s", rx.stamp.c_str(), HexText(rx.bytes).c_str());
  con::Print(Color::Rx, "%s%s", INDENT, note ? note : nt::Describe(rx.frame).c_str());
}

bool Talker::exchangeRtu(const Bytes& request, int timeoutMs, nt::Frame* answer)
{
  if (!m_port.isOpen()) {
    con::Print(Color::Warn, "The port is not open - use S to set it up.");
    return false;
  }

  std::vector<Rx> rx;
  rtuPoll(rx);     // show what is already waiting, so it is not mistaken for the answer
  for (const Rx& r : rx) showRtuRx(r);
  Sleep(2);        // Modbus RTU wants a pause of 3.5 characters between frames: 2 ms at 19200 baud

  showRtuTx(request, Stamp());
  std::string err;
  if (!m_port.write(request.data(), request.size(), err)) {
    con::Print(Color::Error, "%s", err.c_str());
    return false;
  }

  const nt::Frame         sent  = nt::Parse(request);
  const Clock::time_point start = Clock::now();
  while (m_port.isOpen() && MsSince(start) < timeoutMs) {
    rx.clear();
    rtuPoll(rx);
    for (const Rx& r : rx) {
      if (r.bytes == request) {   // no reply equals its request, so this is an echo
        showRtuRx(r, "echo of the request: the adapter loops it back");
        continue;
      }
      showRtuRx(r);
      if (r.isFrame && nt::Answers(r.frame, sent)) {
        if (answer) *answer = r.frame;
        return true;
      }
    }
    Sleep(2);
  }
  return false;
}

Talker::Step Talker::driveStep(const Bytes& request, nt::Frame* answer)
{
  nt::Frame reply;
  if (!exchangeRtu(request, RTU_TIMEOUT_MS, &reply)) {
    if (m_port.isOpen()) {
      con::Print(Color::Warn, "No answer from slave %d within %d ms.", nt::Parse(request).slave, RTU_TIMEOUT_MS);
    }
    return Step::Silent;
  }
  if (!nt::IsOk(reply)) return Step::Refused;
  if (answer) *answer = reply;
  return Step::Done;
}

// Writes one object. A drive that stays silent is asked up to tries times. True once it confirmed.
bool Talker::setObject(int slave, const nt::Object& o, long long value, int tries)
{
  for (int i = 0; i < tries; i++) {
    const Step s = driveStep(nt::BuildWriteObject(slave, o, value));
    if (s == Step::Done) return true;
    if (s == Step::Refused) return false;   // it answered: asking again will not change that
  }
  return false;
}

// Brakes a drive to a standstill along its deceleration ramp, then switches its power stage
// off. True when the drive confirmed the switch-off, which is what ends any rotation.
bool Talker::stopDrive(int slave)
{
  con::Print(Color::Normal, "Stopping slave %d.", slave);

  if (!setObject(slave, nt::OD_TARGET_VELOCITY, 0, 3)) {
    con::Print(Color::Warn, "The drive did not take target velocity 0; its target may still be %d.", TEST_RPM);
  } else {
    const Clock::time_point start = Clock::now();
    for (;;) {
      nt::Frame reply;
      long long v = 0;
      if (driveStep(nt::BuildReadObject(slave, nt::OD_VELOCITY_ACTUAL), &reply) != Step::Done) break;
      if (nt::ValueOf(reply, nt::OD_VELOCITY_ACTUAL, v) && std::llabs(v) <= STANDSTILL_RPM) break;
      if (MsSince(start) > TEST_STOP_MS) {
        con::Print(Color::Warn, "Still turning after %d s.", TEST_STOP_MS / 1000);
        break;
      }
      Sleep(250);
    }
  }

  return setObject(slave, nt::OD_CONTROL_WORD, nt::CW_SHUTDOWN, 3);
}

// Said when nothing answers a Modbus request.
void Talker::lineHint()
{
  const LineSettings& l = settings.line;
  if (!SameLine(l, NANOTEC_LINE)) {
    con::Print(Color::Warn,
               "The port is open at %d %d%c%d. The PD1-C drives ship set to 19200 8E1 and MetalJet opens "
               "them that way: a drive answers only at its own line setting, and the chiller on the same "
               "bus needs the same one. Change it with S.",
               l.baud, l.dataBits, l.parity, l.stopBits);
  } else {
    con::Print(Color::Warn,
               "The line equals the drives' factory setting. Check that the drive is powered, its slave id, "
               "and the RS-485 wiring: + and -, signal ground; try swapping + and -.");
  }
}

void Talker::scanBus()
{
  if (!m_port.isOpen()) {
    con::Print(Color::Warn, "The port is not open - use S to set it up.");
    return;
  }
  con::Print(Color::Normal,
             "Asking slave ids %d to %d, but not %d (the chiller), for their server id on %s at %d %d%c%d. "
             "Esc stops.",
             nt::MIN_SLAVE, nt::MAX_SLAVE, settings.address, settings.port.c_str(), settings.line.baud,
             settings.line.dataBits, settings.line.parity, settings.line.stopBits);
  poll();     // whatever the chiller sent meanwhile is shown as that
  m_rtu.reset();
  con::EscPressed();

  std::vector<int> found;
  std::vector<Rx>  rx;

  // Shows what arrived. A slave that answers, with data or with an error, is there. Returns
  // true when the slave being asked was among those that answered.
  auto take = [&](const Bytes* probe, const std::string& probeStamp, int asked) {
    bool hit = false;
    for (const Rx& r : rx) {
      if (probe && r.bytes == *probe) continue;   // the adapter's echo
      const bool answer = r.isFrame && (r.frame.kind == nt::Kind::ReportIdReply || r.frame.kind == nt::Kind::Exception ||
                                        r.frame.kind == nt::Kind::ObjectException);
      if (!answer) {
        showRtuRx(r);
        continue;
      }
      const int slave = r.frame.slave;
      if (slave == asked) hit = true;
      if (slave == settings.address || std::find(found.begin(), found.end(), slave) != found.end()) continue;
      found.push_back(slave);
      if (probe && slave == asked) showRtuTx(*probe, probeStamp);
      showRtuRx(r);
    }
    rx.clear();
    return hit;
  };

  int  probes  = 0;
  bool stopped = false;
  for (int id = nt::MIN_SLAVE; id <= nt::MAX_SLAVE; id++) {
    if (id == settings.address) continue;
    if (con::Quit() || con::EscPressed() || !m_port.isOpen()) {
      stopped = true;
      break;
    }

    const Bytes       probe = nt::BuildReportId(id);
    const std::string stamp = Stamp();
    std::string       err;
    if (!m_port.write(probe.data(), probe.size(), err)) {
      con::Print(Color::Error, "%s", err.c_str());
      return;
    }
    probes++;

    const Clock::time_point sent = Clock::now();
    bool                    hit  = false;
    while (!hit && m_port.isOpen() && MsSince(sent) < SCAN_TIMEOUT_MS) {
      rtuPoll(rx);
      hit = take(&probe, stamp, id);
      if (!hit) Sleep(2);
    }
    if (probes % 50 == 0) con::Print(Color::Normal, "  ... %d ids asked, %d answered so far", probes, int(found.size()));
  }

  // a slave slower than a probe answers while the next one is being asked; the last one has no next
  const Clock::time_point end = Clock::now();
  while (!stopped && m_port.isOpen() && MsSince(end) < SCAN_TIMEOUT_MS) {
    rtuPoll(rx);
    take(nullptr, std::string(), -1);
    Sleep(2);
  }

  std::sort(found.begin(), found.end());
  std::string list;
  for (int id : found) list += (list.empty() ? "" : ", ") + std::to_string(id);

  if (stopped) con::Print(Color::Warn, "Stopped after %d ids.", probes);
  if (!found.empty()) {
    con::Print(Color::Normal, "%d slave%s answered: %s", int(found.size()), found.size() == 1 ? "" : "s", list.c_str());
  } else if (!stopped) {
    con::Print(Color::Warn, "No slave answered.");
    lineHint();
  }
}

void Talker::spinDrive()
{
  if (!m_port.isOpen()) {
    con::Print(Color::Warn, "The port is not open - use S to set it up.");
    return;
  }

  char prompt[96];
  std::snprintf(prompt, sizeof prompt, "Nanotec slave id (%d-%d, Enter cancels): ", nt::MIN_SLAVE, nt::MAX_SLAVE);
  std::string text;
  if (!ask(prompt, text) || text.empty()) return;

  int slave = 0;
  if (!ToInt(text, slave) || slave < nt::MIN_SLAVE || slave > nt::MAX_SLAVE) {
    con::Print(Color::Warn, "Not a slave id from %d to %d.", nt::MIN_SLAVE, nt::MAX_SLAVE);
    return;
  }
  if (slave == settings.address) {
    con::Print(Color::Warn, "%d is the chiller's address (S). Give the slave id of a Nanotec drive.", slave);
    return;
  }
  poll();
  m_rtu.reset();
  con::EscPressed();

  // Is anything there, and is it a drive? Nothing is changed before both are known.
  nt::Frame reply;
  Step      step = driveStep(nt::BuildReportId(slave));
  if (step == Step::Silent) {
    lineHint();
    return;
  }
  step = driveStep(nt::BuildReadObject(slave, nt::OD_STATUS_WORD), &reply);
  if (step == Step::Refused) {
    con::Print(Color::Warn, "Slave %d has no status word, so it is no Nanotec drive. Nothing was changed.", slave);
  }
  if (step != Step::Done) return;

  long long status = 0;
  if (!nt::ValueOf(reply, nt::OD_STATUS_WORD, status)) {
    con::Print(Color::Warn, "Slave %d answers with no 2-byte status word, so it is no Nanotec drive. "
                            "Nothing was changed.", slave);
    return;
  }
  nt::State state = nt::StateOf(u16(status));
  if (state == nt::State::NotReady || state == nt::State::Fault || state == nt::State::FaultReaction) {
    con::Print(Color::Warn, "The drive is in the state \"%s\" and cannot be enabled. Nothing was changed.",
               nt::StateText(state));
    return;
  }

  con::Print(Color::Normal, "Turning slave %d at %d rpm for %d s, acceleration %d. Esc stops it earlier.", slave,
             TEST_RPM, TEST_RUN_MS / 1000, TEST_ACCEL);

  // MetalJet's sequence (NanotecLib.cpp, SetAcceleration and SetVelocity): profile velocity mode
  // and the ramps, then the target speed, then the power state machine up to "operation enabled".
  if (!(setObject(slave, nt::OD_MODE, nt::MODE_PROFILE_VELOCITY) &&
        setObject(slave, nt::OD_ACCELERATION, TEST_ACCEL) &&
        setObject(slave, nt::OD_DECELERATION, TEST_ACCEL) &&
        setObject(slave, nt::OD_QUICK_STOP, TEST_ACCEL))) {
    con::Print(Color::Warn, "The drive did not take its settings. It is not turning.");
    return;
  }

  // From here on the drive may be told to turn, so whatever happens it is stopped afterwards.
  bool ok = setObject(slave, nt::OD_TARGET_VELOCITY, TEST_RPM) &&
            setObject(slave, nt::OD_CONTROL_WORD, nt::CW_SHUTDOWN) &&
            setObject(slave, nt::OD_CONTROL_WORD, nt::CW_SWITCH_ON) &&
            setObject(slave, nt::OD_CONTROL_WORD, nt::CW_ENABLE);
  if (ok) {
    Sleep(50);
    ok = driveStep(nt::BuildReadObject(slave, nt::OD_STATUS_WORD), &reply) == Step::Done &&
         nt::ValueOf(reply, nt::OD_STATUS_WORD, status);
    if (ok) {
      state = nt::StateOf(u16(status));
      ok    = state == nt::State::OperationEnabled;
      if (!ok) {
        con::Print(Color::Warn, "The drive did not reach \"operation enabled\" but \"%s\". Is the motor supply on?",
                   nt::StateText(state));
      }
    }
  }

  long long peak    = 0;
  bool      escaped = false;
  if (ok) {
    const Clock::time_point start = Clock::now();
    Clock::time_point       next  = start;   // the speed is read once a second
    while (MsSince(start) < TEST_RUN_MS) {
      if (con::Quit() || con::EscPressed()) {
        escaped = true;
        break;
      }
      if (Clock::now() >= next) {
        next += std::chrono::seconds(1);
        long long v = 0;
        if (driveStep(nt::BuildReadObject(slave, nt::OD_VELOCITY_ACTUAL), &reply) != Step::Done) {
          ok = false;
          break;
        }
        if (nt::ValueOf(reply, nt::OD_VELOCITY_ACTUAL, v)) peak = std::max(peak, std::llabs(v));
      }
      Sleep(20);
    }
  }

  const bool stopped = stopDrive(slave);
  if (!stopped) {
    con::Print(Color::Error,
               "The drive did not confirm that its power stage is off: the motor may still be turning. "
               "Switch the drive off, or run T for this id again.");
  } else if (!ok) {
    con::Print(Color::Warn, "The test failed. The drive is stopped.");
  } else if (peak >= MIN_TURN_RPM) {
    con::Print(Color::Normal, "Slave %d turned, highest actual velocity %lld rpm for a target of %d, and is stopped.",
               slave, peak, TEST_RPM);
  } else if (escaped) {
    con::Print(Color::Normal, "Stopped at your request, highest actual velocity %lld rpm.", peak);
  } else {
    con::Print(Color::Warn, "Slave %d took every command but did not turn, highest actual velocity %lld rpm. "
                            "Is the motor connected?", slave, peak);
  }
}

static void PrintMenu(const Settings& s)
{
  con::Print(Color::Normal, "%s", "");
  con::Print(Color::Normal, "  %s", LineText(s).c_str());
  con::Print(Color::Normal, "  1  Read measured temperature      PV1");
  con::Print(Color::Normal, "  2  Read target temperature        SV1");
  con::Print(Color::Normal, "  3  Set target temperature         SV1");
  con::Print(Color::Normal, "  4  Read temperature offset        PVS");
  con::Print(Color::Normal, "  5  Set temperature offset         PVS");
  con::Print(Color::Normal, "  6  Read operation mode            _MD");
  con::Print(Color::Normal, "  7  Set operation mode (RUN/STOP)  _MD");
  con::Print(Color::Normal, "  8  Read alarms                    _AL");
  con::Print(Color::Normal, "  9  Store the set values           STR");
  con::Print(Color::Normal, "  C  Custom message (R/W, identifier, data)");
  con::Print(Color::Normal, "  H  Send raw hex bytes");
  con::Print(Color::Normal, "  W  Scan the bus for Modbus slave ids (all but the chiller's)");
  con::Print(Color::Normal, "  T  Test a Nanotec drive: ask its slave id, turn it for a few seconds");
  con::Print(Color::Normal, "  S  Settings (port, line, address, BCC)");
  con::Print(Color::Normal, "  Q  Quit");
}

void Talker::run()
{
  PrintMenu(settings);
  for (;;) {
    std::string cmd;
    if (!ask("choice (Enter = menu)> ", cmd)) return;
    if (cmd.empty() || cmd[0] == '?') {
      PrintMenu(settings);
      continue;
    }
    switch (std::tolower((unsigned char)cmd[0])) {
      case '1': readItem(ID_PV);         break;
      case '2': readItem(ID_SV);         break;
      case '3': writeTenths(ID_SV);      break;
      case '4': readItem(ID_OFFSET);     break;
      case '5': writeTenths(ID_OFFSET);  break;
      case '6': readItem(ID_MODE);       break;
      case '7': writeMode();             break;
      case '8': readItem(ID_ALARM);      break;
      case '9': store();                 break;
      case 'c': custom();                break;
      case 'h': rawHex();                break;
      case 'w': scanBus();               break;
      case 't': spinDrive();             break;
      case 's': configure();             break;
      case 'q': return;
      default:  con::Print(Color::Warn, "Unknown choice '%s'.", cmd.c_str());
    }
  }
}

// ---------------------------------------------------------------- start up

static void PrintUsage()
{
  std::printf(
      "usage: ChillTalker [COMx] [options]\n"
      "\n"
      "  --port COMx     serial port; without one the settings dialog asks for it\n"
      "  --baud N        2400, 4800, 9600, 19200 or 38400     (default 19200)\n"
      "  --data N        7 or 8 data bits                      (default 8)\n"
      "  --parity P      N, E or O                             (default N)\n"
      "  --stop N        1 or 2 stop bits                      (default 2)\n"
      "  --addr N        chiller address, 1 to 99              (default 1)\n"
      "  --bcc on|off    BCC check, as set on the chiller      (default off)\n"
      "  -h, --help      this text\n"
      "\n"
      "The defaults are the chiller's factory settings, except the baud rate (its factory speed is 9600).\n");

  std::string known;
  for (const std::string& p : ComPort::list()) known += (known.empty() ? "" : ", ") + p;
  std::printf("Serial ports found: %s\n", known.empty() ? "none" : known.c_str());
}

// Returns false after saying which argument cannot be used.
static bool ParseArgs(int argc, char** argv, Settings& s, bool& portGiven, bool& help)
{
  for (int i = 1; i < argc; i++) 
  {
    const std::string a = argv[i];
    if (a == "-h" || a == "--help" || a == "/?") 
    {
      help = true;
      continue;
    }

    bool (*apply)(const std::string&, Settings&) = nullptr;
    if      (a == "--port")   apply = SetPort;
    else if (a == "--baud")   apply = SetBaud;
    else if (a == "--data")   apply = SetDataBits;
    else if (a == "--parity") apply = SetParity;
    else if (a == "--stop")   apply = SetStopBits;
    else if (a == "--addr")   apply = SetAddress;
    else if (a == "--bcc")    apply = SetBcc;
    else if (a[0] != '-') 
    {             // a bare port name
      if (!SetPort(a, s)) 
      {
        con::Print(Color::Error, "'%s' is not a port name.", a.c_str());
        return false;
      }
      portGiven = true;
      continue;
    } 
    else 
    {
      con::Print(Color::Error, "Unknown option '%s'. Try --help.", a.c_str());
      return false;
    }

    if (i + 1 >= argc) {
      con::Print(Color::Error, "%s needs a value.", a.c_str());
      return false;
    }
    const std::string value = argv[++i];
    if (!apply(value, s)) {
      con::Print(Color::Error, "'%s' is not a valid value for %s.", value.c_str(), a.c_str());
      return false;
    }
    if (apply == SetPort) portGiven = true;
  }
  return true;
}

int main(int argc, char** argv)
{
  con::Init();

  Talker talker;
  bool   portGiven = false;
  bool   help      = false;
  if (!ParseArgs(argc, argv, talker.settings, portGiven, help)) return 2;
  if (help) 
  {
    PrintUsage();
    return 0;
  }

  con::Print(Color::Normal, "ChillTalker - serial console for the SMC Thermo-con HEF002 chiller");
  if (portGiven) talker.openPort();
  else           talker.configure();

  talker.run();
  talker.closePort();
  return 0;
}

#include "Profile.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

const char* PinTypeName(PinType t) {
  switch (t) {
    case PinType::AI: return "AI";
    case PinType::AO: return "AO";
    case PinType::DI: return "DI";
    case PinType::DO: return "DO";
  }
  return "?";
}

int PinTypeRegWidth(PinType t) {
  return (t == PinType::AI || t == PinType::AO) ? 16 : 8;
}

// ------------------------------------------------------------------ helpers

static std::string ReadWholeFile(const std::string& path, bool& ok) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { ok = false; return {}; }
  std::ostringstream ss;
  ss << in.rdbuf();
  ok = true;
  return ss.str();
}

// Look for name in dir, then in up to three parent directories.
static std::string FindConfigFile(const std::string& dir, const char* name) {
  std::error_code ec;
  fs::path        d(dir);
  for (int up = 0; up < 4 && !d.empty(); ++up) {
    fs::path cand = d / name;
    if (fs::exists(cand, ec) && fs::is_regular_file(cand, ec)) return cand.string();
    fs::path parent = d.parent_path();
    if (parent == d) break;
    d = parent;
  }
  return {};
}

static std::string StripComment(const std::string& line) {
  // The assignment lines we care about never contain a quoted '#'.
  size_t h = line.find('#');
  return h == std::string::npos ? line : line.substr(0, h);
}

static std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static bool IsIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
static bool IsIdentChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// ------------------------------------------------- integer expression evaluator
//
// Evaluates the subset of Python that the IoDefs constants are written in:
// integers, previously defined constant names, + - * // / % and parentheses.
// Anything else (calls, subscripts, strings, f-strings) makes eval() fail, and
// the caller simply skips that line.

namespace {

class ExprEval {
public:
  ExprEval(const std::string& text, const std::map<std::string, long long>& consts)
      : m_s(text), m_consts(consts) {}

  bool eval(long long& out) {
    m_i  = 0;
    m_ok = true;
    long long v = parseSum();
    if (!m_ok) return false;
    skipWs();
    if (m_i != m_s.size()) return false;   // trailing junk -> not a plain expression
    out = v;
    return true;
  }

private:
  void skipWs() {
    while (m_i < m_s.size() && (m_s[m_i] == ' ' || m_s[m_i] == '\t')) ++m_i;
  }

  long long parseSum() {
    long long v = parseProduct();
    for (;;) {
      skipWs();
      if (!m_ok || m_i >= m_s.size()) return v;
      char c = m_s[m_i];
      if (c == '+') { ++m_i; v += parseProduct(); continue; }
      if (c == '-') { ++m_i; v -= parseProduct(); continue; }
      return v;
    }
  }

  long long parseProduct() {
    long long v = parseUnary();
    for (;;) {
      skipWs();
      if (!m_ok || m_i >= m_s.size()) return v;
      if (m_s[m_i] == '*') {
        ++m_i;
        v *= parseUnary();
        continue;
      }
      if (m_s[m_i] == '/') {
        ++m_i;
        if (m_i < m_s.size() && m_s[m_i] == '/') ++m_i;   // Python floor division
        long long d = parseUnary();
        if (d == 0) { m_ok = false; return 0; }
        v /= d;
        continue;
      }
      if (m_s[m_i] == '%') {
        ++m_i;
        long long d = parseUnary();
        if (d == 0) { m_ok = false; return 0; }
        v %= d;
        continue;
      }
      return v;
    }
  }

  long long parseUnary() {
    skipWs();
    if (m_i < m_s.size() && m_s[m_i] == '-') { ++m_i; return -parseUnary(); }
    if (m_i < m_s.size() && m_s[m_i] == '+') { ++m_i; return parseUnary(); }
    return parseAtom();
  }

  long long parseAtom() {
    skipWs();
    if (m_i >= m_s.size()) { m_ok = false; return 0; }

    if (m_s[m_i] == '(') {
      ++m_i;
      long long v = parseSum();
      skipWs();
      if (m_i >= m_s.size() || m_s[m_i] != ')') { m_ok = false; return 0; }
      ++m_i;
      return v;
    }

    if (std::isdigit((unsigned char)m_s[m_i])) {
      const char* start = m_s.c_str() + m_i;
      char*       end   = nullptr;
      long long   v     = std::strtoll(start, &end, 0);   // 0 -> honours 0x prefixes
      m_i += size_t(end - start);
      return v;
    }

    if (IsIdentStart(m_s[m_i])) {
      size_t a = m_i;
      while (m_i < m_s.size() && IsIdentChar(m_s[m_i])) ++m_i;
      std::string name = m_s.substr(a, m_i - a);
      skipWs();
      // A call or an attribute reference is not a constant expression.
      if (m_i < m_s.size() && (m_s[m_i] == '(' || m_s[m_i] == '.' || m_s[m_i] == '[')) {
        m_ok = false;
        return 0;
      }
      auto it = m_consts.find(name);
      if (it == m_consts.end()) { m_ok = false; return 0; }
      return it->second;
    }

    m_ok = false;
    return 0;
  }

  const std::string&                     m_s;
  const std::map<std::string, long long>& m_consts;
  size_t                                 m_i  = 0;
  bool                                   m_ok = true;
};

// ------------------------------------------------------- python dict literal
//
// Parses the GPIO_Scheme literal: a dict of string -> dict of string -> dict of
// int -> int. Only what the scheme file actually uses is supported.

class SchemeParser {
public:
  SchemeParser(const std::string& text) : m_s(text) {}

  // Positions the cursor just past "<name> =" and parses the dict that follows.
  bool parseNamed(const std::string& name,
                  std::map<std::string, Profile::ConnectorMap>& out,
                  std::string& err) {
    size_t at = findAssignment(name);
    if (at == std::string::npos) {
      err = "\"" + name + "\" not found";
      return false;
    }
    m_i = at;
    skip();
    if (!expect('{')) { err = "expected '{' after " + name; return false; }

    for (;;) {
      skip();
      if (accept('}')) break;

      std::string key;
      if (!parseStringKey(key)) { err = "expected a quoted I/O type name"; return false; }
      skip();
      if (!expect(':')) { err = "expected ':' after \"" + key + "\""; return false; }

      Profile::ConnectorMap connectors;
      if (!parseConnectorDict(connectors, err)) return false;
      out[key] = connectors;

      skip();
      if (accept(',')) continue;
      skip();
      if (accept('}')) break;
      err = "expected ',' or '}' after the \"" + key + "\" section";
      return false;
    }
    return true;
  }

private:
  // Finds "name" used as an assignment target at the start of a line. Accepts a
  // PEP 484 annotation between the name and the '=', which some machine configs
  // carry: "GPIO_Scheme: GPIOSharedUtils.IOSchemeDict = { ... }".
  size_t findAssignment(const std::string& name) {
    size_t from = 0;
    while (true) {
      size_t p = m_s.find(name, from);
      if (p == std::string::npos) return std::string::npos;
      from = p + name.size();

      const bool atLineStart = (p == 0) || m_s[p - 1] == '\n' || m_s[p - 1] == '\r';
      if (!atLineStart) continue;

      size_t q = p + name.size();
      while (q < m_s.size() && (m_s[q] == ' ' || m_s[q] == '\t')) ++q;
      if (q >= m_s.size()) continue;

      if (m_s[q] == ':') {
        // Skip the annotation up to the '=' on the same line.
        ++q;
        while (q < m_s.size() && m_s[q] != '=' && m_s[q] != '\n') ++q;
        if (q >= m_s.size() || m_s[q] != '=') continue;
      } else if (m_s[q] != '=') {
        continue;
      }
      if (q + 1 < m_s.size() && m_s[q + 1] == '=') continue;   // a comparison
      return q + 1;
    }
  }

  void skip() {
    while (m_i < m_s.size()) {
      char c = m_s[m_i];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++m_i; continue; }
      if (c == '#') {
        while (m_i < m_s.size() && m_s[m_i] != '\n') ++m_i;
        continue;
      }
      break;
    }
  }

  bool expect(char c) {
    skip();
    if (m_i < m_s.size() && m_s[m_i] == c) { ++m_i; return true; }
    return false;
  }

  bool accept(char c) {
    skip();
    if (m_i < m_s.size() && m_s[m_i] == c) { ++m_i; return true; }
    return false;
  }

  bool parseStringKey(std::string& out) {
    skip();
    if (m_i >= m_s.size()) return false;
    char quote = m_s[m_i];
    if (quote != '\'' && quote != '"') return false;
    ++m_i;
    size_t a = m_i;
    while (m_i < m_s.size() && m_s[m_i] != quote) ++m_i;
    if (m_i >= m_s.size()) return false;
    out = m_s.substr(a, m_i - a);
    ++m_i;
    return true;
  }

  bool parseInt(long long& out) {
    skip();
    if (m_i >= m_s.size()) return false;
    bool neg = false;
    if (m_s[m_i] == '-') { neg = true; ++m_i; skip(); }
    if (m_i >= m_s.size() || !std::isdigit((unsigned char)m_s[m_i])) return false;
    const char* start = m_s.c_str() + m_i;
    char*       end   = nullptr;
    long long   v     = std::strtoll(start, &end, 0);
    m_i += size_t(end - start);
    out = neg ? -v : v;
    return true;
  }

  bool parseConnectorDict(Profile::ConnectorMap& out, std::string& err) {
    skip();
    if (!expect('{')) { err = "expected '{' for a connector dictionary"; return false; }
    for (;;) {
      skip();
      if (accept('}')) return true;

      std::string conn;
      if (!parseStringKey(conn)) { err = "expected a quoted connector name"; return false; }
      skip();
      if (!expect(':')) { err = "expected ':' after connector " + conn; return false; }

      Profile::PinMap pins;
      if (!parsePinDict(pins, err)) return false;
      // A connector can legitimately appear once per type only; merge anyway.
      for (const auto& kv : pins) out[conn][kv.first] = kv.second;

      skip();
      if (accept(',')) continue;
      skip();
      if (accept('}')) return true;
      err = "expected ',' or '}' after connector " + conn;
      return false;
    }
  }

  bool parsePinDict(Profile::PinMap& out, std::string& err) {
    skip();
    if (!expect('{')) { err = "expected '{' for a pin dictionary"; return false; }
    for (;;) {
      skip();
      if (accept('}')) return true;

      long long pin = 0, idx = 0;
      if (!parseInt(pin)) { err = "expected a pin number"; return false; }
      skip();
      if (!expect(':')) { err = "expected ':' after a pin number"; return false; }
      if (!parseInt(idx)) { err = "expected an index"; return false; }
      out[int(pin)] = int(idx);

      skip();
      if (accept(',')) continue;
      skip();
      if (accept('}')) return true;
      err = "expected ',' or '}' in a pin dictionary";
      return false;
    }
  }

  const std::string& m_s;
  size_t             m_i = 0;
};

}  // namespace

// ------------------------------------------------------------------ Profile

void Profile::clear() {
  m_loaded = false;
  m_scheme.clear();
  m_consts.clear();
  m_schemeFile.clear();
  m_ioDefsFile.clear();
  m_baseAI = m_baseAO = m_baseDI = m_baseDO = -1;
}

bool Profile::load(const std::string& dir, std::string& err) {
  clear();

  if (dir.empty()) {
    err = "No machine-configuration directory set.";
    return false;
  }
  std::error_code ec;
  if (!fs::is_directory(fs::path(dir), ec)) {
    err = "\"" + dir + "\" is not a directory.";
    return false;
  }

  const std::string schemePath = FindConfigFile(dir, "Mini_GPIO_Scheme.py");
  const std::string ioDefsPath = FindConfigFile(dir, "Mini_GPIO_IoDefs.py");
  if (schemePath.empty() || ioDefsPath.empty()) {
    err = "Mini_GPIO_Scheme.py and Mini_GPIO_IoDefs.py were not found in \"" + dir +
          "\" or its parent directories.";
    return false;
  }

  bool        ok       = false;
  std::string schemeTx = ReadWholeFile(schemePath, ok);
  if (!ok) { err = "Cannot read \"" + schemePath + "\"."; return false; }
  std::string ioDefsTx = ReadWholeFile(ioDefsPath, ok);
  if (!ok) { err = "Cannot read \"" + ioDefsPath + "\"."; return false; }

  // --- constants: evaluate every simple top-level assignment, in file order,
  // so later lines can refer to earlier ones exactly as Python would.
  {
    std::istringstream in(ioDefsTx);
    std::string        line;
    while (std::getline(in, line)) {
      std::string code = StripComment(line);
      if (code.empty()) continue;
      if (code[0] == ' ' || code[0] == '\t') continue;   // indented: not top level

      size_t eq = code.find('=');
      if (eq == std::string::npos) continue;
      if (eq + 1 < code.size() && code[eq + 1] == '=') continue;
      if (eq > 0 && (code[eq - 1] == '!' || code[eq - 1] == '<' || code[eq - 1] == '>' ||
                     code[eq - 1] == '+' || code[eq - 1] == '-' || code[eq - 1] == '*' ||
                     code[eq - 1] == '/')) {
        continue;
      }

      std::string lhs = Trim(code.substr(0, eq));
      std::string rhs = Trim(code.substr(eq + 1));
      if (lhs.empty() || rhs.empty()) continue;

      // Tolerate an annotated target: "NAME: int = 5".
      const size_t colon = lhs.find(':');
      if (colon != std::string::npos) lhs = Trim(lhs.substr(0, colon));
      if (lhs.empty()) continue;
      if (!IsIdentStart(lhs[0])) continue;
      bool plainName = true;
      for (char c : lhs) {
        if (!IsIdentChar(c)) { plainName = false; break; }
      }
      if (!plainName) continue;   // e.g. "GPIOUtils.X" or a tuple target

      long long value = 0;
      ExprEval  ev(rhs, m_consts);
      if (ev.eval(value)) m_consts[lhs] = value;
    }
  }

  auto constOr = [this](const char* name, int fallback) -> int {
    auto it = m_consts.find(name);
    return it == m_consts.end() ? fallback : int(it->second);
  };

  m_baseDO = constOr("ACTUATORS_REG_BASE", -1);
  m_baseDI = constOr("DIGITAL_SENSORS_STATUS_BASE", -1);
  m_baseAI = constOr("ANALOG_SENSORS_BASE", -1);
  m_baseAO = constOr("EXT_DAC_BUFFER_BASE", -1);

  if (m_baseAI < 0 || m_baseAO < 0 || m_baseDI < 0 || m_baseDO < 0) {
    err = "\"" + ioDefsPath +
          "\" does not define all four register bases (ACTUATORS_REG_BASE, "
          "DIGITAL_SENSORS_STATUS_BASE, ANALOG_SENSORS_BASE, EXT_DAC_BUFFER_BASE).";
    return false;
  }

  // --- scheme
  {
    SchemeParser sp(schemeTx);
    std::string  serr;
    if (!sp.parseNamed("GPIO_Scheme", m_scheme, serr)) {
      err = "Cannot parse GPIO_Scheme in \"" + schemePath + "\": " + serr;
      return false;
    }
  }

  int found = 0;
  for (const char* t : {"AI", "AO", "DI", "DO"}) {
    if (m_scheme.count(t)) ++found;
  }
  if (found == 0) {
    err = "GPIO_Scheme in \"" + schemePath + "\" has no AI/AO/DI/DO sections.";
    return false;
  }

  m_schemeFile = schemePath;
  m_ioDefsFile = ioDefsPath;
  m_loaded     = true;
  return true;
}

const Profile::ConnectorMap* Profile::connectorsFor(PinType t) const {
  auto it = m_scheme.find(PinTypeName(t));
  return it == m_scheme.end() ? nullptr : &it->second;
}

std::vector<std::string> Profile::connectors(PinType t) const {
  std::vector<std::string> out;
  const ConnectorMap*      cm = connectorsFor(t);
  if (!cm) return out;
  for (const auto& kv : *cm) out.push_back(kv.first);
  return out;
}

std::vector<int> Profile::pins(PinType t, const std::string& connector) const {
  std::vector<int>    out;
  const ConnectorMap* cm = connectorsFor(t);
  if (!cm) return out;
  auto it = cm->find(connector);
  if (it == cm->end()) return out;
  for (const auto& kv : it->second) out.push_back(kv.first);
  return out;
}

int Profile::index(PinType t, const std::string& connector, int pin) const {
  const ConnectorMap* cm = connectorsFor(t);
  if (!cm) return -1;
  auto ci = cm->find(connector);
  if (ci == cm->end()) return -1;
  auto pi = ci->second.find(pin);
  return pi == ci->second.end() ? -1 : pi->second;
}

int Profile::base(PinType t) const {
  switch (t) {
    case PinType::AI: return m_baseAI;
    case PinType::AO: return m_baseAO;
    case PinType::DI: return m_baseDI;
    case PinType::DO: return m_baseDO;
  }
  return -1;
}

int Profile::regAddr(PinType t, const std::string& connector, int pin) const {
  const int idx = index(t, connector, pin);
  const int b   = base(t);
  if (idx < 0 || b < 0) return -1;
  return b + idx;
}

int Profile::pinCount(PinType t) const {
  const ConnectorMap* cm = connectorsFor(t);
  if (!cm) return 0;
  int n = 0;
  for (const auto& kv : *cm) n += int(kv.second.size());
  return n;
}

std::string Profile::summary() const {
  if (!m_loaded) return "No machine configuration loaded.";

  char buf[1024];
  std::snprintf(buf, sizeof(buf),
                "Scheme:  %s\r\n"
                "IoDefs:  %s\r\n"
                "\r\n"
                "AI  base %5d  IR16   %3d pins on %2d connectors   read\r\n"
                "DI  base %5d  IR8    %3d pins on %2d connectors   read\r\n"
                "DO  base %5d  IR8    %3d pins on %2d connectors   read + write\r\n"
                "AO  base %5d  IR16   %3d channels on %2d connectors   write\r\n"
                "\r\n"
                "Register address = base + scheme[type][connector][pin]\r\n"
                "%d constants evaluated from IoDefs.",
                m_schemeFile.c_str(), m_ioDefsFile.c_str(),
                m_baseAI, pinCount(PinType::AI), int(connectors(PinType::AI).size()),
                m_baseDI, pinCount(PinType::DI), int(connectors(PinType::DI).size()),
                m_baseDO, pinCount(PinType::DO), int(connectors(PinType::DO).size()),
                m_baseAO, pinCount(PinType::AO), int(connectors(PinType::AO).size()),
                int(m_consts.size()));
  return buf;
}

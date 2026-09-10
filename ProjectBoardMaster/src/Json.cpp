#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace json {

// ---------------------------------------------------------------- writing

static void escapeTo(std::string& out, const std::string& s) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c & 0xFF);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

static void numberTo(std::string& out, double d) {
  if (d == std::floor(d) && std::fabs(d) < 1e15) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
    out += buf;
  } else {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.10g", d);
    out += buf;
  }
}

void Value::dumpTo(std::string& out, int indent, int depth) const {
  const std::string pad(indent ? size_t(indent) * (depth + 1) : 0, ' ');
  const std::string padEnd(indent ? size_t(indent) * depth : 0, ' ');
  const char* nl = indent ? "\n" : "";

  switch (m_type) {
    case Type::Null:   out += "null"; break;
    case Type::Bool:   out += m_bool ? "true" : "false"; break;
    case Type::Number: numberTo(out, m_num); break;
    case Type::String: escapeTo(out, m_str); break;
    case Type::Object: {
      if (m_obj.empty()) { out += "{}"; break; }
      out += "{"; out += nl;
      bool first = true;
      for (const auto& kv : m_obj) {
        if (!first) { out += ","; out += nl; }
        first = false;
        out += pad;
        escapeTo(out, kv.first);
        out += indent ? ": " : ":";
        kv.second.dumpTo(out, indent, depth + 1);
      }
      out += nl; out += padEnd; out += "}";
      break;
    }
    case Type::Array: {
      if (m_arr.empty()) { out += "[]"; break; }
      out += "["; out += nl;
      for (size_t i = 0; i < m_arr.size(); ++i) {
        if (i) { out += ","; out += nl; }
        out += pad;
        m_arr[i].dumpTo(out, indent, depth + 1);
      }
      out += nl; out += padEnd; out += "]";
      break;
    }
  }
}

std::string Value::dump(int indent) const {
  std::string out;
  dumpTo(out, indent, 0);
  return out;
}

// ---------------------------------------------------------------- parsing

namespace {

struct Parser {
  const std::string& s;
  size_t             i = 0;
  std::string        err;

  explicit Parser(const std::string& text) : s(text) {}

  void skip() {
    while (i < s.size()) {
      char c = s[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
      // Tolerate // and /* */ comments - handy for a hand-edited config file.
      if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
        while (i < s.size() && s[i] != '\n') ++i;
        continue;
      }
      if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
        i += 2;
        while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
        i = (i + 1 < s.size()) ? i + 2 : s.size();
        continue;
      }
      break;
    }
  }

  bool fail(const char* what) {
    if (err.empty()) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s at offset %zu", what, i);
      err = buf;
    }
    return false;
  }

  bool parseString(std::string& out) {
    if (i >= s.size() || s[i] != '"') return fail("expected string");
    ++i;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c != '\\') { out += c; continue; }
      if (i >= s.size()) return fail("truncated escape");
      char e = s[i++];
      switch (e) {
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case '/':  out += '/';  break;
        case 'b':  out += '\b'; break;
        case 'f':  out += '\f'; break;
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        case 'u': {
          if (i + 3 >= s.size()) return fail("truncated \\u escape");
          unsigned cp = std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
          i += 4;
          // Emit UTF-8 for the BMP range; surrogate pairs are not expected here.
          if (cp < 0x80) {
            out += char(cp);
          } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
          } else {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
          }
          break;
        }
        default: return fail("bad escape");
      }
    }
    if (i >= s.size()) return fail("unterminated string");
    ++i;  // closing quote
    return true;
  }

  bool parseValue(Value& out) {
    skip();
    if (i >= s.size()) return fail("unexpected end of input");
    char c = s[i];

    if (c == '{') {
      ++i;
      Object o;
      skip();
      if (i < s.size() && s[i] == '}') { ++i; out = Value(o); return true; }
      for (;;) {
        skip();
        std::string key;
        if (!parseString(key)) return false;
        skip();
        if (i >= s.size() || s[i] != ':') return fail("expected ':'");
        ++i;
        Value v;
        if (!parseValue(v)) return false;
        o[key] = v;
        skip();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') { ++i; break; }
        return fail("expected ',' or '}'");
      }
      out = Value(o);
      return true;
    }

    if (c == '[') {
      ++i;
      Array a;
      skip();
      if (i < s.size() && s[i] == ']') { ++i; out = Value(a); return true; }
      for (;;) {
        Value v;
        if (!parseValue(v)) return false;
        a.push_back(v);
        skip();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ']') { ++i; break; }
        return fail("expected ',' or ']'");
      }
      out = Value(a);
      return true;
    }

    if (c == '"') {
      std::string str;
      if (!parseString(str)) return false;
      out = Value(str);
      return true;
    }

    if (s.compare(i, 4, "true") == 0)  { i += 4; out = Value(true);  return true; }
    if (s.compare(i, 5, "false") == 0) { i += 5; out = Value(false); return true; }
    if (s.compare(i, 4, "null") == 0)  { i += 4; out = Value();      return true; }

    // number
    {
      const char* start = s.c_str() + i;
      char*       end   = nullptr;
      double      d     = std::strtod(start, &end);
      if (end == start) return fail("expected value");
      i += size_t(end - start);
      out = Value(d);
      return true;
    }
  }
};

}  // namespace

bool parse(const std::string& text, Value& out, std::string& err) {
  Parser p(text);
  if (!p.parseValue(out)) {
    err = p.err;
    return false;
  }
  p.skip();
  if (p.i != text.size()) {
    err = "trailing characters after the top-level value";
    return false;
  }
  err.clear();
  return true;
}

}  // namespace json

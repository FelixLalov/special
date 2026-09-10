// Minimal JSON value / parser / writer. No third-party dependencies.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace json {

class Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

enum class Type { Null, Bool, Number, String, Object, Array };

class Value {
public:
  Value() : m_type(Type::Null), m_bool(false), m_num(0) {}
  Value(bool b) : m_type(Type::Bool), m_bool(b), m_num(0) {}
  Value(double d) : m_type(Type::Number), m_bool(false), m_num(d) {}
  Value(int i) : m_type(Type::Number), m_bool(false), m_num(double(i)) {}
  Value(const char* s) : m_type(Type::String), m_bool(false), m_num(0), m_str(s ? s : "") {}
  Value(const std::string& s) : m_type(Type::String), m_bool(false), m_num(0), m_str(s) {}
  Value(const Object& o) : m_type(Type::Object), m_bool(false), m_num(0), m_obj(o) {}
  Value(const Array& a) : m_type(Type::Array), m_bool(false), m_num(0), m_arr(a) {}

  Type type() const { return m_type; }
  bool isNull() const { return m_type == Type::Null; }
  bool isObject() const { return m_type == Type::Object; }
  bool isArray() const { return m_type == Type::Array; }

  // Lenient accessors: return the fallback when the type or key does not match.
  bool        asBool(bool def = false) const { return m_type == Type::Bool ? m_bool : def; }
  int         asInt(int def = 0) const { return m_type == Type::Number ? int(m_num) : def; }
  double      asDouble(double def = 0) const { return m_type == Type::Number ? m_num : def; }
  std::string asString(const std::string& def = std::string()) const {
    return m_type == Type::String ? m_str : def;
  }

  const Array&  arr() const { return m_arr; }
  const Object& obj() const { return m_obj; }

  bool has(const std::string& key) const {
    return m_type == Type::Object && m_obj.find(key) != m_obj.end();
  }
  const Value& operator[](const std::string& key) const {
    static const Value kNull;
    if (m_type != Type::Object) return kNull;
    auto it = m_obj.find(key);
    return it == m_obj.end() ? kNull : it->second;
  }

  // Builders
  void set(const std::string& key, const Value& v) {
    m_type = Type::Object;
    m_obj[key] = v;
  }
  void push(const Value& v) {
    m_type = Type::Array;
    m_arr.push_back(v);
  }

  std::string dump(int indent = 2) const;

private:
  void dumpTo(std::string& out, int indent, int depth) const;

  Type        m_type;
  bool        m_bool;
  double      m_num;
  std::string m_str;
  Object      m_obj;
  Array       m_arr;
};

// Returns false and fills err on malformed input.
bool parse(const std::string& text, Value& out, std::string& err);

}  // namespace json

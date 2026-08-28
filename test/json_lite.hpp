#pragma once

// A dependency-free reader for the small JSON subset used by golden/*.json.
//
// Supported: objects, arrays, numbers, strings (with \" \\ \/ \b \f \n \r \t
// escapes), true, false, null.  Not supported: \uXXXX.  That is all the
// generator ever emits.
//
// Arrays whose first element is a number are parsed straight into a
// std::vector<double> -- the vectors are mostly long flat number arrays, and
// boxing 100k values would be silly.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace jsonlite {

struct Value;

struct Member;

struct Value {
  enum class Type { Null, Bool, Number, String, Array, Numbers, Object };

  Type type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  std::string string;
  std::vector<Value> array;    // Type::Array
  std::vector<double> numbers; // Type::Numbers
  std::vector<Member> object;  // Type::Object

  bool is_null() const { return type == Type::Null; }

  const Value *find(const char *key) const;

  // Accessors that abort with a clear message rather than returning garbage;
  // this only ever reads data committed in this repository.
  const Value &at(const char *key) const;
  double num(const char *key) const;
  long integer(const char *key) const;
  long integer(const char *key, long fallback) const;
  const std::string &str(const char *key) const;
  const std::vector<double> &nums(const char *key) const;
};

struct Member {
  std::string key;
  Value value;
};

inline const Value *Value::find(const char *key) const {
  if (type != Type::Object)
    return nullptr;
  for (const Member &m : object)
    if (m.key == key)
      return &m.value;
  return nullptr;
}

[[noreturn]] inline void fail(const std::string &msg) {
  std::fprintf(stderr, "json_lite: %s\n", msg.c_str());
  std::exit(1);
}

inline const Value &Value::at(const char *key) const {
  const Value *v = find(key);
  if (!v)
    fail(std::string("missing key \"") + key + "\"");
  return *v;
}

inline double Value::num(const char *key) const {
  const Value &v = at(key);
  if (v.type != Type::Number)
    fail(std::string("key \"") + key + "\" is not a number");
  return v.number;
}

inline long Value::integer(const char *key) const {
  return static_cast<long>(num(key));
}

inline long Value::integer(const char *key, long fallback) const {
  const Value *v = find(key);
  return v && v->type == Type::Number ? static_cast<long>(v->number) : fallback;
}

inline const std::string &Value::str(const char *key) const {
  const Value &v = at(key);
  if (v.type != Type::String)
    fail(std::string("key \"") + key + "\" is not a string");
  return v.string;
}

inline const std::vector<double> &Value::nums(const char *key) const {
  const Value &v = at(key);
  if (v.type == Type::Numbers)
    return v.numbers;
  if (v.type == Type::Array && v.array.empty()) {
    static const std::vector<double> kEmpty;
    return kEmpty; // "[]" carries no type information
  }
  fail(std::string("key \"") + key + "\" is not an array of numbers");
}

namespace detail {

struct Parser {
  const char *p = nullptr;
  const char *end = nullptr;
  std::string where;

  [[noreturn]] void die(const char *what) const {
    fail(where + ": " + what + " at byte " +
         std::to_string(static_cast<long>(p - begin)));
  }
  const char *begin = nullptr;

  void skip_ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
      ++p;
  }

  char peek() {
    skip_ws();
    if (p >= end)
      die("unexpected end of input");
    return *p;
  }

  void expect(char c) {
    if (peek() != c)
      die("unexpected character");
    ++p;
  }

  bool literal(const char *lit) {
    const size_t n = std::string(lit).size();
    if (static_cast<size_t>(end - p) < n)
      return false;
    for (size_t i = 0; i < n; ++i)
      if (p[i] != lit[i])
        return false;
    p += n;
    return true;
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    while (true) {
      if (p >= end)
        die("unterminated string");
      const char c = *p++;
      if (c == '"')
        break;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (p >= end)
        die("unterminated escape");
      switch (*p++) {
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      case '/': out.push_back('/'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      default: die("unsupported escape");
      }
    }
    return out;
  }

  double parse_number() {
    char *stop = nullptr;
    const double d = std::strtod(p, &stop);
    if (stop == p)
      die("malformed number");
    p = stop;
    return d;
  }

  void parse_value(Value &out) {
    const char c = peek();
    if (c == '{') {
      out.type = Value::Type::Object;
      ++p;
      if (peek() == '}') { ++p; return; }
      while (true) {
        skip_ws();
        Member m;
        m.key = parse_string();
        expect(':');
        parse_value(m.value);
        out.object.push_back(std::move(m));
        const char d = peek();
        ++p;
        if (d == '}') return;
        if (d != ',') die("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++p;
      if (peek() == ']') {
        ++p;
        out.type = Value::Type::Array;
        return;
      }
      const char first = peek();
      if (first == '-' || (first >= '0' && first <= '9')) {
        out.type = Value::Type::Numbers;
        while (true) {
          skip_ws();
          out.numbers.push_back(parse_number());
          const char d = peek();
          ++p;
          if (d == ']') return;
          if (d != ',') die("expected ',' or ']'");
        }
      }
      out.type = Value::Type::Array;
      while (true) {
        Value v;
        parse_value(v);
        out.array.push_back(std::move(v));
        const char d = peek();
        ++p;
        if (d == ']') return;
        if (d != ',') die("expected ',' or ']'");
      }
    }
    if (c == '"') {
      out.type = Value::Type::String;
      out.string = parse_string();
      return;
    }
    if (literal("true")) { out.type = Value::Type::Bool; out.boolean = true; return; }
    if (literal("false")) { out.type = Value::Type::Bool; out.boolean = false; return; }
    if (literal("null")) { out.type = Value::Type::Null; return; }
    out.type = Value::Type::Number;
    out.number = parse_number();
  }
};

} // namespace detail

inline Value parse(const std::string &text, const std::string &where) {
  detail::Parser parser;
  parser.begin = parser.p = text.c_str();
  parser.end = text.c_str() + text.size();
  parser.where = where;
  Value root;
  parser.parse_value(root);
  return root;
}

inline Value parse_file(const std::string &path) {
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    fail("cannot open " + path);
  std::string text;
  char buf[1 << 16];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
    text.append(buf, n);
  std::fclose(f);
  return parse(text, path);
}

} // namespace jsonlite

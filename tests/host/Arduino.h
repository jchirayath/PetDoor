// Arduino.h — a host shim, just large enough to compile petdoor/beacon.cpp off
// the chip.
//
// beacon.* is pure functions over advertisement bytes with no BLE stack
// dependency, which is what makes it testable at all — but it still takes
// Arduino `String`, so building it on a laptop needs this much and no more.
//
// Deliberately NOT a general Arduino emulator. It implements only the String
// members beacon.cpp actually uses, so that a test cannot accidentally depend
// on behaviour the real core does not have. If a new call appears in
// beacon.cpp, the host build fails loudly and this file grows by one method.
//
// THE ONE SEMANTIC THAT MATTERS: the real Arduino String carries an explicit
// length and is binary-safe — `String(buf, len)` keeps embedded NUL bytes,
// while `String(const char *)` stops at the first one. std::string behaves the
// same way, so this shim reproduces the distinction faithfully. That is the
// whole point: the bug these tests exist to pin lived in the gap between those
// two constructors.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <cctype>
#include <cstring>
#include <string>

class String {
 public:
  String() = default;
  String(const char *s) : s_(s ? s : "") {}                       // stops at NUL
  String(const char *s, unsigned int len) : s_(s, len) {}          // binary safe
  String(const uint8_t *s, unsigned int len)
      : s_(reinterpret_cast<const char *>(s), len) {}              // binary safe

  void reserve(size_t n) { s_.reserve(n); }
  const char *c_str() const { return s_.c_str(); }
  unsigned int length() const { return static_cast<unsigned int>(s_.size()); }

  String &operator+=(char c) { s_ += c; return *this; }
  String &operator+=(const char *s) { if (s) s_ += s; return *this; }
  char operator[](size_t i) const { return s_[i]; }

  void toLowerCase() {
    for (char &c : s_) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  int indexOf(const char *needle) const {
    const size_t at = s_.find(needle);
    return at == std::string::npos ? -1 : static_cast<int>(at);
  }
  bool startsWith(const char *prefix) const {
    return s_.rfind(prefix, 0) == 0;
  }

  bool operator==(const char *other) const { return s_ == (other ? other : ""); }
  const std::string &std_str() const { return s_; }   // tests only

 private:
  std::string s_;
};

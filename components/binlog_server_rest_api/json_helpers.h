/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef BINLOG_SERVER_REST_API_JSON_HELPERS_H
#define BINLOG_SERVER_REST_API_JSON_HELPERS_H

#include <sstream>
#include <string>
#include <vector>

namespace rest_api {
namespace json {

inline std::string escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

class Object {
 public:
  Object &add(const char *key, const std::string &val) {
    add_comma();
    buf_ << '"' << key << "\":\"" << escape(val) << '"';
    return *this;
  }
  Object &add(const char *key, const char *val) {
    return add(key, std::string(val ? val : ""));
  }
  Object &add(const char *key, long long val) {
    add_comma();
    buf_ << '"' << key << "\":" << val;
    return *this;
  }
  Object &add(const char *key, unsigned long long val) {
    add_comma();
    buf_ << '"' << key << "\":" << val;
    return *this;
  }
  Object &add(const char *key, int val) { return add(key, (long long)val); }
  Object &add(const char *key, bool val) {
    add_comma();
    buf_ << '"' << key << "\":" << (val ? "true" : "false");
    return *this;
  }
  Object &add_raw(const char *key, const std::string &raw_json) {
    add_comma();
    buf_ << '"' << key << "\":" << raw_json;
    return *this;
  }
  std::string str() const { return "{" + buf_.str() + "}"; }

 private:
  void add_comma() {
    if (count_++ > 0) buf_ << ',';
  }
  std::ostringstream buf_;
  int count_{0};
};

class Array {
 public:
  Array &add(const std::string &json_element) {
    if (count_++ > 0) buf_ << ',';
    buf_ << json_element;
    return *this;
  }
  Array &add_string(const std::string &val) {
    if (count_++ > 0) buf_ << ',';
    buf_ << '"' << escape(val) << '"';
    return *this;
  }
  std::string str() const { return "[" + buf_.str() + "]"; }
  bool empty() const { return count_ == 0; }

 private:
  std::ostringstream buf_;
  int count_{0};
};

}  // namespace json
}  // namespace rest_api

#endif

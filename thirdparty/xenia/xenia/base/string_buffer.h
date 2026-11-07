#pragma once
#include <format>
#include <string>
#include <string_view>
namespace xe {
class StringBuffer {
 public:
  explicit StringBuffer(size_t = 0) {}
  void Reset() { s_.clear(); }
  void Append(char c) { s_ += c; }
  void Append(char c, size_t n) { s_.append(n, c); }
  void Append(std::string_view v) { s_ += v; }
  template <typename... Args> void AppendFormat(std::string_view f, Args&&... args) {
    s_ += std::vformat(f, std::make_format_args(args...));
  }
  void AppendVarargs(const char*, va_list) {}
  void AppendBytes(const uint8_t* b, size_t n) { s_.append((const char*)b, n); }
  std::string to_string() const { return s_; }
  std::string_view to_string_view() const { return s_; }
  const char* buffer() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }
 private:
  std::string s_;
};
}

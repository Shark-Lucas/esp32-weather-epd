#pragma once
#include <string>
#include <cstring>
#include <algorithm>
class WiFiClient
{
public:
  explicit WiFiClient(std::string value) : value_(std::move(value)) {}
  int read() { return pos_ < value_.size() ? static_cast<unsigned char>(value_[pos_++]) : -1; }
  size_t readBytes(char *buffer, size_t length)
  {
    const size_t count = std::min(length, value_.size() - pos_);
    std::memcpy(buffer, value_.data() + pos_, count); pos_ += count; return count;
  }
private:
  std::string value_;
  size_t pos_ = 0;
};

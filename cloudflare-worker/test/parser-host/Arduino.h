#pragma once
#include <string>
#include <cstdint>
#include <cstddef>
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 0
class String : public std::string
{
public:
  using std::string::string;
  String() = default;
  String(const char *value) : std::string(value ? value : "") {}
  bool isEmpty() const { return empty(); }
};
#include <ArduinoJson.h>
inline void convertFromJson(ArduinoJson::JsonVariantConst source, String &target)
{
  target = String(source.as<const char *>());
}
struct HostSerial { template<class... T> void printf(const char *, T...) {} };
static HostSerial Serial;

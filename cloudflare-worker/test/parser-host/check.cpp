// Compile the real firmware parsers, replacing only Arduino IO with host shims.
#include <cassert>
#include <fstream>
#include <sstream>
#include "api/openweather_v4.h"

std::string file(const std::string &path)
{
  std::ifstream input(path); assert(input.good());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string fingerprint(owm_resp_onecall_t &r, owm_resp_air_pollution_t &air)
{
  std::ostringstream o;
  o.precision(17);
#define FIELD(x) o << (x) << '|'
  FIELD(r.lat); FIELD(r.lon); FIELD(r.timezone); FIELD(r.timezone_offset);
  const auto &c = r.current;
  FIELD(c.dt); FIELD(c.sunrise); FIELD(c.sunset); FIELD(c.temp); FIELD(c.feels_like);
  FIELD(c.pressure); FIELD(c.humidity); FIELD(c.dew_point); FIELD(c.clouds); FIELD(c.uvi);
  FIELD(c.visibility); FIELD(c.wind_speed); FIELD(c.wind_gust); FIELD(c.wind_deg);
  FIELD(c.rain_1h); FIELD(c.snow_1h); FIELD(c.weather.id); FIELD(c.weather.main);
  FIELD(c.weather.description); FIELD(c.weather.icon);
  for (int i = 0; i < 24; ++i) {
    const auto &h = r.hourly[i];
    FIELD(h.dt); FIELD(h.temp); FIELD(h.feels_like); FIELD(h.pressure); FIELD(h.humidity);
    FIELD(h.dew_point); FIELD(h.clouds); FIELD(h.uvi); FIELD(h.visibility);
    FIELD(h.wind_speed); FIELD(h.wind_gust); FIELD(h.wind_deg); FIELD(h.pop);
    FIELD(h.rain_1h); FIELD(h.snow_1h);
  }
  for (const auto &d : r.daily) {
    FIELD(d.dt); FIELD(d.sunrise); FIELD(d.sunset); FIELD(d.moonrise); FIELD(d.moonset); FIELD(d.moon_phase);
    FIELD(d.temp.morn); FIELD(d.temp.day); FIELD(d.temp.eve); FIELD(d.temp.night); FIELD(d.temp.min); FIELD(d.temp.max);
    FIELD(d.feels_like.morn); FIELD(d.feels_like.day); FIELD(d.feels_like.eve); FIELD(d.feels_like.night);
    FIELD(d.pressure); FIELD(d.humidity); FIELD(d.dew_point); FIELD(d.clouds); FIELD(d.uvi);
    FIELD(d.visibility); FIELD(d.wind_speed); FIELD(d.wind_gust); FIELD(d.wind_deg);
    FIELD(d.pop); FIELD(d.rain); FIELD(d.snow); FIELD(d.weather.id);
    FIELD(d.weather.main); FIELD(d.weather.description); FIELD(d.weather.icon);
  }
  for (const auto &a : r.alerts) { FIELD(a.sender_name); FIELD(a.event); FIELD(a.start); FIELD(a.end); }
  FIELD(air.coord.lat); FIELD(air.coord.lon);
  for (int i = 0; i < 24; ++i) {
    FIELD(air.dt[i]); FIELD(air.main_aqi[i]); FIELD(air.components.co[i]); FIELD(air.components.no[i]);
    FIELD(air.components.no2[i]); FIELD(air.components.o3[i]); FIELD(air.components.so2[i]);
    FIELD(air.components.pm2_5[i]); FIELD(air.components.pm10[i]); FIELD(air.components.nh3[i]);
  }
#undef FIELD
  return o.str();
}

std::string parse(const std::string &base)
{
  owm_resp_onecall_t r{};
  owm_resp_air_pollution_t air{};
  std::vector<String> ids;
  WiFiClient current(file(base + "-current.json"));
  assert(!deserializeOneCallV4Current(current, r, ids));
  size_t count = 0;
  int64_t last = 0;
  WiFiClient first(file(base + "-hourly1.json"));
  assert(!deserializeOneCallV4Hourly(first, r, 0, count, last, ids)); assert(count == 20);
  WiFiClient second(file(base + "-hourly2.json"));
  assert(!deserializeOneCallV4Hourly(second, r, 20, count, last, ids)); assert(count == 4);
  WiFiClient daily(file(base + "-daily.json"));
  assert(!deserializeOneCallV4Daily(daily, r, count, ids)); assert(count == 8);
  WiFiClient alert(file(base + "-alert.json"));
  assert(!deserializeOneCallV4Alert(alert, r));
  WiFiClient pollution(file(base + "-air.json"));
  assert(!deserializeAirQuality(pollution, air));
  assert(ids.size() == 1);
  return fingerprint(r, air);
}

int main(int argc, char **argv)
{
  assert(argc == 3);
  assert(parse(argv[1]) == parse(argv[2]));
}

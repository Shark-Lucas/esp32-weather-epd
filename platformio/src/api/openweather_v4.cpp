/* Independent One Call 4.0 response adapter. */

#include <algorithm>
#include <vector>

#include <Arduino.h>
#include <ArduinoJson.h>

#include "api/openweather_v4.h"
#include "api_response.h"
#include "config.h"

namespace
{
void printJsonDebug(const JsonDocument &doc)
{
#if DEBUG_LEVEL >= 1
  Serial.println("[owm4] doc.overflowed(): " + String(doc.overflowed()));
#endif
#if DEBUG_LEVEL >= 2
  serializeJsonPretty(doc, Serial);
#endif
}

void addRootFilter(JsonDocument &filter)
{
  filter["lat"] = true;
  filter["lon"] = true;
  filter["timezone"] = true;
  filter["timezone_offset"] = true;
}

void addWeatherFilter(JsonObject filter)
{
  filter["id"] = true;
  filter["main"] = true;
  filter["description"] = true;
  filter["icon"] = true;
}

void addCurrentFilter(JsonDocument &filter)
{
  addRootFilter(filter);
  JsonObject item = filter["data"][0].to<JsonObject>();
  item["dt"] = true;
  item["sunrise"] = true;
  item["sunset"] = true;
  item["temp"] = true;
  item["feels_like"] = true;
  item["pressure"] = true;
  item["humidity"] = true;
  item["dew_point"] = true;
  item["uvi"] = true;
  item["clouds"] = true;
  item["visibility"] = true;
  item["wind_speed"] = true;
  item["wind_gust"] = true;
  item["wind_deg"] = true;
  item["rain"]["1h"] = true;
  item["snow"]["1h"] = true;
  addWeatherFilter(item["weather"][0].to<JsonObject>());
#if DISPLAY_ALERTS
  item["alerts"] = true;
#endif
}

void addHourlyFilter(JsonDocument &filter)
{
  addRootFilter(filter);
  JsonObject item = filter["data"][0].to<JsonObject>();
  item["dt"] = true;
  item["temp"] = true;
  item["feels_like"] = true;
  item["pressure"] = true;
  item["humidity"] = true;
  item["dew_point"] = true;
  item["uvi"] = true;
  item["clouds"] = true;
  item["visibility"] = true;
  item["wind_speed"] = true;
  item["wind_gust"] = true;
  item["wind_deg"] = true;
  item["pop"] = true;
  item["rain"]["1h"] = true;
  item["snow"]["1h"] = true;
#if DISPLAY_ALERTS
  item["alerts"] = true;
#endif
}

void addDailyFilter(JsonDocument &filter)
{
  addRootFilter(filter);
  JsonObject item = filter["data"][0].to<JsonObject>();
  item["dt"] = true;
  item["sunrise"] = true;
  item["sunset"] = true;
  item["moonrise"] = true;
  item["moonset"] = true;
  item["moon_phase"] = true;
  item["temp"] = true;
  item["feels_like"] = true;
  item["pressure"] = true;
  item["humidity"] = true;
  item["dew_point"] = true;
  item["uvi"] = true;
  item["clouds"] = true;
  item["visibility"] = true;
  item["wind_speed"] = true;
  item["wind_gust"] = true;
  item["wind_deg"] = true;
  item["pop"] = true;
  item["rain"] = true;
  item["snow"] = true;
  addWeatherFilter(item["weather"][0].to<JsonObject>());
#if DISPLAY_ALERTS
  item["alerts"] = true;
#endif
}

DeserializationError deserializeFiltered(WiFiClient &json,
                                         JsonDocument &doc,
                                         JsonDocument &filter)
{
  DeserializationError error = deserializeJson(
      doc, json, DeserializationOption::Filter(filter));
  printJsonDebug(doc);
  return error;
}

void copyRootFields(JsonDocument &doc, owm_resp_onecall_t &response)
{
  response.lat = doc["lat"].as<float>();
  response.lon = doc["lon"].as<float>();
  response.timezone = doc["timezone"] | "";
  response.timezone_offset = doc["timezone_offset"].as<int>();
}

void copyWeather(JsonObject item, owm_weather_t &weather)
{
  JsonObject source = item["weather"][0];
  weather.id = source["id"].as<int>();
  weather.main = source["main"] | "";
  weather.description = source["description"] | "";
  weather.icon = source["icon"] | "";
}

// One Call 4.0 documents weather[] for daily records, but the production
// service can currently return `weather: null` for otherwise complete daily
// forecasts. Keep that upstream omission inside the v4 adapter and derive a
// conservative OpenWeather condition from the daily aggregate. The shared
// response model and the independent v3 parser remain unchanged.
void synthesizeDailyWeather(owm_daily_t &daily)
{
  if (daily.weather.id > 0)
  {
    return;
  }

  if (daily.snow > 0.0f)
  {
    daily.weather.id = 601;
    daily.weather.main = "Snow";
    daily.weather.description = "snow";
    daily.weather.icon = "13d";
    return;
  }

  if (daily.rain > 0.0f)
  {
    daily.weather.id = daily.rain < 2.5f ? 500
                     : daily.rain < 7.5f ? 501 : 502;
    daily.weather.main = "Rain";
    daily.weather.description = "rain";
    daily.weather.icon = "10d";
    return;
  }

  daily.weather.main = daily.clouds <= 10 ? "Clear" : "Clouds";
  if (daily.clouds <= 10)
  {
    daily.weather.id = 800;
    daily.weather.description = "clear sky";
    daily.weather.icon = "01d";
  }
  else if (daily.clouds <= 25)
  {
    daily.weather.id = 801;
    daily.weather.description = "few clouds";
    daily.weather.icon = "02d";
  }
  else if (daily.clouds <= 50)
  {
    daily.weather.id = 802;
    daily.weather.description = "scattered clouds";
    daily.weather.icon = "03d";
  }
  else if (daily.clouds <= 84)
  {
    daily.weather.id = 803;
    daily.weather.description = "broken clouds";
    daily.weather.icon = "04d";
  }
  else
  {
    daily.weather.id = 804;
    daily.weather.description = "overcast clouds";
    daily.weather.icon = "04d";
  }
}

float precipitationValue(JsonVariant value)
{
  if (value.is<JsonObject>())
  {
    return value["1h"].as<float>();
  }
  return value.as<float>();
}

void collectAlertIds(JsonVariant value, std::vector<String> &alertIds)
{
#if DISPLAY_ALERTS
  for (JsonVariant idValue : value.as<JsonArray>())
  {
    const String id = idValue.as<String>();
    if (id.isEmpty())
    {
      continue;
    }

    bool duplicate = false;
    for (const String &existing : alertIds)
    {
      if (existing == id)
      {
        duplicate = true;
        break;
      }
    }
    if (!duplicate)
    {
      alertIds.push_back(id);
      if (alertIds.size() >= OWM_NUM_ALERTS)
      {
        return;
      }
    }
  }
#else
  (void)value;
  (void)alertIds;
#endif
}
} // namespace

DeserializationError deserializeOneCallV4Current(
    WiFiClient &json, owm_resp_onecall_t &response,
    std::vector<String> &alertIds)
{
  JsonDocument filter;
  addCurrentFilter(filter);
  JsonDocument doc;
  DeserializationError error = deserializeFiltered(json, doc, filter);
  if (error)
  {
    return error;
  }

  JsonArray data = doc["data"].as<JsonArray>();
  if (data.size() != 1)
  {
    return DeserializationError::InvalidInput;
  }

  copyRootFields(doc, response);
  response.current = owm_current_t{};
  JsonObject current = data[0];
  response.current.dt = current["dt"].as<int64_t>();
  response.current.sunrise = current["sunrise"].as<int64_t>();
  response.current.sunset = current["sunset"].as<int64_t>();
  response.current.temp = current["temp"].as<float>();
  response.current.feels_like = current["feels_like"].as<float>();
  response.current.pressure = current["pressure"].as<int>();
  response.current.humidity = current["humidity"].as<int>();
  response.current.dew_point = current["dew_point"].as<float>();
  response.current.clouds = current["clouds"].as<int>();
  response.current.uvi = current["uvi"].as<float>();
  response.current.visibility = current["visibility"].as<int>();
  response.current.wind_speed = current["wind_speed"].as<float>();
  response.current.wind_gust = current["wind_gust"].as<float>();
  response.current.wind_deg = current["wind_deg"].as<int>();
  response.current.rain_1h = precipitationValue(current["rain"]);
  response.current.snow_1h = precipitationValue(current["snow"]);
  copyWeather(current, response.current.weather);
  collectAlertIds(current["alerts"], alertIds);

  Serial.printf("[owm4] Current dt=%lld temp=%.2f weather=%d alerts=%u\n",
                static_cast<long long>(response.current.dt),
                response.current.temp, response.current.weather.id,
                static_cast<unsigned>(alertIds.size()));

  if (response.current.dt <= 0 || response.current.temp <= 0
      || response.current.weather.id <= 0)
  {
    return DeserializationError::InvalidInput;
  }
  return DeserializationError::Ok;
}

DeserializationError deserializeOneCallV4Hourly(
    WiFiClient &json, owm_resp_onecall_t &response, size_t destinationOffset,
    size_t &recordsRead, int64_t &lastTimestamp,
    std::vector<String> &alertIds)
{
  recordsRead = 0;
  lastTimestamp = 0;

  JsonDocument filter;
  addHourlyFilter(filter);
  JsonDocument doc;
  DeserializationError error = deserializeFiltered(json, doc, filter);
  if (error)
  {
    return error;
  }

  copyRootFields(doc, response);
  for (JsonObject hourly : doc["data"].as<JsonArray>())
  {
    const size_t destination = destinationOffset + recordsRead;
    if (destination >= OWM_NUM_HOURLY)
    {
      break;
    }

    response.hourly[destination] = owm_hourly_t{};
    owm_hourly_t &target = response.hourly[destination];
    target.dt = hourly["dt"].as<int64_t>();
    target.temp = hourly["temp"].as<float>();
    target.feels_like = hourly["feels_like"].as<float>();
    target.pressure = hourly["pressure"].as<int>();
    target.humidity = hourly["humidity"].as<int>();
    target.dew_point = hourly["dew_point"].as<float>();
    target.clouds = hourly["clouds"].as<int>();
    target.uvi = hourly["uvi"].as<float>();
    target.visibility = hourly["visibility"].as<int>();
    target.wind_speed = hourly["wind_speed"].as<float>();
    target.wind_gust = hourly["wind_gust"].as<float>();
    target.wind_deg = hourly["wind_deg"].as<int>();
    target.pop = hourly["pop"].as<float>();
    target.rain_1h = precipitationValue(hourly["rain"]);
    target.snow_1h = precipitationValue(hourly["snow"]);
    collectAlertIds(hourly["alerts"], alertIds);

    if (target.dt <= 0 || target.temp <= 0)
    {
      return DeserializationError::InvalidInput;
    }
    lastTimestamp = target.dt;
    ++recordsRead;
  }

  if (recordsRead == 0)
  {
    return DeserializationError::InvalidInput;
  }
  Serial.printf("[owm4] Hourly page offset=%u records=%u last=%lld\n",
                static_cast<unsigned>(destinationOffset),
                static_cast<unsigned>(recordsRead),
                static_cast<long long>(lastTimestamp));
  return DeserializationError::Ok;
}

DeserializationError deserializeOneCallV4Daily(
    WiFiClient &json, owm_resp_onecall_t &response, size_t &recordsRead,
    std::vector<String> &alertIds)
{
  recordsRead = 0;
  size_t synthesizedWeatherRecords = 0;

  JsonDocument filter;
  addDailyFilter(filter);
  JsonDocument doc;
  DeserializationError error = deserializeFiltered(json, doc, filter);
  if (error)
  {
    return error;
  }

  copyRootFields(doc, response);
  for (JsonObject daily : doc["data"].as<JsonArray>())
  {
    if (recordsRead >= OWM_NUM_DAILY)
    {
      break;
    }

    response.daily[recordsRead] = owm_daily_t{};
    owm_daily_t &target = response.daily[recordsRead];
    target.dt = daily["dt"].as<int64_t>();
    target.sunrise = daily["sunrise"].as<int64_t>();
    target.sunset = daily["sunset"].as<int64_t>();
    target.moonrise = daily["moonrise"].as<int64_t>();
    target.moonset = daily["moonset"].as<int64_t>();
    target.moon_phase = daily["moon_phase"].as<float>();

    JsonObject temp = daily["temp"];
    target.temp.morn = temp["morn"].as<float>();
    target.temp.day = temp["day"].as<float>();
    target.temp.eve = temp["eve"].as<float>();
    target.temp.night = temp["night"].as<float>();
    target.temp.min = temp["min"].as<float>();
    target.temp.max = temp["max"].as<float>();

    JsonObject feelsLike = daily["feels_like"];
    target.feels_like.morn = feelsLike["morn"].as<float>();
    target.feels_like.day = feelsLike["day"].as<float>();
    target.feels_like.eve = feelsLike["eve"].as<float>();
    target.feels_like.night = feelsLike["night"].as<float>();
    target.pressure = daily["pressure"].as<int>();
    target.humidity = daily["humidity"].as<int>();
    target.dew_point = daily["dew_point"].as<float>();
    target.clouds = daily["clouds"].as<int>();
    target.uvi = daily["uvi"].as<float>();
    target.visibility = daily["visibility"].as<int>();
    target.wind_speed = daily["wind_speed"].as<float>();
    target.wind_gust = daily["wind_gust"].as<float>();
    target.wind_deg = daily["wind_deg"].as<int>();
    target.pop = daily["pop"].as<float>();
    target.rain = precipitationValue(daily["rain"]);
    target.snow = precipitationValue(daily["snow"]);
    copyWeather(daily, target.weather);
    if (target.weather.id <= 0)
    {
      synthesizeDailyWeather(target);
      ++synthesizedWeatherRecords;
    }
    collectAlertIds(daily["alerts"], alertIds);

    if (target.dt <= 0 || target.temp.min <= 0 || target.temp.max <= 0
        || target.weather.id <= 0)
    {
      return DeserializationError::InvalidInput;
    }
    ++recordsRead;
  }

  if (recordsRead < 5)
  {
    return DeserializationError::InvalidInput;
  }
  Serial.printf("[owm4] Daily records=%u synthesized_weather=%u "
                "first=%lld max=%.2f min=%.2f\n",
                static_cast<unsigned>(recordsRead),
                static_cast<unsigned>(synthesizedWeatherRecords),
                static_cast<long long>(response.daily[0].dt),
                response.daily[0].temp.max, response.daily[0].temp.min);
  return DeserializationError::Ok;
}

DeserializationError deserializeOneCallV4Alert(
    WiFiClient &json, owm_resp_onecall_t &response)
{
  JsonDocument filter;
  filter["sender_name"] = true;
  filter["event"] = true;
  filter["start"] = true;
  filter["end"] = true;
  filter["description"] = false;

  JsonDocument doc;
  DeserializationError error = deserializeFiltered(json, doc, filter);
  if (error)
  {
    return error;
  }
  if (doc["event"].isNull() || doc["start"].isNull() || doc["end"].isNull())
  {
    return DeserializationError::InvalidInput;
  }

  owm_alerts_t alert = {};
  alert.sender_name = doc["sender_name"] | "";
  alert.event = doc["event"] | "";
  alert.start = doc["start"].as<int64_t>();
  alert.end = doc["end"].as<int64_t>();
  response.alerts.push_back(alert);
  return DeserializationError::Ok;
}

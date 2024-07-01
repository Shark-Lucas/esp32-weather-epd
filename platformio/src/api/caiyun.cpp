/* Client side utilities for esp32-weather-epd.
 * Copyright (C) 2022-2024  Luke Marzen
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// built-in C++ libraries
#include <cstring>
#include <vector>

// arduino/esp32 libraries
#include <Arduino.h>
#include <esp_sntp.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <time.h>
#include <WiFi.h>

// additional libraries
#include <Adafruit_BusIO_Register.h>
#include <ArduinoJson.h>

// header files
#include "_locale.h"
#include "api_response.h"
#include "aqi.h"
#include "client_utils.h"
#include "config.h"
#include "display_utils.h"
#include "renderer.h"
#ifndef USE_HTTP
#include <WiFiClientSecure.h>
#endif

#include <time.h>
#include <sys/time.h>

#ifdef USE_HTTP
static const uint16_t CY_PORT = 80;
#else
static const uint16_t CY_PORT = 443;
#endif

DeserializationError deserializeWeather(WiFiClient &json, owm_resp_onecall_t &r);

/* Perform an HTTP GET request to OpenWeatherMap's "One Call" API
 * If data is received, it will be parsed and stored in the global variable
 * owm_onecall.
 *
 * Returns the HTTP Status Code.
 */
#ifdef USE_HTTP
int getCYWeather(WiFiClient &client, owm_resp_onecall_t &r)
#else
int getCYWeather(WiFiClientSecure &client, owm_resp_onecall_t &r)
#endif
{
    int attempts = 0;
    bool rxSuccess = false;
    DeserializationError jsonErr = {};
    // https://api.caiyunapp.com/v2.6/APIKEY/114.3882,30.4583/weather?alert=true&dailysteps=5&hourlysteps=24
    String uri = "/" + CY_API_VERSION + "/" + CY_APIKEY + "/" + LON + "," + LAT +
                 "/weather?alert=true&dailysteps=5&hourlysteps=24&unit=SI";

    // This string is printed to terminal to help with debugging. The API key is
    // censored to reduce the risk of users exposing their key.
    String sanitizedUri = CY_ENDPOINT + uri;

    Serial.print(TXT_ATTEMPTING_HTTP_REQ);
    Serial.println(": " + uri);
    int httpResponse = 0;
    while (!rxSuccess && attempts < 3)
    {
        HTTPClient http;
        http.begin(client, CY_ENDPOINT, CY_PORT, uri);
        httpResponse = http.GET();
        if (httpResponse == HTTP_CODE_OK)
        {
            jsonErr = deserializeWeather(http.getStream(), r);
            if (jsonErr)
            {
                rxSuccess = false;
                // -100 offset distinguishes these errors from httpClient errors
                httpResponse = -100 - static_cast<int>(jsonErr.code());
            }
            rxSuccess = !jsonErr;
        }
        client.stop();
        http.end();
        Serial.println("  " + String(httpResponse, DEC) + " " + getHttpResponsePhrase(httpResponse));
        ++attempts;
    }

    return httpResponse;
} // getCYonecall


int64_t timeStringToTimestamp(const char *timeStr)
{
    struct tm timeInfo;
    time_t now;

    // 获取当前时间
    time(&now);
    localtime_r(&now, &timeInfo);

    // 解析小时和分钟
    int hour, minute;
    sscanf(timeStr, "%d:%d", &hour, &minute);

    // 设置时间信息
    timeInfo.tm_hour = hour;
    timeInfo.tm_min = minute;
    timeInfo.tm_sec = 0;

    // 转换为time_t类型
    time_t result = mktime(&timeInfo);

    // 转换为int64_t
    return (int64_t)result;
}


// 弧度转角度的函数
int radiansToDegrees(float radians)
{
    float degrees = radians * (180.0 / M_PI);  // 将弧度转换为度
    float meteorological = 270.0 - degrees;    // 将数学角度转换为气象角度

    // 确保角度在0到360之间
    if (meteorological < 0) {
        meteorological += 360.0;
    }
    return meteorological;
}

unsigned long parseIso8601ToTimestamp(const String& dateTime)
{
    struct tm t = {0};
    int year, month, day, hour, minute;
    char sign;
    int tzHour, tzMinute;

    // 解析日期时间和时区
    sscanf(dateTime.c_str(), "%d-%d-%dT%d:%d%c%02d:%02d", 
           &year, &month, &day, &hour, &minute, &sign, &tzHour, &tzMinute);

    // 调整年份
    t.tm_year = year - 1900; // tm_year是从1900年开始的
    t.tm_mon = month - 1;    // tm_mon是从0开始的
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;

    // 将本地时间转换为UTC时间戳
    time_t localTimestamp = mktime(&t);

    // 计算时区偏移（以秒为单位）
    // int offsetSeconds = (tzHour * 3600 + tzMinute * 60) * (sign == '+' ? 1 : -1);
    return localTimestamp;
}

DeserializationError deserializeWeather(WiFiClient &json, owm_resp_onecall_t &r)
{
    int i;

    JsonDocument filter;
    filter["server_time"] = true;
    filter["forecast_keypoint"] = true;

    JsonObject filter_result = filter["result"].to<JsonObject>();
    filter_result["alert"] = true;
    filter_result["realtime"] = true;
    filter_result["hourly"] = true;
    filter_result["daily"] = true;

    JsonDocument doc;

    DeserializationError error = deserializeJson(doc, json, DeserializationOption::Filter(filter));
#if DEBUG_LEVEL >= 1
    Serial.println("[debug] doc.overflowed() : " + String(doc.overflowed()));
#endif
#if DEBUG_LEVEL >= 2
    serializeJsonPretty(doc, Serial);
#endif
    if (error)
    {
        return error;
    }

    JsonObject result = doc["result"];

    JsonObject result_alert = result["alert"];
    
    uint8_t index = 0;

    for (JsonObject result_alert_adcode : result_alert["adcodes"].as<JsonArray>())
    {
        if (index != 0)
        {
            r.city_name += result_alert_adcode["name"].as<String>(); // "湖北省", "武汉市", "江夏区"
            r.city_name += " ";
        }
        index ++;
    }
    index = 0;

    // 实时天气部分

    JsonObject result_realtime = result["realtime"];

    r.current.dt         = doc["server_time"].as<int64_t>();
    r.current.sunrise    = timeStringToTimestamp(result["daily"]["astro"][0]["sunrise"]["time"].as<const char *>());
    r.current.sunset     = timeStringToTimestamp(result["daily"]["astro"][0]["sunset"]["time"].as<const char *>());
    r.current.temp       = result_realtime["temperature"].as<float>();
    r.current.feels_like = result_realtime["apparent_temperature"].as<float>();
    r.current.pressure   = (int)(result_realtime["pressure"].as<float>() / 100);
    r.current.humidity   = (int)(result_realtime["humidity"].as<float>() * 100);
    r.current.clouds     = (int)(result_realtime["cloudrate"].as<float>() * 100);
    r.current.uvi        = result_realtime["life_index"]["ultraviolet"]["index"].as<float>();
    r.current.visibility = (int)result_realtime["visibility"].as<float>();
    r.current.wind_speed = result_realtime["wind"]["speed"].as<float>();
    r.current.wind_deg   = radiansToDegrees(result_realtime["wind"]["speed"].as<float>());
    r.current.aqi        = result_realtime["air_quality"]["aqi"]["chn"].as<int>();
    r.current.weather.skycon = result_realtime["skycon"].as<String>();


    // 未来五天预报部分
    JsonObject result_daily = result["daily"];
    const char* result_daily_status = result_daily["status"];

    for (JsonObject result_daily_temperature_item : result_daily["temperature"].as<JsonArray>())
    {
        r.daily[index].temp.max = result_daily_temperature_item["max"].as<float>();
        r.daily[index].temp.min = result_daily_temperature_item["min"].as<float>();
        index ++;
    }
    index = 0;

    for (JsonObject result_daily_skycon_item : result_daily["skycon"].as<JsonArray>())
    {
        r.daily[index].weather.skycon = result_daily_skycon_item["value"].as<String>();
        index ++;
    }
    index = 0;


    // 24小时预测部分
    JsonObject result_hourly = result["hourly"];

    for (JsonObject result_hourly_temperature_item : result_hourly["temperature"].as<JsonArray>())
    {
        r.hourly[index].dt   = parseIso8601ToTimestamp(result_hourly_temperature_item["datetime"].as<String>());
        r.hourly[index].temp = result_hourly_temperature_item["value"].as<float>();
        Serial.printf("dt = %lu, temp = %.2f\n", r.hourly[i].dt, r.hourly[i].temp);
        index ++;
    }
    index = 0;

    for (JsonObject result_hourly_precipitation_item : result_hourly["precipitation"].as<JsonArray>())
    {
        r.hourly[index].rain_1h = result_hourly_precipitation_item["value"].as<float>() * 3600;
        r.hourly[index].snow_1h = 0;    // 彩云没有降雪数据，全是降水
        Serial.printf("rain_1h = %f\n", r.hourly[i].rain_1h);
        index ++;
    }
    index = 0;



    return error;


} // end deserializeWeather
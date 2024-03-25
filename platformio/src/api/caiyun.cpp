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

#ifdef USE_HTTP
static const uint16_t CY_PORT = 80;
#else
static const uint16_t CY_PORT = 443;
#endif

/* Perform an HTTP GET request to OpenWeatherMap's "One Call" API
 * If data is received, it will be parsed and stored in the global variable
 * owm_onecall.
 *
 * Returns the HTTP Status Code.
 */
#ifdef USE_HTTP
int getCYWeather(WiFiClient &client, owm_resp_onecall_t &r, owm_resp_air_pollution_t &p)
#else
int getCYWeather(WiFiClientSecure &client, owm_resp_onecall_t &r, owm_resp_air_pollution_t &p)
#endif
{
    int attempts = 0;
    bool rxSuccess = false;
    DeserializationError jsonErr = {};
    // https://api.caiyunapp.com/v2.6/APIKEY/114.3882,30.4583/weather?alert=true&dailysteps=5&hourlysteps=24
    String uri = "/" + CY_API_VERSION + "/" + CY_APIKEY + "/" + LON + "," + LAT +
                 "/weather?alert=true&dailysteps=5&hourlysteps=24&unit=metric:v2";

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
            jsonErr = deserializeOneCall(http.getStream(), r);
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
    const char *result_alert_status = result_alert["status"]; // "ok"

    for (JsonObject result_alert_adcode : result_alert["adcodes"].as<JsonArray>())
    {

        long result_alert_adcode_adcode = result_alert_adcode["adcode"];    // 420000, 420100, 420115
        const char *result_alert_adcode_name = result_alert_adcode["name"]; // "湖北省", "武汉市", "江夏区"
    }

    JsonObject result_realtime = result["realtime"];

    r.current.dt = doc["server_time"];
    r.current.sunrise    = result["daily"]["astro"][0]["sunrise"];
    r.current.sunset     = result["daily"]["astro"][0]["sunset"];
    r.current.temp = result_realtime["temperature"];



    //////////////////////////////////////////////

    const char *result_realtime_status = result_realtime["status"];     // "ok"
    float result_realtime_temperature = result_realtime["temperature"]; // 10.66
    float result_realtime_humidity = result_realtime["humidity"];       // 0.93
    int result_realtime_cloudrate = result_realtime["cloudrate"];       // 0
    const char *result_realtime_skycon = result_realtime["skycon"];     // "CLEAR_NIGHT"
    float result_realtime_visibility = result_realtime["visibility"];   // 11.2
    int result_realtime_dswrf = result_realtime["dswrf"];               // 0

    float result_realtime_wind_speed = result_realtime["wind"]["speed"];         // 9.51
    float result_realtime_wind_direction = result_realtime["wind"]["direction"]; // 6.67

    double result_realtime_pressure = result_realtime["pressure"];                        // 102014.48
    float result_realtime_apparent_temperature = result_realtime["apparent_temperature"]; // 9.1

    JsonObject result_realtime_precipitation_local = result_realtime["precipitation"]["local"];
    const char *result_realtime_precipitation_local_status = result_realtime_precipitation_local["status"];
    const char *result_realtime_precipitation_local_datasource = result_realtime_precipitation_local["datasource"];
    int result_realtime_precipitation_local_intensity = result_realtime_precipitation_local["intensity"];

    JsonObject result_realtime_precipitation_nearest = result_realtime["precipitation"]["nearest"];
    const char *result_realtime_precipitation_nearest_status = result_realtime_precipitation_nearest["status"];
    int result_realtime_precipitation_nearest_distance = result_realtime_precipitation_nearest["distance"];
    int result_realtime_precipitation_nearest_intensity = result_realtime_precipitation_nearest["intensity"];

    JsonObject result_realtime_air_quality = result_realtime["air_quality"];
    int result_realtime_air_quality_pm25 = result_realtime_air_quality["pm25"]; // 18
    int result_realtime_air_quality_pm10 = result_realtime_air_quality["pm10"]; // 23
    int result_realtime_air_quality_o3 = result_realtime_air_quality["o3"];     // 88
    int result_realtime_air_quality_so2 = result_realtime_air_quality["so2"];   // 6
    int result_realtime_air_quality_no2 = result_realtime_air_quality["no2"];   // 21
    float result_realtime_air_quality_co = result_realtime_air_quality["co"];   // 0.5

    int result_realtime_air_quality_aqi_chn = result_realtime_air_quality["aqi"]["chn"]; // 28
    int result_realtime_air_quality_aqi_usa = result_realtime_air_quality["aqi"]["usa"]; // 63

    const char *result_realtime_air_quality_description_chn = result_realtime_air_quality["description"]["chn"];
    const char *result_realtime_air_quality_description_usa = result_realtime_air_quality["description"]["usa"];

    for (JsonPair result_realtime_life_index_item : result_realtime["life_index"].as<JsonObject>())
    {
        const char *result_realtime_life_index_item_key = result_realtime_life_index_item.key().c_str();

        int result_realtime_life_index_item_value_index = result_realtime_life_index_item.value()["index"];
        const char *result_realtime_life_index_item_value_desc = result_realtime_life_index_item.value()["desc"];
    }

} // end deserializeWeather
/* Configuration options for esp32-weather-epd.
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

#include <Arduino.h>
#include "config.h"

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #include "secrets.example.h"
  #warning "secrets.h not found; using empty credentials from secrets.example.h"
#endif

// PINS
// The configuration below is intended for use with the project's official 
// wiring diagrams using the FireBeetle 2 ESP32-E microcontroller board.
//
// Note: LED_BUILTIN pin will be disabled to reduce power draw.  Refer to your
//       board's pinout to ensure you avoid using a pin with this shared 
//       functionality.
//
// ADC pin used to measure battery voltage
const uint8_t PIN_BAT_ADC  = A2; // A0 for micro-usb firebeetle
// Pins for E-Paper Driver Board
const uint8_t PIN_EPD_BUSY = 14; // 5 for micro-usb firebeetle
const uint8_t PIN_EPD_CS   = 13;
const uint8_t PIN_EPD_RST  = 21;
const uint8_t PIN_EPD_DC   = 22;
const uint8_t PIN_EPD_SCK  = 18;
const uint8_t PIN_EPD_MISO = 19; // 19 Master-In Slave-Out not used, as no data from display
const uint8_t PIN_EPD_MOSI = 23;
const uint8_t PIN_EPD_PWR  = 26; // Irrelevant if directly connected to 3.3V
// I2C Pins used for BME280
const uint8_t PIN_BME_SDA = 17;
const uint8_t PIN_BME_SCL = 16;
const uint8_t PIN_BME_PWR =  4;   // Irrelevant if directly connected to 3.3V
const uint8_t BME_ADDRESS = 0x76; // If sensor does not work, try 0x77

// WIFI
const unsigned long WIFI_TIMEOUT = 10000; // ms, WiFi connection timeout.
const unsigned long WIFI_PORTAL_TIMEOUT = 180; // seconds, hard portal limit.
const unsigned long WIFI_SAVE_CONNECT_TIMEOUT = 15; // seconds.
const unsigned long WIFI_DOUBLE_RESET_WINDOW = 4000; // ms.

// OPENWEATHERMAP API
// OpenWeatherMap API key, https://openweathermap.org/
const String OWM_APIKEY   = SECRET_OWM_API_KEY;
// OpenWeather's official regional endpoint for users in mainland China.
// API paths, parameters, response formats, and API keys are the same as the
// global standard endpoint.
const String OWM_ENDPOINT = "cn-api.openweathermap.org";
// The China endpoint currently aliases this host. Resolving the final target
// directly avoids a CNAME-chain limitation seen in ESP32's lwIP resolver.
const String OWM_DNS_TARGET = "us-west-api.openweathermap.org";
// Last-resort address used only if both DNS servers time out. HTTP requests
// still carry OWM_ENDPOINT as their Host header.
const IPAddress OWM_FALLBACK_IP(38, 143, 66, 114);
// Some One Call 4.0 timeline endpoints are not currently responsive through
// the China frontend. The 4.0 adapter uses the global endpoint only for those
// endpoints while keeping current/hourly/air-quality traffic on the regional
// endpoint.
const String OWM_GLOBAL_ENDPOINT = "api.openweathermap.org";
const String OWM_GLOBAL_DNS_TARGET = "api.openweathermap.org";
const IPAddress OWM_GLOBAL_FALLBACK_IP(15, 235, 222, 68);
// OpenWeatherMap One Call 2.5 API is deprecated for all new free users
// (accounts created after Summer 2022).
//
// Please note, that One Call API 3.0 is included in the "One Call by Call"
// subscription only. This separate subscription includes 1,000 calls/day for
// free and allows you to pay only for the number of API calls made to this
// product.
//
// Here’s how to subscribe and avoid any credit card changes:
// - Go to https://home.openweathermap.org/subscriptions/billing_info/onecall_30/base?key=base&service=onecall_30
// - Follow the instructions to complete the subscription.
// - Go to https://home.openweathermap.org/subscriptions and set the "Calls per
//   day (no more than)" to 1,000. This ensures you will never overrun the free
//   calls.
#if OWM_ONECALL_API_VERSION == 3
const String OWM_ONECALL_VERSION = "3.0";
#elif OWM_ONECALL_API_VERSION == 4
const String OWM_ONECALL_VERSION = "4.0";
#endif


// 彩云天气 API
// 彩云天气 API key, https://platform.caiyunapp.com
const String CY_APIKEY       = SECRET_CY_API_KEY;
const String CY_ENDPOINT     = "api.caiyunapp.com";
const String CY_API_VERSION  = "v2.6";

// LOCATION
// Set your latitude and longitude.
// (used to get weather data as part of API requests to OpenWeatherMap)
const String LAT = "31.1663";
const String LON = "121.3616";
// City name that will be shown in the top-right corner of the display.
const String CITY_STRING = "上海";

// TIME
// For list of time zones see
// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
const char *TIMEZONE = "CST-8";
// Time format used when displaying sunrise/set times. (Max 11 characters)
// For more information about formatting see
// https://man7.org/linux/man-pages/man3/strftime.3.html
// const char *TIME_FORMAT = "%l:%M%P"; // 12-hour ex: 1:23am  11:00pm
const char *TIME_FORMAT = "%H:%M";   // 24-hour ex: 01:23   23:00
// Time format used when displaying axis labels. (Max 11 characters)
// For more information about formatting see
// https://man7.org/linux/man-pages/man3/strftime.3.html
// const char *HOUR_FORMAT = "%l%P"; // 12-hour ex: 1am  11pm
const char *HOUR_FORMAT = "%H";      // 24-hour ex: 01   23
// Date format used when displaying date in top-right corner.
// For more information about formatting see
// https://man7.org/linux/man-pages/man3/strftime.3.html
const char *DATE_FORMAT = "%B%e 日 %A"; // ex: Sat, January 1
// Date/Time format used when displaying the last refresh time along the bottom
// of the screen.
// For more information about formatting see
// https://man7.org/linux/man-pages/man3/strftime.3.html
const char *REFRESH_TIME_FORMAT = "%y/%m/%d %H:%M";
// NTP_SERVER_1 is the primary time server, while NTP_SERVER_2 is a fallback.
// pool.ntp.org will find the closest available NTP server to you.
const char *NTP_SERVER_1 = "ntp1.aliyun.com";
const char *NTP_SERVER_2 = "ntp.tencent.com";
// If you encounter the 'Failed To Fetch The Time' error, try increasing
// NTP_TIMEOUT or select closer/lower latency time servers.
const unsigned long NTP_TIMEOUT = 20000; // ms
// ESP32 system time is retained by the RTC timer during deep sleep. Only
// refresh it periodically to limit drift from the internal RTC oscillator.
const unsigned long NTP_RESYNC_INTERVAL = 12UL * 60UL * 60UL; // seconds
// Sleep duration in minutes. (aka how often esp32 will wake for an update)
// Aligned to the nearest minute boundary and must evenly divide 60.
// For example, if set to 30 (minutes) the display will update at 00 or 30
// minutes past the hour. (range: [2-60])
const long SLEEP_DURATION = 30;
// If BED_TIME == WAKE_TIME, then this battery saving feature will be disabled.
// (range: [0-23])
const int BED_TIME  = 00; // Last update at 00:00 (midnight) until WAKE_TIME.
const int WAKE_TIME = 06; // Hour of first update after BED_TIME, 06:00.

// HOURLY OUTLOOK GRAPH
// Number of hours to display on the outlook graph. (range: [8-48])
const int HOURLY_GRAPH_MAX = 24;

// BATTERY
// To protect the battery upon LOW_BATTERY_VOLTAGE, the display will cease to
// update until battery is charged again. The ESP32 will deep-sleep (consuming
// < 11μA), waking briefly check the voltage at the corresponding interval (in
// minutes). Once the battery voltage has fallen to CRIT_LOW_BATTERY_VOLTAGE,
// the esp32 will hibernate and a manual press of the reset (RST) button to
// begin operating again.
const uint32_t MAX_BATTERY_VOLTAGE      = 4200; // (millivolts)
const uint32_t WARN_BATTERY_VOLTAGE     = 3400; // (millivolts)
const uint32_t LOW_BATTERY_VOLTAGE      = 3200; // (millivolts)
const uint32_t VERY_LOW_BATTERY_VOLTAGE = 3100; // (millivolts)
const uint32_t CRIT_LOW_BATTERY_VOLTAGE = 3000; // (millivolts)
const unsigned long LOW_BATTERY_SLEEP_INTERVAL      = 30;  // (minutes)
const unsigned long VERY_LOW_BATTERY_SLEEP_INTERVAL = 120; // (minutes)

// See config.h for the below options
// E-PAPER PANEL
// LOCALE
// UNITS
// AIR QUALITY INDEX
// WIND ICON PRECISION
// FONTS
// ALERTS
// BATTERY MONITORING


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
#include <algorithm>
#include <cstring>
#include <functional>
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
#include "api/openweather_v4.h"
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
  static const uint16_t OWM_PORT = 80;
#else
  static const uint16_t OWM_PORT = 443;
#endif

namespace
{
constexpr uint32_t NETWORK_PROBE_TIMEOUT = 5000; // ms
constexpr int32_t OWM_CONNECT_TIMEOUT = 8000;    // ms
constexpr uint16_t OWM_RESPONSE_TIMEOUT = 10000; // ms
constexpr uint16_t OWM_V4_DAILY_RESPONSE_TIMEOUT = 25000; // ms
constexpr uint8_t OWM_MAX_ATTEMPTS = 2;
constexpr uint8_t OWM_V4_DAILY_MAX_ATTEMPTS = 3;
constexpr size_t OWM_V4_PAGE_SIZE = 20;

enum class OwmEndpointKind
{
  REGIONAL,
  GLOBAL
};

IPAddress owmRegionalResolvedIP;
IPAddress owmGlobalResolvedIP;
bool owmRegionalAddressReady = false;
bool owmGlobalAddressReady = false;

const String &getOwmEndpoint(OwmEndpointKind kind)
{
  return kind == OwmEndpointKind::REGIONAL
       ? OWM_ENDPOINT : OWM_GLOBAL_ENDPOINT;
}

const String &getOwmDnsTarget(OwmEndpointKind kind)
{
  return kind == OwmEndpointKind::REGIONAL
       ? OWM_DNS_TARGET : OWM_GLOBAL_DNS_TARGET;
}

IPAddress getOwmFallbackIP(OwmEndpointKind kind)
{
  return kind == OwmEndpointKind::REGIONAL
       ? OWM_FALLBACK_IP : OWM_GLOBAL_FALLBACK_IP;
}

IPAddress &getOwmResolvedIP(OwmEndpointKind kind)
{
  return kind == OwmEndpointKind::REGIONAL
       ? owmRegionalResolvedIP : owmGlobalResolvedIP;
}

bool &getOwmAddressReady(OwmEndpointKind kind)
{
  return kind == OwmEndpointKind::REGIONAL
       ? owmRegionalAddressReady : owmGlobalAddressReady;
}

void resolveOwmAddress(OwmEndpointKind kind)
{
  bool &addressReady = getOwmAddressReady(kind);
  if (addressReady)
  {
    return;
  }

  IPAddress &resolvedIP = getOwmResolvedIP(kind);
  const String &dnsTarget = getOwmDnsTarget(kind);
  const String &endpoint = getOwmEndpoint(kind);
  const unsigned long dnsStarted = millis();
  if (WiFi.hostByName(dnsTarget.c_str(), resolvedIP))
  {
    Serial.printf("[net] DNS %s (for %s) -> %s (%lu ms)\n",
                  dnsTarget.c_str(), endpoint.c_str(),
                  resolvedIP.toString().c_str(), millis() - dnsStarted);
  }
  else
  {
    resolvedIP = getOwmFallbackIP(kind);
    Serial.printf("[net] DNS %s failed (%lu ms); fallback IP: %s\n",
                  dnsTarget.c_str(), millis() - dnsStarted,
                  resolvedIP.toString().c_str());
  }
  addressReady = true;
}

#ifdef USE_HTTP
bool connectOwmHttpTransport(WiFiClient &client, OwmEndpointKind kind,
                             uint16_t responseTimeout)
{
  resolveOwmAddress(kind);
  const IPAddress &resolvedIP = getOwmResolvedIP(kind);
  client.stop();
  const unsigned long connectStarted = millis();
  const bool connected = client.connect(resolvedIP, OWM_PORT,
                                        OWM_CONNECT_TIMEOUT);
  Serial.printf("[net] HTTP transport %s:%u: %s (%lu ms)\n",
                resolvedIP.toString().c_str(), OWM_PORT,
                connected ? "CONNECTED" : "FAILED",
                millis() - connectStarted);
  if (connected)
  {
    client.setTimeout((responseTimeout + 500) / 1000);
  }
  return connected;
}
#endif

void printNetworkDiagnostics(const String &host)
{
  const String localIP = WiFi.localIP().toString();
  const String subnetMask = WiFi.subnetMask().toString();
  const String gatewayIP = WiFi.gatewayIP().toString();
  const String dns1 = WiFi.dnsIP(0).toString();
  const String dns2 = WiFi.dnsIP(1).toString();

  Serial.println("[net] Network diagnostics");
  Serial.printf("[net] API endpoint: %s\n", host.c_str());
  Serial.printf("[net] SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("[net] RSSI: %d dBm, channel: %d\n",
                WiFi.RSSI(), WiFi.channel());
  Serial.printf("[net] IP: %s, subnet: %s, gateway: %s\n",
                localIP.c_str(), subnetMask.c_str(), gatewayIP.c_str());
  Serial.printf("[net] DNS1: %s, DNS2: %s\n",
                dns1.c_str(), dns2.c_str());

  resolveOwmAddress(OwmEndpointKind::REGIONAL);
  const IPAddress &resolvedIP = getOwmResolvedIP(OwmEndpointKind::REGIONAL);
  const String resolvedIPString = resolvedIP.toString();

  constexpr uint16_t probePorts[] = {80, 443};
  for (const uint16_t port : probePorts)
  {
    WiFiClient probe;
    const unsigned long probeStarted = millis();
    const bool connected = probe.connect(resolvedIP, port,
                                         NETWORK_PROBE_TIMEOUT);
    const unsigned long probeElapsed = millis() - probeStarted;
    Serial.printf("[net] TCP %s:%u: %s (%lu ms)\n",
                  resolvedIPString.c_str(), port,
                  connected ? "CONNECTED" : "FAILED", probeElapsed);
    probe.stop();
  }
}

#ifndef USE_HTTP
void printLastTlsError(WiFiClientSecure &client, int httpResponse,
                       uint8_t attempt, unsigned long elapsed)
{
  Serial.printf("[net] HTTPS attempt %u: HTTPClient=%d (%lu ms)\n",
                attempt, httpResponse, elapsed);
  if (httpResponse >= 0)
  {
    return;
  }

  char errorBuffer[160] = {};
  const int tlsResult = client.lastError(errorBuffer, sizeof(errorBuffer));
  if (tlsResult >= 0)
  {
    // In Arduino-ESP32 2.0.x, lastError() contains the positive socket file
    // descriptor after a successful handshake. Do not translate it as an
    // mbedTLS error code.
    Serial.printf("[net] TLS handshake completed, socket: %d\n", tlsResult);
    return;
  }

  Serial.printf("[net] TLS connection error: %d (%s)\n",
                tlsResult, errorBuffer[0] == '\0' ? "no detail" : errorBuffer);
}
#endif

#ifdef USE_HTTP
using OwmNetworkClient = WiFiClient;
#else
using OwmNetworkClient = WiFiClientSecure;
#endif

using OwmResponseParser =
    std::function<DeserializationError(WiFiClient &stream)>;

int performOwmRequest(OwmNetworkClient &client, OwmEndpointKind endpointKind,
                      const String &uri, const String &sanitizedUri,
                      const OwmResponseParser &parser,
                      uint16_t responseTimeout = OWM_RESPONSE_TIMEOUT,
                      uint8_t maxAttempts = OWM_MAX_ATTEMPTS)
{
  const String &endpoint = getOwmEndpoint(endpointKind);
  Serial.print(TXT_ATTEMPTING_HTTP_REQ);
  Serial.println(": " + sanitizedUri);

  int httpResponse = 0;
  for (uint8_t attempt = 1; attempt <= maxAttempts; ++attempt)
  {
    HTTPClient http;
#ifdef USE_HTTP
    http.begin(client, endpoint, OWM_PORT, uri);
    if (!connectOwmHttpTransport(client, endpointKind, responseTimeout))
    {
      httpResponse = HTTPC_ERROR_CONNECTION_REFUSED;
      http.end();
      Serial.printf("[net] HTTP attempt %u: transport unavailable\n", attempt);
      Serial.println("  " + String(httpResponse, DEC) + " "
                     + getHttpResponsePhrase(httpResponse));
      continue;
    }
#else
    http.begin(client, endpoint, OWM_PORT, uri, true);
#endif
    http.setConnectTimeout(OWM_CONNECT_TIMEOUT);
    http.setTimeout(responseTimeout);

    const unsigned long requestStarted = millis();
    httpResponse = http.GET();
    const unsigned long requestElapsed = millis() - requestStarted;
#ifndef USE_HTTP
    printLastTlsError(client, httpResponse, attempt, requestElapsed);
#else
    Serial.printf("[net] HTTP attempt %u: HTTPClient=%d (%lu ms)\n",
                  attempt, httpResponse, requestElapsed);
#endif

    if (httpResponse == HTTP_CODE_OK)
    {
      const int expectedBodySize = http.getSize();
      Serial.printf("[net] HTTP response body: %d bytes\n", expectedBodySize);
      const DeserializationError jsonError = parser(http.getStream());
      if (!jsonError)
      {
        client.stop();
        http.end();
        Serial.println("  200 OK");
        return HTTP_CODE_OK;
      }

      // -100 offset distinguishes JSON/schema errors from HTTPClient errors.
      httpResponse = -100 - static_cast<int>(jsonError.code());
      Serial.printf("[owm] JSON/schema error: %s; expected=%d, "
                    "remaining=%d, connected=%d\n",
                    jsonError.c_str(), expectedBodySize,
                    client.available(), client.connected());
    }

    client.stop();
    http.end();
    Serial.println("  " + String(httpResponse, DEC) + " "
                   + getHttpResponsePhrase(httpResponse));
  }
  return httpResponse;
}
} // namespace

/* Power-on and connect WiFi.
 * Takes int parameter to store WiFi RSSI, or “Received Signal Strength
 * Indicator"
 *
 * Returns WiFi status.
 */
wl_status_t startWiFi(int &wifiRSSI)
{
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  const String savedSSID = WiFi.SSID();
  Serial.printf("%s '%s'", TXT_CONNECTING_TO, savedSSID.c_str());

  const unsigned long connectionStart = millis();
  wl_status_t connection_status = WiFi.status();

  while ((connection_status != WL_CONNECTED)
         && (millis() - connectionStart < WIFI_TIMEOUT))
  {
    Serial.print(".");
    delay(50);
    connection_status = WiFi.status();
  }
  Serial.println();

  if (connection_status == WL_CONNECTED)
  {
    wifiRSSI = WiFi.RSSI(); // get WiFi signal strength now, because the WiFi
                            // will be turned off to save power!
    Serial.println("IP: " + WiFi.localIP().toString());
  }
  else
  {
    Serial.printf("%s '%s'\n", TXT_COULD_NOT_CONNECT_TO, savedSSID.c_str());
  }
  return connection_status;
} // startWiFi

/* Disconnect and power-off WiFi.
 */
void killWiFi()
{
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
} // killWiFi

/* Prints the local time to serial monitor.
 *
 * Returns true if getting local time was a success, otherwise false.
 */
bool printLocalTime(tm *timeInfo)
{
  int attempts = 0;
  while (!getLocalTime(timeInfo) && attempts++ < 3)
  {
    Serial.println(TXT_FAILED_TO_GET_TIME);
    return false;
  }
  Serial.println(timeInfo, "%A, %B %d, %Y %H:%M:%S");
  return true;
} // printLocalTime

/* Waits for NTP server time sync, adjusted for the time zone specified in
 * config.cpp.
 *
 * Returns true if time was set successfully, otherwise false.
 *
 * Note: Must be connected to WiFi to get time from NTP server.
 */
bool waitForSNTPSync(tm *timeInfo)
{
  // Wait for SNTP synchronization to complete
  unsigned long timeout = millis() + NTP_TIMEOUT;
  // In ESP-IDF, reading SNTP_SYNC_STATUS_COMPLETED consumes that state and
  // resets it. Cache the value so a successful sync is not mistaken for a
  // failure by a second status read.
  sntp_sync_status_t syncStatus = sntp_get_sync_status();
  if ((syncStatus == SNTP_SYNC_STATUS_RESET) && (millis() < timeout))
  {
    Serial.print(TXT_WAITING_FOR_SNTP);
    delay(100); // ms
    while ((syncStatus == SNTP_SYNC_STATUS_RESET) && (millis() < timeout))
    {
      Serial.print(".");
      delay(100); // ms
      syncStatus = sntp_get_sync_status();
    }
    Serial.println();
  }
  if (syncStatus == SNTP_SYNC_STATUS_RESET)
  {
    return false;
  }
  return printLocalTime(timeInfo);
} // waitForSNTPSync

String makeOneCallV4TimelineUri(const String &step, size_t count,
                                int64_t startTimestamp = 0)
{
  String uri = "/data/4.0/onecall/timeline/" + step
             + "?lat=" + LAT + "&lon=" + LON
             + "&lang=" + OWM_LANG + "&units=standard"
             + "&cnt=" + String(count);
  if (startTimestamp > 0)
  {
    uri += "&start=" + String(static_cast<long long>(startTimestamp));
  }
  return uri;
}

String sanitizedOwmUri(OwmEndpointKind kind, const String &uri)
{
  return getOwmEndpoint(kind) + uri + "&appid={API key}";
}

int getOWMonecallV3(OwmNetworkClient &client, owm_resp_onecall_t &response)
{
  String uri = "/data/3.0/onecall?lat=" + LAT + "&lon=" + LON
             + "&lang=" + OWM_LANG + "&units=standard&exclude=minutely";
#if !DISPLAY_ALERTS
  uri += ",alerts";
#endif
  const String sanitizedUri = sanitizedOwmUri(OwmEndpointKind::REGIONAL, uri);
  uri += "&appid=" + OWM_APIKEY;

  return performOwmRequest(
      client, OwmEndpointKind::REGIONAL, uri, sanitizedUri,
      [&response](WiFiClient &stream) {
        response.alerts.clear();
        return deserializeOneCallV3(stream, response);
      });
}

int getOWMonecallV4(OwmNetworkClient &client, owm_resp_onecall_t &response)
{
  std::vector<String> alertIds;
  response.alerts.clear();

  // Current conditions: one record in data[].
  String uri = "/data/4.0/onecall/current?lat=" + LAT + "&lon=" + LON
             + "&lang=" + OWM_LANG + "&units=standard";
  String sanitizedUri = sanitizedOwmUri(OwmEndpointKind::REGIONAL, uri);
  uri += "&appid=" + OWM_APIKEY;
  int status = performOwmRequest(
      client, OwmEndpointKind::REGIONAL, uri, sanitizedUri,
      [&response, &alertIds](WiFiClient &stream) {
        return deserializeOneCallV4Current(stream, response, alertIds);
      });
  if (status != HTTP_CODE_OK)
  {
    return status;
  }

  // Hourly timeline: the API returns at most 20 records per page. Fetch only
  // the records consumed by the renderer. The response array keeps its 48-hour
  // capacity, so changing HOURLY_GRAPH_MAX automatically adjusts request count
  // without wasting API calls on undisplayed records.
  const size_t requiredHourlyRecords = std::min(
      static_cast<size_t>(OWM_NUM_HOURLY),
      static_cast<size_t>(HOURLY_GRAPH_MAX));
  size_t hourlyRecords = 0;
  int64_t nextStart = 0;
  while (hourlyRecords < requiredHourlyRecords)
  {
    const size_t requested = std::min(
        OWM_V4_PAGE_SIZE, requiredHourlyRecords - hourlyRecords);
    uri = makeOneCallV4TimelineUri("1h", requested, nextStart);
    sanitizedUri = sanitizedOwmUri(OwmEndpointKind::REGIONAL, uri);
    uri += "&appid=" + OWM_APIKEY;

    size_t pageRecords = 0;
    int64_t lastTimestamp = 0;
    status = performOwmRequest(
        client, OwmEndpointKind::REGIONAL, uri, sanitizedUri,
        [&response, &alertIds, hourlyRecords, &pageRecords,
         &lastTimestamp](WiFiClient &stream) {
          return deserializeOneCallV4Hourly(
              stream, response, hourlyRecords, pageRecords, lastTimestamp,
              alertIds);
        });
    if (status != HTTP_CODE_OK)
    {
      return status;
    }

    hourlyRecords += pageRecords;
    if (pageRecords < requested || lastTimestamp <= 0)
    {
      break;
    }
    nextStart = lastTimestamp + 3600;
  }
  if (hourlyRecords < requiredHourlyRecords)
  {
    Serial.printf("[owm4] Insufficient hourly records: %u/%d\n",
                  static_cast<unsigned>(hourlyRecords), HOURLY_GRAPH_MAX);
    return -100 - static_cast<int>(DeserializationError::InvalidInput);
  }

  // The China frontend currently stalls on the 1-day endpoint, so this one
  // official 4.0 request uses the global frontend. Its schema is identical.
  uri = makeOneCallV4TimelineUri("1day", OWM_NUM_DAILY);
  sanitizedUri = sanitizedOwmUri(OwmEndpointKind::GLOBAL, uri);
  uri += "&appid=" + OWM_APIKEY;
  size_t dailyRecords = 0;
  status = performOwmRequest(
      client, OwmEndpointKind::GLOBAL, uri, sanitizedUri,
      [&response, &alertIds, &dailyRecords](WiFiClient &stream) {
        return deserializeOneCallV4Daily(
            stream, response, dailyRecords, alertIds);
      }, OWM_V4_DAILY_RESPONSE_TIMEOUT, OWM_V4_DAILY_MAX_ATTEMPTS);
  if (status != HTTP_CODE_OK)
  {
    return status;
  }

#if DISPLAY_ALERTS
  // 4.0 timeline records contain alert IDs, not full alert objects. Fetch
  // details independently. A failed optional detail must not discard valid
  // weather/forecast data.
  for (const String &alertId : alertIds)
  {
    uri = "/data/4.0/onecall/alert/" + alertId;
    sanitizedUri = getOwmEndpoint(OwmEndpointKind::GLOBAL) + uri
                 + "?appid={API key}";
    uri += "?appid=" + OWM_APIKEY;
    const int alertStatus = performOwmRequest(
        client, OwmEndpointKind::GLOBAL, uri, sanitizedUri,
        [&response](WiFiClient &stream) {
          return deserializeOneCallV4Alert(stream, response);
        });
    if (alertStatus != HTTP_CODE_OK)
    {
      Serial.printf("[owm4] Alert detail skipped after error %d\n",
                    alertStatus);
    }
  }
#endif

  Serial.printf("[owm4] Assembled current=1, hourly=%u, daily=%u, alerts=%u\n",
                static_cast<unsigned>(hourlyRecords),
                static_cast<unsigned>(dailyRecords),
                static_cast<unsigned>(response.alerts.size()));
  return HTTP_CODE_OK;
}

/* Dispatch to an independent One Call implementation. Switching versions
 * requires changing only OWM_ONECALL_API_VERSION in config.h.
 */
#ifdef USE_HTTP
  int getOWMonecall(WiFiClient &client, owm_resp_onecall_t &response)
#else
  int getOWMonecall(WiFiClientSecure &client, owm_resp_onecall_t &response)
#endif
{
  printNetworkDiagnostics(OWM_ENDPOINT);
#if OWM_ONECALL_API_VERSION == 3
  return getOWMonecallV3(client, response);
#elif OWM_ONECALL_API_VERSION == 4
  return getOWMonecallV4(client, response);
#endif
}

/* Perform an HTTP GET request to OpenWeatherMap's "Air Pollution" API
 * If data is received, it will be parsed and stored in the global variable
 * owm_air_pollution.
 *
 * Returns the HTTP Status Code.
 */
#ifdef USE_HTTP
  int getOWMairpollution(WiFiClient &client, owm_resp_air_pollution_t &r)
#else
  int getOWMairpollution(WiFiClientSecure &client, owm_resp_air_pollution_t &r)
#endif
{
  // set start and end to appropriate values so that the last 24 hours of air
  // pollution history is returned. Unix, UTC.
  time_t now;
  int64_t end = time(&now);
  // minus 1 is important here, otherwise we could get an extra hour of history
  int64_t start = end - ((3600 * OWM_NUM_AIR_POLLUTION) - 1);
  char endStr[22];
  char startStr[22];
  sprintf(endStr, "%lld", end);
  sprintf(startStr, "%lld", start);
  String uri = "/data/2.5/air_pollution/history?lat=" + LAT + "&lon=" + LON
               + "&start=" + startStr + "&end=" + endStr
               + "&appid=" + OWM_APIKEY;
  // This string is printed to terminal to help with debugging. The API key is
  // censored to reduce the risk of users exposing their key.
  String sanitizedUri = OWM_ENDPOINT +
               "/data/2.5/air_pollution/history?lat=" + LAT + "&lon=" + LON
               + "&start=" + startStr + "&end=" + endStr
               + "&appid={API key}";

  return performOwmRequest(
      client, OwmEndpointKind::REGIONAL, uri, sanitizedUri,
      [&r](WiFiClient &stream) {
        return deserializeAirQuality(stream, r);
      });
} // getOWMairpollution

/* Prints debug information about heap usage.
 */
void printHeapUsage() {
  Serial.println("[debug] Heap Size       : "
                 + String(ESP.getHeapSize()) + " B");
  Serial.println("[debug] Available Heap  : "
                 + String(ESP.getFreeHeap()) + " B");
  Serial.println("[debug] Min Free Heap   : "
                 + String(ESP.getMinFreeHeap()) + " B");
  Serial.println("[debug] Max Allocatable : "
                 + String(ESP.getMaxAllocHeap()) + " B");
  return;
}


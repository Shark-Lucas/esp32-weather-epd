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
#include <esp_netif.h>
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
#include "icons/icons_128x128.h"
#include "renderer.h"
#include "relay_warmup.h"
#ifndef USE_HTTP
  #include <WiFiClientSecure.h>
  #include <mbedtls/ssl.h>
  #include <mbedtls/x509.h>
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
constexpr uint8_t OWM_MAX_ATTEMPTS = 5; // first attempt plus four retries
constexpr uint32_t OWM_RETRY_DELAY = 3000; // ms, only between attempts
constexpr size_t OWM_V4_PAGE_SIZE = 20;

OwmRequestError lastOwmRequestError;

#define OWM_UI_LOCALE_STRING_(value) #value
#define OWM_UI_LOCALE_STRING(value) OWM_UI_LOCALE_STRING_(value)
const char *errorText(const char *english, const char *chinese)
{
  // UI locale and upstream weather language are intentionally independent.
  return std::strcmp(OWM_UI_LOCALE_STRING(LOCALE), "zh_CN") == 0
       ? chinese : english;
}
#undef OWM_UI_LOCALE_STRING
#undef OWM_UI_LOCALE_STRING_

const char *requestName(const String &uri)
{
  if (uri.startsWith("/data/2.5/air_pollution"))
    return errorText("Air quality", "空气质量");
  if (uri.startsWith("/data/4.0/onecall/timeline/1day"))
    return errorText("Daily forecast", "每日预报");
  if (uri.startsWith("/data/4.0/onecall/timeline/1h"))
    return errorText("Hourly forecast", "小时预报");
  if (uri.startsWith("/data/4.0/onecall/alert/"))
    return errorText("Weather alert", "天气告警");
  if (uri.startsWith("/data/3.0/onecall"))
    return errorText("Weather forecast", "天气预报");
  return errorText("Current weather", "实时天气");
}

void recordRequestError(const String &reason, const String &raw,
                        unsigned long elapsed, const String &uri,
                        uint8_t attempt, uint8_t maxAttempts)
{
  lastOwmRequestError.reason = reason;
  lastOwmRequestError.detail = raw + " / " + String(elapsed / 1000.0f, 1) + "s";
#ifdef OWM_USE_RELAY
  lastOwmRequestError.context = String("CF / ");
#else
  lastOwmRequestError.context = String("OW / ");
#endif
  lastOwmRequestError.context += String(requestName(uri)) + " / "
                            + String(attempt) + "/" + String(maxAttempts);
}

String requestFailureReason(int status, const String &relayError)
{
#ifdef OWM_USE_RELAY
  if (relayError == "snapshot_warming")
    return errorText("Weather cache preparing", "天气缓存准备中");
  if (relayError == "snapshot_warmup_timeout")
    return errorText("Weather cache preparation timed out", "天气缓存准备超时");
  if (relayError == "snapshot_expired")
    return errorText("Weather cache expired", "天气缓存已过期");
  if (relayError == "snapshot_range_unavailable")
    return errorText("Weather cache range unavailable", "天气缓存范围不足");
  if (relayError == "upstream_budget_exhausted" || relayError == "upstream_budget_insufficient")
    return errorText("OW daily budget insufficient", "OW 每日额度不足");
  if (relayError == "upstream_budget_not_configured" || relayError == "upstream_budget_unit_changed")
    return errorText("OW budget needs configuration", "OW 额度配置未完成");
  if (relayError == "location_capacity_reached")
    return errorText("Weather location limit reached", "天气位置数量已满");
  if (relayError == "kv_unavailable" || relayError == "snapshot_invalid"
      || relayError == "snapshot_publish_failed" || relayError == "snapshot_too_large")
    return errorText("CF weather cache unavailable", "CF 天气缓存不可用");
  if (relayError == "upstream_timeout")
    return errorText("CF to OW timed out", "CF 请求 OW 超时");
  if (relayError == "upstream_fetch_failed")
    return errorText("CF to OW connection failed", "CF 连接 OW 失败");
  if (relayError == "upstream_invalid_payload")
    return errorText("OW returned invalid data", "OW 响应数据异常");
  if (relayError == "unauthorized")
    return errorText("Invalid device token", "CF 设备口令无效");
  if (relayError == "worker_not_configured" || relayError == "invalid_worker_configuration")
    return errorText("CF configuration error", "CF 代理配置错误");
  if (relayError == "upstream_http_401")
    return errorText("Invalid OW API key", "OW 密钥无效");
  if (relayError == "upstream_http_403")
    return errorText("OW access denied", "OW 接口权限不足");
  if (relayError == "upstream_http_429")
    return errorText("OW rate/quota limit", "OW 请求额度受限");
#else
  (void)relayError;
#endif
  switch (status)
  {
  case HTTPC_ERROR_CONNECTION_REFUSED:
#ifdef OWM_USE_RELAY
    return errorText("CF TCP/TLS connection failed", "CF 连接失败(TCP/TLS)");
#else
    return errorText("OW TCP/TLS connection failed", "OW 连接失败(TCP/TLS)");
#endif
  case HTTPC_ERROR_READ_TIMEOUT:
#ifdef OWM_USE_RELAY
    return errorText("Waiting for CF timed out", "等待 CF 响应超时");
#else
    return errorText("Waiting for OW timed out", "等待 OW 响应超时");
#endif
  case HTTPC_ERROR_CONNECTION_LOST:
    return errorText("Response connection lost", "读取响应时连接断开");
  case HTTPC_ERROR_TOO_LESS_RAM:
    return errorText("Not enough memory", "设备内存不足");
  case -100 - DeserializationError::EmptyInput:
  case -100 - DeserializationError::IncompleteInput:
    return errorText("Weather data incomplete", "天气数据接收不完整");
  case -100 - DeserializationError::InvalidInput:
    return errorText("Invalid weather data", "天气数据格式异常");
  case -100 - DeserializationError::NoMemory:
    return errorText("Not enough memory for JSON", "解析天气时内存不足");
  case 401:
    return errorText("Request authentication failed", "请求鉴权失败");
  case 403:
    return errorText("API access denied", "接口拒绝访问");
  case 429:
    return errorText("Request rate/quota limit", "请求额度受限");
  case 502:
    return errorText("Proxy or upstream failure", "代理或上游响应异常");
  case 504:
    return errorText("Proxy or upstream timeout", "代理或上游响应超时");
  default:
    // Old deployments have no diagnostic header. Never infer an OW timeout
    // from a generic CF 502/504 or copy an untrusted response body to screen.
    return getHttpResponsePhrase(status);
  }
}

enum class OwmEndpointKind
{
  REGIONAL,
  GLOBAL
};

IPAddress owmRegionalResolvedIP;
IPAddress owmGlobalResolvedIP;
bool owmRegionalAddressReady = false;
bool owmGlobalAddressReady = false;
uint8_t wifiReconnectAttempts = 0;

bool reconnectWiFi(uint32_t totalTimeout = UINT32_MAX)
{
  if (WiFi.status() == WL_CONNECTED) return true;
  const uint32_t started = millis();
  while (wifiReconnectAttempts < WIFI_MAX_RECONNECT_ATTEMPTS
         && millis() - started < totalTimeout)
  {
    if (WiFi.status() == WL_CONNECTED) break;
    if (wifiReconnectAttempts > 0)
    {
      delay(std::min<uint32_t>(OWM_RETRY_DELAY, totalTimeout - (millis() - started)));
      if (WiFi.status() == WL_CONNECTED) break;
      if (millis() - started >= totalTimeout) return false;
    }
    ++wifiReconnectAttempts;
    const unsigned long attemptStarted = millis();
    const bool requested = WiFi.reconnect();
    if (!requested) WiFi.begin(); // Reload saved NVS config if reconnect could not start.
    Serial.printf("[net] WiFi reconnect %u/%u: requested=%d\n",
                  wifiReconnectAttempts, WIFI_MAX_RECONNECT_ATTEMPTS, requested);
    while (WiFi.status() != WL_CONNECTED
           && millis() - attemptStarted < WIFI_TIMEOUT
           && millis() - started < totalTimeout)
    {
      delay(std::min<uint32_t>(50, totalTimeout - (millis() - started)));
    }
    Serial.printf("[net] WiFi reconnect %u/%u: status=%d (%lu ms)\n",
                  wifiReconnectAttempts, WIFI_MAX_RECONNECT_ATTEMPTS,
                  static_cast<int>(WiFi.status()), millis() - attemptStarted);
    if (WiFi.status() == WL_CONNECTED) break;
  }
  if (WiFi.status() != WL_CONNECTED) return false;
  owmRegionalAddressReady = false;
  owmGlobalAddressReady = false;
  configureWiFiDns(); // DHCP can replace preferred DNS during reconnection.
  Serial.printf("[net] WiFi recovered after %u/%u reconnects, RSSI=%d dBm\n",
                wifiReconnectAttempts, WIFI_MAX_RECONNECT_ATTEMPTS, WiFi.RSSI());
  return true;
}

const String &getOwmEndpoint(OwmEndpointKind kind)
{
#ifdef OWM_USE_RELAY
  (void)kind;
  return OWM_RELAY_HOST;
#else
  return kind == OwmEndpointKind::REGIONAL
       ? OWM_ENDPOINT : OWM_GLOBAL_ENDPOINT;
#endif
}

const String &getOwmDnsTarget(OwmEndpointKind kind)
{
#ifdef OWM_USE_RELAY
  (void)kind;
  return OWM_RELAY_HOST;
#else
  return kind == OwmEndpointKind::REGIONAL
       ? OWM_DNS_TARGET : OWM_GLOBAL_DNS_TARGET;
#endif
}

#ifndef OWM_USE_RELAY
IPAddress getOwmFallbackIP(OwmEndpointKind kind)
{
  return kind == OwmEndpointKind::REGIONAL
       ? OWM_FALLBACK_IP : OWM_GLOBAL_FALLBACK_IP;
}
#endif

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
#ifdef OWM_USE_RELAY
    // Never send a relay credential to an old, fixed OpenWeather address.
    Serial.printf("[net] Relay DNS %s failed (%lu ms)\n",
                  dnsTarget.c_str(), millis() - dnsStarted);
    return;
#else
    resolvedIP = getOwmFallbackIP(kind);
    Serial.printf("[net] DNS %s failed (%lu ms); fallback IP: %s\n",
                  dnsTarget.c_str(), millis() - dnsStarted,
                  resolvedIP.toString().c_str());
#endif
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
  if (!getOwmAddressReady(OwmEndpointKind::REGIONAL))
  {
    return;
  }
  const IPAddress &resolvedIP = getOwmResolvedIP(OwmEndpointKind::REGIONAL);
  const String resolvedIPString = resolvedIP.toString();

#ifdef OWM_USE_RELAY
  constexpr uint16_t probePorts[] = {443};
#else
  constexpr uint16_t probePorts[] = {80, 443};
#endif
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
    // A DNS failure does not update lastError(). A nonnegative value can be
    // zero or an old socket descriptor, so it does not prove this attempt's
    // handshake succeeded.
    Serial.printf("[net] No new TLS error reported (raw=%d); "
                  "check the DNS/TCP logs above\n", tlsResult);
    return;
  }
  if (tlsResult == -1)
  {
    // The core uses -1 for several socket/setup/timeout failures. Its INFO
    // and ERROR logs carry the actual errno or timed-out phase.
    Serial.println("[net] Socket/TLS setup failed; "
                   "check the core errno/timeout logs above");
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
#ifndef USE_HTTP
  client.setHandshakeTimeout(25);
#endif

  int httpResponse = 0;
  RelayWarmup warmup;
  bool warmingPoll = false;
  for (uint8_t attempt = 1; attempt <= maxAttempts; ++attempt)
  {
    if (warmup.active() && warmup.remaining(millis()) == 0)
    {
      recordRequestError(requestFailureReason(503, "snapshot_warmup_timeout"),
                         "HTTP 503", RelayWarmup::LIMIT_MS, uri,
                         attempt, maxAttempts);
      return HTTP_CODE_SERVICE_UNAVAILABLE;
    }
    if (attempt > 1 && !warmingPoll)
    {
      Serial.printf("[net] Waiting %lu ms before retry %u/%u\n",
                    static_cast<unsigned long>(OWM_RETRY_DELAY),
                    attempt, maxAttempts);
      delay(OWM_RETRY_DELAY);
    }
    warmingPoll = false;
    lastOwmRequestError = {};
    const unsigned long attemptStarted = millis();
    if (WiFi.status() != WL_CONNECTED)
    {
      client.stop();
      if (!reconnectWiFi(warmup.active() ? warmup.remaining(millis()) : UINT32_MAX))
      {
        httpResponse = HTTPC_ERROR_NOT_CONNECTED;
        const String reason = warmup.active() && warmup.remaining(millis()) == 0
                            ? requestFailureReason(503, "snapshot_warmup_timeout")
                            : errorText("WiFi reconnect failed", "WiFi 重连失败");
        recordRequestError(reason, "WiFi " + String(wifiReconnectAttempts)
                           + "/" + String(WIFI_MAX_RECONNECT_ATTEMPTS) + " / -4",
                           millis() - attemptStarted, uri, attempt, maxAttempts);
        return httpResponse; // The shared WiFi budget is spent; do not just poll five more times.
      }
    }
    Serial.printf("[net] Request attempt %u/%u: WiFi=%d, RSSI=%d dBm, "
                  "heap=%u, largest=%u\n",
                  attempt, maxAttempts, static_cast<int>(WiFi.status()),
                  WiFi.RSSI(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());

#ifndef USE_HTTP
    // Log DNS independently: WiFiClientSecure otherwise reports DNS, TCP,
    // and TLS failures through the same HTTPClient -1 result. Its internal
    // hostname lookup then uses the DNS cache, keeping SNI and certificate
    // hostname verification in the normal secure-client connection path.
    IPAddress address;
    const unsigned long dnsStarted = millis();
    if (!WiFi.hostByName(endpoint.c_str(), address))
    {
      Serial.printf("[net] DNS stage FAILED: %s (%lu ms), WiFi=%d\n",
                    endpoint.c_str(), millis() - dnsStarted,
                    static_cast<int>(WiFi.status()));
      client.stop();
      httpResponse = HTTPC_ERROR_CONNECTION_REFUSED;
      recordRequestError(errorText("DNS lookup failed", "DNS 解析失败"),
                         "HTTP -1 / DNS", millis() - attemptStarted,
                         uri, attempt, maxAttempts);
      continue;
    }
    Serial.printf("[net] DNS check %s -> %s (%lu ms)\n",
                  endpoint.c_str(), address.toString().c_str(),
                  millis() - dnsStarted);
#endif

    HTTPClient http;
#ifdef USE_HTTP
    http.begin(client, endpoint, OWM_PORT, uri);
    if (!connectOwmHttpTransport(client, endpointKind, responseTimeout))
    {
      httpResponse = HTTPC_ERROR_CONNECTION_REFUSED;
      recordRequestError(errorText("TCP connection failed", "TCP 连接失败"),
                         "HTTP -1", millis() - attemptStarted,
                         uri, attempt, maxAttempts);
      http.end();
      Serial.printf("[net] HTTP attempt %u: transport unavailable\n", attempt);
      Serial.println("  " + String(httpResponse, DEC) + " "
                     + getHttpResponsePhrase(httpResponse));
      continue;
    }
#else
    http.begin(client, endpoint, OWM_PORT, uri, true);
#endif
    const uint32_t remaining = warmup.remaining(millis());
    if (remaining == 0)
    {
      recordRequestError(requestFailureReason(503, "snapshot_warmup_timeout"),
                         "HTTP 503", RelayWarmup::LIMIT_MS, uri,
                         attempt, maxAttempts);
      client.stop(); http.end();
      return HTTP_CODE_SERVICE_UNAVAILABLE;
    }
    const uint16_t readTimeout = std::min<uint32_t>(responseTimeout, remaining);
    http.setConnectTimeout(std::min<uint32_t>(OWM_CONNECT_TIMEOUT, remaining));
    http.setTimeout(readTimeout);
#ifndef USE_HTTP
    if (warmup.active()) client.setHandshakeTimeout(std::max<uint32_t>(1, std::min<uint32_t>(25, remaining / 1000)));
#endif
    // In Arduino-ESP32 2.0.x the client's setTimeout(seconds) changes socket
    // timeouts, but leaves Stream's separate millisecond timeout at 1000.
    // ArduinoJson reads through Stream::readBytes(), so configure that base
    // explicitly or a brief gap between packets becomes IncompleteInput.
    Stream &responseStream = client;
    const unsigned long previousStreamTimeout = responseStream.getTimeout();
    responseStream.setTimeout(readTimeout);
    Serial.printf("[net] Stream read timeout: %lu -> %u ms\n",
                  previousStreamTimeout, readTimeout);
#ifdef OWM_USE_RELAY
    // ArduinoJson reads the raw stream. HTTP/1.0 avoids chunk framing, and
    // identity encoding avoids compressed JSON that this parser cannot read.
    http.useHTTP10(true);
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Authorization", "Bearer " + OWM_RELAY_TOKEN);
    const char *relayHeaders[] = {
      "X-Weather-Cache", "X-Weather-Fetched-At", "Age", "X-Weather-Error",
      "Server-Timing", "Retry-After"
    };
    http.collectHeaders(relayHeaders, 6);
#endif

    const unsigned long requestStarted = millis();
    httpResponse = http.GET();
    const unsigned long requestElapsed = millis() - requestStarted;
#ifndef USE_HTTP
    printLastTlsError(client, httpResponse, attempt, requestElapsed);
#else
    Serial.printf("[net] HTTP attempt %u: HTTPClient=%d (%lu ms)\n",
                  attempt, httpResponse, requestElapsed);
#endif

#ifdef OWM_USE_RELAY
    if (httpResponse == HTTP_CODE_SERVICE_UNAVAILABLE
        && http.header("X-Weather-Error") == "snapshot_warming")
    {
      const bool firstWait = !warmup.active();
      warmup.begin(millis());
      const int retryAfter = http.header("Retry-After").toInt();
      recordRequestError(requestFailureReason(httpResponse, "snapshot_warming"),
                         "HTTP 503", RelayWarmup::LIMIT_MS - warmup.remaining(millis()),
                         uri, attempt, maxAttempts);
      client.stop(); http.end();
      if (firstWait)
      {
        // One persistent preparing page; do not refresh E-Paper every poll.
        initDisplay();
        do
        {
          drawActionScreen(wi_cloud_down_128x128,
                           requestFailureReason(503, "snapshot_warming"),
                           requestName(uri),
                           errorText("Waiting up to 120s", "最多等待120秒"));
        } while (display.nextPage());
        powerOffDisplay();
      }
      const uint32_t waitMs = warmup.pollDelay(millis(), retryAfter);
      Serial.printf("[relay] Snapshot preparing; polling in %lu ms\n",
                    static_cast<unsigned long>(waitMs));
      delay(waitMs);
      warmingPoll = true;
      --attempt; // A warming response does not consume a network retry.
      continue;
    }
#endif

    String reason = requestFailureReason(httpResponse,
                                         http.header("X-Weather-Error"));
    String rawError = "HTTP " + String(httpResponse);
#ifndef USE_HTTP
    if (httpResponse == HTTPC_ERROR_CONNECTION_REFUSED)
    {
      char tlsMessage[1] = {};
      const int tlsError = client.lastError(tlsMessage, sizeof(tlsMessage));
      if (tlsError < 0)
      {
        rawError += " / TLS " + String(tlsError);
        if (tlsError == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
          reason = errorText("TLS certificate rejected", "TLS 证书校验失败");
        else if (tlsError == MBEDTLS_ERR_SSL_TIMEOUT)
          reason = errorText("TLS handshake timed out", "TLS 握手超时");
        else if (tlsError < -1)
          reason = errorText("TLS handshake failed", "TLS 握手失败");
      }
    }
#endif

    if (httpResponse == HTTP_CODE_OK)
    {
#ifdef OWM_USE_RELAY
      Serial.printf("[relay] cache=%s fetched=%s age=%ss\n",
                    http.header("X-Weather-Cache").c_str(),
                    http.header("X-Weather-Fetched-At").c_str(),
                    http.header("Age").c_str());
      Serial.printf("[relay] timing=%s\n", http.header("Server-Timing").c_str());
#endif
      const int expectedBodySize = http.getSize();
      Serial.printf("[net] HTTP response body: %d bytes\n", expectedBodySize);
      const unsigned long parsingStarted = millis();
      const DeserializationError jsonError = parser(http.getStream());
      if (!jsonError)
      {
        client.stop();
        http.end();
        lastOwmRequestError = {};
#ifndef USE_HTTP
        client.setHandshakeTimeout(25);
#endif
        Serial.println("  200 OK");
        return HTTP_CODE_OK;
      }

      // -100 offset distinguishes JSON/schema errors from HTTPClient errors.
      httpResponse = -100 - static_cast<int>(jsonError.code());
      reason = requestFailureReason(httpResponse, "");
      rawError = "JSON " + String(httpResponse);
      Serial.printf("[owm] JSON/schema error: %s; expected=%d, "
                    "remaining=%d, connected=%d, elapsed=%lu ms, "
                    "stream-timeout=%lu ms\n",
                    jsonError.c_str(), expectedBodySize,
                    client.available(), client.connected(),
                    millis() - parsingStarted, responseStream.getTimeout());
    }

    recordRequestError(reason, rawError, millis() - attemptStarted,
                       uri, attempt, maxAttempts);
    client.stop();
    http.end();
    Serial.println("  " + String(httpResponse, DEC) + " "
                   + getHttpResponsePhrase(httpResponse));
  }
  return httpResponse;
}
} // namespace

const OwmRequestError &getLastOwmRequestError()
{
  return lastOwmRequestError;
}

uint8_t getWiFiReconnectAttempts()
{
  return wifiReconnectAttempts;
}

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

  if (connection_status != WL_CONNECTED)
  {
    reconnectWiFi();
    connection_status = WiFi.status();
  }

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

void configureWiFiDns()
{
  esp_netif_t *station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (station == nullptr)
  {
    Serial.println("[net] DNS configuration unavailable; keeping DHCP DNS.");
    return;
  }

  // Keep the original DHCP resolver as a third choice. Apply this after both
  // saved-network and provisioning connections, before SNTP or HTTPS queries.
  const IPAddress dhcpDns = WiFi.dnsIP(0);
  const IPAddress servers[] = {WIFI_DNS_PRIMARY, WIFI_DNS_SECONDARY, dhcpDns};
  const esp_netif_dns_type_t types[] = {
    ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP, ESP_NETIF_DNS_FALLBACK
  };
  for (size_t index = 0; index < 3; ++index)
  {
    esp_netif_dns_info_t info = {};
    info.ip.type = ESP_IPADDR_TYPE_V4;
    info.ip.u_addr.ip4.addr = static_cast<uint32_t>(servers[index]);
    const esp_err_t result = esp_netif_set_dns_info(station, types[index], &info);
    if (result != ESP_OK)
    {
      Serial.printf("[net] DNS slot %u configuration failed: %s\n",
                    static_cast<unsigned>(index), esp_err_to_name(result));
    }
  }
  Serial.printf("[net] DNS primary=%s backup=%s fallback=%s (DHCP)\n",
                WiFi.dnsIP(0).toString().c_str(),
                WiFi.dnsIP(1).toString().c_str(),
                WiFi.dnsIP(2).toString().c_str());
}

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
#ifdef OWM_USE_RELAY
  return getOwmEndpoint(kind) + uri;
#else
  return getOwmEndpoint(kind) + uri
       + (uri.indexOf('?') < 0 ? "?appid=" : "&appid=") + "{API key}";
#endif
}

void appendOwmApiKey(String &uri)
{
#ifdef OWM_USE_RELAY
  (void)uri;
#else
  uri += (uri.indexOf('?') < 0 ? "?appid=" : "&appid=") + OWM_APIKEY;
#endif
}

int getOWMonecallV3(OwmNetworkClient &client, owm_resp_onecall_t &response)
{
  String uri = "/data/3.0/onecall?lat=" + LAT + "&lon=" + LON
             + "&lang=" + OWM_LANG + "&units=standard&exclude=minutely";
#if !DISPLAY_ALERTS
  uri += ",alerts";
#endif
  const String sanitizedUri = sanitizedOwmUri(OwmEndpointKind::REGIONAL, uri);
  appendOwmApiKey(uri);

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
  appendOwmApiKey(uri);
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
    appendOwmApiKey(uri);

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
    lastOwmRequestError = {
      errorText("Hourly forecast incomplete", "小时预报数据不足"),
      String(hourlyRecords) + "/" + String(requiredHourlyRecords),
      errorText("Hourly forecast validation", "小时预报校验")
    };
    return -100 - static_cast<int>(DeserializationError::InvalidInput);
  }

  // The China frontend currently stalls on the 1-day endpoint, so this one
  // official 4.0 request uses the global frontend. Its schema is identical.
  uri = makeOneCallV4TimelineUri("1day", OWM_NUM_DAILY);
  sanitizedUri = sanitizedOwmUri(OwmEndpointKind::GLOBAL, uri);
  appendOwmApiKey(uri);
  size_t dailyRecords = 0;
  status = performOwmRequest(
      client, OwmEndpointKind::GLOBAL, uri, sanitizedUri,
      [&response, &alertIds, &dailyRecords](WiFiClient &stream) {
        return deserializeOneCallV4Daily(
            stream, response, dailyRecords, alertIds);
      }, OWM_V4_DAILY_RESPONSE_TIMEOUT);
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
    sanitizedUri = sanitizedOwmUri(OwmEndpointKind::GLOBAL, uri);
    appendOwmApiKey(uri);
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
#ifdef OWM_USE_RELAY
  if (OWM_RELAY_TOKEN.length() < 32 || OWM_RELAY_TOKEN.length() > 256)
  {
    Serial.println("[relay] Set SECRET_OWM_RELAY_TOKEN (32-256 characters) in secrets.h");
    lastOwmRequestError = {
      errorText("Device token not configured", "设备口令未正确配置"),
      "HTTP 503", errorText("Device configuration", "设备配置")
    };
    return HTTP_CODE_SERVICE_UNAVAILABLE;
  }
#endif
  printNetworkDiagnostics(getOwmEndpoint(OwmEndpointKind::REGIONAL));
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
               + "&start=" + startStr + "&end=" + endStr;
  // This string is printed to terminal to help with debugging. The API key is
  // censored to reduce the risk of users exposing their key.
  String sanitizedUri = sanitizedOwmUri(OwmEndpointKind::REGIONAL, uri);
  appendOwmApiKey(uri);

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


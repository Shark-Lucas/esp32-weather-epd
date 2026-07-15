/* Main program for esp32-weather-epd.
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
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <esp_sleep.h>
#include <Preferences.h>
#include <time.h>
#include <WiFi.h>
#include <Wire.h>

#include "_locale.h"
#include "api_response.h"
#include "client_utils.h"
#include "config.h"
#include "display_utils.h"
#include "icons/icons_128x128.h"
#include "renderer.h"
#include "wifi_provisioning.h"
#if defined(USE_HTTPS_NO_CERT_VERIF) || defined(USE_HTTPS_WITH_CERT_VERIF)
  #include <WiFiClientSecure.h>
#endif
#ifdef USE_HTTPS_WITH_CERT_VERIF
  #include "cert.h"
#endif

#ifdef WEATHER_PROVIDER_CAIYUN
  #include "api/caiyun.h"
#endif

// too large to allocate locally on stack
static owm_resp_onecall_t       owm_onecall;
static owm_resp_air_pollution_t owm_air_pollution;

Preferences prefs;

namespace
{
constexpr uint32_t POWER_SAVE_CPU_FREQUENCY_MHZ = 80;
constexpr uint32_t API_CPU_FREQUENCY_MHZ = 240;
constexpr uint32_t RTC_TIME_STATE_MAGIC = 0x52544354; // "RTCT"
constexpr time_t MIN_VALID_EPOCH = 1704067200;        // 2024-01-01 UTC

RTC_DATA_ATTR uint32_t rtcTimeStateMagic = 0;
RTC_DATA_ATTR time_t rtcLastNtpSyncEpoch = 0;

bool readRtcTime(tm *timeInfo, time_t *epoch = nullptr)
{
  const time_t now = time(nullptr);
  if (now < MIN_VALID_EPOCH || localtime_r(&now, timeInfo) == nullptr)
  {
    return false;
  }
  if (epoch != nullptr)
  {
    *epoch = now;
  }
  return true;
}

bool shouldSynchronizeTime(esp_sleep_wakeup_cause_t wakeupCause,
                           time_t rtcEpoch)
{
  if (wakeupCause != ESP_SLEEP_WAKEUP_TIMER
      || rtcTimeStateMagic != RTC_TIME_STATE_MAGIC
      || rtcLastNtpSyncEpoch < MIN_VALID_EPOCH
      || rtcEpoch < rtcLastNtpSyncEpoch)
  {
    return true;
  }

  return static_cast<unsigned long>(rtcEpoch - rtcLastNtpSyncEpoch)
         >= NTP_RESYNC_INTERVAL;
}

void setApiCpuFrequency(uint32_t frequencyMhz)
{
  if (getCpuFrequencyMhz() == frequencyMhz)
  {
    return;
  }

  if (setCpuFrequencyMhz(frequencyMhz))
  {
    Serial.printf("[net] CPU frequency: %u MHz\n", getCpuFrequencyMhz());
  }
  else
  {
    Serial.printf("[net] Failed to set CPU frequency to %u MHz\n",
                  frequencyMhz);
  }
}
} // namespace

void showActionScreen(const uint8_t *bitmap_128x128,
                      const String &title,
                      const String &line1 = "", const String &line2 = "",
                      const String &line3 = "", const String &line4 = "",
                      const String &line5 = "")
{
  initDisplay();
  do
  {
    drawActionScreen(bitmap_128x128, title, line1, line2, line3, line4, line5);
  } while (display.nextPage());
  powerOffDisplay();
}

void showWiFiSetupScreen(const String &apName, const String &apPassword)
{
  initDisplay();
  do
  {
    drawWiFiSetupScreen(apName, apPassword);
  } while (display.nextPage());
  powerOffDisplay();
}

void showWiFiFailureScreen(WiFiFailureReason reason,
                           bool automaticRetryAvailable = true)
{
  const uint8_t *icon = wifi_off_128x128;
  String title = TXT_WIFI_CONNECTION_FAILED;
  String detail = TXT_WIFI_CONNECTION_TIMEOUT;
  String action1 = TXT_WIFI_RESET_TO_SETUP;
  String action2 = TXT_RETRY_NEXT_UPDATE;

  switch (reason)
  {
  case WiFiFailureReason::NO_CREDENTIALS:
    detail = TXT_WIFI_NO_SAVED_CREDENTIALS;
    break;
  case WiFiFailureReason::PORTAL_START_FAILED:
    detail = TXT_WIFI_SETUP_START_FAILED;
    break;
  case WiFiFailureReason::PORTAL_NOT_COMPLETED:
    detail = TXT_WIFI_SETUP_NOT_COMPLETED;
    break;
  case WiFiFailureReason::NETWORK_NOT_FOUND:
    icon = wifi_x_128x128;
    title = TXT_NETWORK_NOT_AVAILABLE;
    detail = TXT_WIFI_NETWORK_NOT_FOUND;
    action1 = TXT_CHECK_NETWORK_AND_RESET;
    break;
  case WiFiFailureReason::CREDENTIALS_REJECTED:
    icon = wifi_x_128x128;
    detail = TXT_WIFI_CREDENTIALS_REJECTED;
    break;
  case WiFiFailureReason::CONNECTION_TIMEOUT:
    action1 = TXT_CHECK_NETWORK_AND_RESET;
    break;
  case WiFiFailureReason::NONE:
  default:
    return;
  }

  if (!automaticRetryAvailable)
  {
    action2 = "";
  }
  showActionScreen(icon, title, detail, action1, action2);
}

/* Put esp32 into ultra low-power deep sleep (<11μA).
 * Aligns wake time to the minute. Sleep times defined in config.cpp.
 */
void beginDeepSleep(unsigned long &startTime, tm *timeInfo)
{
  if (!getLocalTime(timeInfo))
  {
    Serial.println(TXT_REFERENCING_OLDER_TIME_NOTICE);
  }

  uint64_t sleepDuration = 0;
  int extraHoursUntilWake = 0;
  int curHour = timeInfo->tm_hour;

  if (timeInfo->tm_min >= 58)
  { // if we are within 2 minutes of the next hour, then round up for the
    // purposes of bed time
    curHour = (curHour + 1) % 24;
    extraHoursUntilWake += 1;
  }

  if (BED_TIME < WAKE_TIME && curHour >= BED_TIME && curHour < WAKE_TIME)
  { // 0              B   v  W  24
    // |--------------zzzzZzz---|
    extraHoursUntilWake += WAKE_TIME - curHour;
  }
  else if (BED_TIME > WAKE_TIME && curHour < WAKE_TIME)
  { // 0 v W               B    24
    // |zZz----------------zzzzz|
    extraHoursUntilWake += WAKE_TIME - curHour;
  }
  else if (BED_TIME > WAKE_TIME && curHour >= BED_TIME)
  { // 0   W               B  v 24
    // |zzz----------------zzzZz|
    extraHoursUntilWake += WAKE_TIME - (curHour - 24);
  }
  else // This feature is disabled (BED_TIME == WAKE_TIME)
  {    // OR it is not past BED_TIME
    extraHoursUntilWake = 0;
  }

  if (extraHoursUntilWake == 0)
  { // align wake time to nearest multiple of SLEEP_DURATION
    sleepDuration = SLEEP_DURATION * 60ULL
                    - ((timeInfo->tm_min % SLEEP_DURATION) * 60ULL
                        + timeInfo->tm_sec);
  }
  else
  { // align wake time to the hour
    sleepDuration = extraHoursUntilWake * 3600ULL
                    - (timeInfo->tm_min * 60ULL + timeInfo->tm_sec);
  }

  // if we are within 2 minutes of the next alignment.
  if (sleepDuration <= 120ULL)
  {
    sleepDuration += SLEEP_DURATION * 60ULL;
  }

  // add extra delay to compensate for esp32's with fast RTCs.
  sleepDuration += 10ULL;

#if DEBUG_LEVEL >= 1
  printHeapUsage();
#endif

  esp_sleep_enable_timer_wakeup(sleepDuration * 1000000ULL);
  Serial.print(TXT_AWAKE_FOR);
  Serial.println(" "  + String((millis() - startTime) / 1000.0, 3) + "s");
  Serial.print(TXT_ENTERING_DEEP_SLEEP_FOR);
  Serial.println(" " + String(sleepDuration) + "s");
  esp_deep_sleep_start();
} // end beginDeepSleep

/* Program entry point.
 */
void setup()
{
  unsigned long startTime = millis();
  Serial.begin(115200);

#if DEBUG_LEVEL >= 1
  printHeapUsage();
#endif

  disableBuiltinLED();

  // Open namespace for read/write to non-volatile storage
  prefs.begin(NVS_NAMESPACE, false);

#if BATTERY_MONITORING
  uint32_t batteryVoltage = readBatteryVoltage();
  Serial.print(TXT_BATTERY_VOLTAGE);
  Serial.println(": " + String(batteryVoltage) + "mv");

  // When the battery is low, the display should be updated to reflect that, but
  // only the first time we detect low voltage. The next time the display will
  // refresh is when voltage is no longer low. To keep track of that we will
  // make use of non-volatile storage.
  bool lowBat = prefs.getBool("lowBat", false);

  // low battery, deep sleep now
  if (batteryVoltage <= LOW_BATTERY_VOLTAGE)
  {
    if (lowBat == false)
    { // battery is now low for the first time
      prefs.putBool("lowBat", true);
      prefs.end();
      const String recoveryAction = batteryVoltage <= CRIT_LOW_BATTERY_VOLTAGE
                                  ? TXT_CHARGE_THEN_RESET
                                  : TXT_RECOVER_AFTER_CHARGING;
      showActionScreen(battery_alert_0deg_128x128,
                       TXT_LOW_BATTERY,
                       String(batteryVoltage) + " mV",
                       TXT_CONNECT_CHARGER,
                       recoveryAction);
    }

    if (batteryVoltage <= CRIT_LOW_BATTERY_VOLTAGE)
    { // critically low battery
      // don't set esp_sleep_enable_timer_wakeup();
      // We won't wake up again until someone manually presses the RST button.
      Serial.println(TXT_CRIT_LOW_BATTERY_VOLTAGE);
      Serial.println(TXT_HIBERNATING_INDEFINITELY_NOTICE);
    }
    else if (batteryVoltage <= VERY_LOW_BATTERY_VOLTAGE)
    { // very low battery
      esp_sleep_enable_timer_wakeup(VERY_LOW_BATTERY_SLEEP_INTERVAL
                                    * 60ULL * 1000000ULL);
      Serial.println(TXT_VERY_LOW_BATTERY_VOLTAGE);
      Serial.print(TXT_ENTERING_DEEP_SLEEP_FOR);
      Serial.println(" " + String(VERY_LOW_BATTERY_SLEEP_INTERVAL) + "min");
    }
    else
    { // low battery
      esp_sleep_enable_timer_wakeup(LOW_BATTERY_SLEEP_INTERVAL
                                    * 60ULL * 1000000ULL);
      Serial.println(TXT_LOW_BATTERY_VOLTAGE);
      Serial.print(TXT_ENTERING_DEEP_SLEEP_FOR);
      Serial.println(" " + String(LOW_BATTERY_SLEEP_INTERVAL) + "min");
    }
    esp_deep_sleep_start();
  }
  // battery is no longer low, reset variable in non-volatile storage
  if (lowBat == true)
  {
    prefs.putBool("lowBat", false);
  }
#else
  uint32_t batteryVoltage = UINT32_MAX;
#endif

  // All data should have been loaded from NVS. Close filesystem.
  prefs.end();

  String statusStr = {};
  String tmpStr = {};
  tm timeInfo = {};

  // START WIFI
  int wifiRSSI = 0; // “Received Signal Strength Indicator"
  bool wifiConnected = false;
  const bool credentialsAvailable = hasStoredWiFiCredentials();
  const WiFiProvisioningTrigger provisioningTrigger =
      getWiFiProvisioningTrigger(credentialsAvailable);

  if (provisioningTrigger != WiFiProvisioningTrigger::NONE)
  {
    const String apName = getWiFiProvisioningAPName();
    const String apPassword = getWiFiProvisioningAPPassword();
    showWiFiSetupScreen(apName, apPassword);

    const WiFiProvisioningOutcome outcome =
        startWiFiProvisioning(apName, apPassword);
    if (outcome.connected)
    {
      wifiConnected = true;
      wifiRSSI = WiFi.RSSI();
      clearWiFiFailure();
    }
    else
    {
      WiFiFailureReason reason = WiFiFailureReason::PORTAL_NOT_COMPLETED;
      if (!outcome.portalStarted)
      {
        reason = WiFiFailureReason::PORTAL_START_FAILED;
      }
      else if (outcome.lastStatus == WL_NO_SSID_AVAIL)
      {
        reason = WiFiFailureReason::NETWORK_NOT_FOUND;
      }
      else if (outcome.lastStatus == WL_CONNECT_FAILED)
      {
        reason = WiFiFailureReason::CREDENTIALS_REJECTED;
      }

      recordWiFiFailure(reason);
      killWiFi();
      showWiFiFailureScreen(reason, credentialsAvailable);
      beginDeepSleep(startTime, &timeInfo);
      return;
    }
  }

  if (!wifiConnected)
  {
    if (!credentialsAvailable)
    {
      const WiFiFailureReason reason = WiFiFailureReason::NO_CREDENTIALS;
      if (recordWiFiFailure(reason))
      {
        showWiFiFailureScreen(reason, false);
      }
      killWiFi();
      beginDeepSleep(startTime, &timeInfo);
      return;
    }

    const wl_status_t wifiStatus = startWiFi(wifiRSSI);
    if (wifiStatus != WL_CONNECTED)
    {
      const WiFiFailureReason reason = classifyWiFiFailure(wifiStatus);
      if (recordWiFiFailure(reason))
      {
        showWiFiFailureScreen(reason);
      }
      killWiFi();
      beginDeepSleep(startTime, &timeInfo);
      return;
    }

    wifiConnected = true;
    clearWiFiFailure();
  }

  // TIME SYNCHRONIZATION
  // Arduino-ESP32 keeps system time using the RTC timer during deep sleep.
  // Apply the timezone on every boot, but contact NTP only when the retained
  // time is missing, this was not a timer wake, or the resync interval elapsed.
  setenv("TZ", TIMEZONE, 1);
  tzset();

  time_t rtcEpoch = 0;
  const bool rtcTimeValid = readRtcTime(&timeInfo, &rtcEpoch);
  const esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  const bool ntpSyncRequired = !rtcTimeValid
                            || shouldSynchronizeTime(wakeupCause, rtcEpoch);

  bool timeConfigured = rtcTimeValid;
  if (!ntpSyncRequired)
  {
    const unsigned long age = static_cast<unsigned long>(
        rtcEpoch - rtcLastNtpSyncEpoch);
    Serial.printf("[time] RTC valid; skipping SNTP (last sync %lu s ago).\n",
                  age);
    Serial.println(&timeInfo, "%A, %B %d, %Y %H:%M:%S");
  }
  else
  {
    Serial.printf("[time] SNTP sync required (wake cause=%d, RTC=%s).\n",
                  static_cast<int>(wakeupCause),
                  rtcTimeValid ? "valid" : "invalid");
    configTzTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);
    if (waitForSNTPSync(&timeInfo))
    {
      rtcTimeStateMagic = RTC_TIME_STATE_MAGIC;
      rtcLastNtpSyncEpoch = time(nullptr);
      timeConfigured = true;
      Serial.println("[time] SNTP synchronization completed; RTC retained.");
    }
    else if (readRtcTime(&timeInfo, &rtcEpoch))
    {
      // A temporary NTP outage must not block weather updates while the RTC
      // still contains a usable time from an earlier synchronization.
      timeConfigured = true;
      Serial.println("[time] SNTP failed; continuing with retained RTC time.");
      Serial.println(&timeInfo, "%A, %B %d, %Y %H:%M:%S");
    }
  }

  if (!timeConfigured)
  {
    Serial.println(TXT_TIME_SYNCHRONIZATION_FAILED);
    killWiFi();
    showActionScreen(wi_time_4_128x128,
                     TXT_TIME_SYNCHRONIZATION_FAILED,
                     TXT_CHECK_NETWORK_AND_RESET,
                     TXT_RETRY_NEXT_UPDATE);
    beginDeepSleep(startTime, &timeInfo);
    return;
  }

  // MAKE API REQUESTS
  setApiCpuFrequency(API_CPU_FREQUENCY_MHZ);
#ifdef USE_HTTP
  WiFiClient client;
#elif defined(USE_HTTPS_NO_CERT_VERIF)
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(25); // seconds
#elif defined(USE_HTTPS_WITH_CERT_VERIF)
  WiFiClientSecure client;
  client.setCACert(cert_Sectigo_RSA_Domain_Validation_Secure_Server_CA);
  client.setHandshakeTimeout(25); // seconds
#endif

#ifdef WEATHER_PROVIDER_OPENWEATHER
  int rxStatus = getOWMonecall(client, owm_onecall);
#elif defined(WEATHER_PROVIDER_CAIYUN)
  int rxStatus = getCYWeather(client, owm_onecall);
#endif
  if (rxStatus != HTTP_CODE_OK)
  {
    setApiCpuFrequency(POWER_SAVE_CPU_FREQUENCY_MHZ);
    killWiFi();
#ifdef WEATHER_PROVIDER_OPENWEATHER
    statusStr = "One Call " + OWM_ONECALL_VERSION + " API";
#else
    statusStr = "Caiyun Weather API";
#endif
    tmpStr = String(rxStatus, DEC) + ": " + getHttpResponsePhrase(rxStatus);
    showActionScreen(wi_cloud_down_128x128,
                     statusStr,
                     tmpStr,
                     TXT_RESET_TO_RETRY,
                     TXT_RETRY_NEXT_UPDATE);
    beginDeepSleep(startTime, &timeInfo);
    return;
  }

#ifdef WEATHER_PROVIDER_OPENWEATHER
  rxStatus = getOWMairpollution(client, owm_air_pollution);
  if (rxStatus != HTTP_CODE_OK)
  {
    setApiCpuFrequency(POWER_SAVE_CPU_FREQUENCY_MHZ);
    killWiFi();
    statusStr = "Air Pollution API";
    tmpStr = String(rxStatus, DEC) + ": " + getHttpResponsePhrase(rxStatus);
    showActionScreen(wi_cloud_down_128x128,
                     statusStr,
                     tmpStr,
                     TXT_RESET_TO_RETRY,
                     TXT_RETRY_NEXT_UPDATE);
    beginDeepSleep(startTime, &timeInfo);
    return;
  }
  owm_onecall.current.aqi = getAQI(owm_air_pollution);
#endif

  setApiCpuFrequency(POWER_SAVE_CPU_FREQUENCY_MHZ);

  // One Call does not return a city name. Caiyun may also omit its adcodes
  // field, so use the configured city as a provider-independent fallback.
  if (owm_onecall.city_name.isEmpty())
  {
    owm_onecall.city_name = CITY_STRING;
  }

  killWiFi(); // WiFi no longer needed

  // GET INDOOR TEMPERATURE AND HUMIDITY, start BME280...
  pinMode(PIN_BME_PWR, OUTPUT);
  digitalWrite(PIN_BME_PWR, HIGH);
  float inTemp     = NAN;
  float inHumidity = NAN;
  Serial.print(String(TXT_READING_FROM) + " BME280... ");
  TwoWire I2C_bme = TwoWire(0);
  Adafruit_BME280 bme;

  I2C_bme.begin(PIN_BME_SDA, PIN_BME_SCL, 100000); // 100kHz
  if(bme.begin(BME_ADDRESS, &I2C_bme))
  {
    inTemp     = bme.readTemperature(); // Celsius
    inHumidity = bme.readHumidity();    // %

    // check if BME readings are valid
    // note: readings are checked again before drawing to screen. If a reading
    //       is not a number (NAN) then an error occurred, a dash '-' will be
    //       displayed.
    if (std::isnan(inTemp) || std::isnan(inHumidity))
    {
      statusStr = "BME " + String(TXT_READ_FAILED);
      Serial.println(statusStr);
    }
    else
    {
      Serial.println(TXT_SUCCESS);
    }
  }
  else
  {
    statusStr = "BME " + String(TXT_NOT_FOUND); // check wiring
    Serial.println(statusStr);
  }
  digitalWrite(PIN_BME_PWR, LOW);

  String refreshTimeStr;
  getRefreshTimeStr(refreshTimeStr, timeConfigured, &timeInfo);
  String dateStr;
  getDateStr(dateStr, &timeInfo);
  // RENDER FULL REFRESH
  initDisplay();
  do
  {
    drawCurrentConditions(owm_onecall.current, owm_onecall.daily[0], owm_air_pollution, inTemp, inHumidity);
    drawForecast(owm_onecall.daily, timeInfo);
    drawLocationDate(owm_onecall.city_name, dateStr);
    drawOutlookGraph(owm_onecall.hourly, timeInfo);
#if DISPLAY_ALERTS
    drawAlerts(owm_onecall.alerts, CITY_STRING, dateStr);
#endif
    drawStatusBar(statusStr, refreshTimeStr, wifiRSSI, batteryVoltage);
  } while (display.nextPage());
  powerOffDisplay();

  // DEEP SLEEP
  beginDeepSleep(startTime, &timeInfo);
} // end setup

/* This will never run
 */
void loop()
{
} // end loop


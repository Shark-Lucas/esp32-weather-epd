/* Runtime WiFi provisioning for esp32-weather-epd. */

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include "config.h"
#include "wifi_provisioning.h"

namespace
{
constexpr const char *NETWORK_NVS_NAMESPACE = "weather_net";
constexpr const char *FAILURE_NVS_KEY = "failure";
constexpr const char *DOUBLE_RESET_NVS_KEY = "drd";

bool isManualResetCandidate()
{
  const esp_reset_reason_t resetReason = esp_reset_reason();

  // Timer wake-ups and all other deep-sleep wake sources must never count as
  // a reset-button press. This keeps the normal 30-minute wake cycle free of
  // NVS writes and provisioning delays.
  if (resetReason == ESP_RST_DEEPSLEEP)
  {
    return false;
  }

  return resetReason == ESP_RST_POWERON || resetReason == ESP_RST_EXT;
}

void clearDoubleResetMarker()
{
  Preferences networkPrefs;
  if (!networkPrefs.begin(NETWORK_NVS_NAMESPACE, false))
  {
    return;
  }
  if (networkPrefs.isKey(DOUBLE_RESET_NVS_KEY))
  {
    networkPrefs.remove(DOUBLE_RESET_NVS_KEY);
  }
  networkPrefs.end();
}
} // namespace

bool hasStoredWiFiCredentials()
{
  WiFi.mode(WIFI_STA);

  wifi_config_t stationConfig = {};
  const esp_err_t result = esp_wifi_get_config(WIFI_IF_STA, &stationConfig);
  return result == ESP_OK && stationConfig.sta.ssid[0] != '\0';
}

WiFiProvisioningTrigger getWiFiProvisioningTrigger(bool credentialsAvailable)
{
  if (!isManualResetCandidate())
  {
    // Clear a stale marker after watchdog, software, panic, or brownout resets.
    // Deep-sleep wake-ups intentionally avoid touching NVS.
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP)
    {
      clearDoubleResetMarker();
    }
    return WiFiProvisioningTrigger::NONE;
  }

  if (!credentialsAvailable)
  {
    clearDoubleResetMarker();
    return WiFiProvisioningTrigger::NO_CREDENTIALS;
  }

  if (getRecordedWiFiFailure() != WiFiFailureReason::NONE)
  {
    clearDoubleResetMarker();
    return WiFiProvisioningTrigger::PREVIOUS_FAILURE;
  }

  Preferences networkPrefs;
  if (!networkPrefs.begin(NETWORK_NVS_NAMESPACE, false))
  {
    return WiFiProvisioningTrigger::NONE;
  }

  if (networkPrefs.getBool(DOUBLE_RESET_NVS_KEY, false))
  {
    networkPrefs.remove(DOUBLE_RESET_NVS_KEY);
    networkPrefs.end();
    return WiFiProvisioningTrigger::DOUBLE_RESET;
  }

  networkPrefs.putBool(DOUBLE_RESET_NVS_KEY, true);
  networkPrefs.end();

  Serial.printf("Press RESET again within %lu ms to enter WiFi setup.\n",
                WIFI_DOUBLE_RESET_WINDOW);
  delay(WIFI_DOUBLE_RESET_WINDOW);
  clearDoubleResetMarker();
  return WiFiProvisioningTrigger::NONE;
}

String getWiFiProvisioningAPName()
{
  char apName[13];
  const uint16_t suffix = static_cast<uint16_t>(ESP.getEfuseMac() & 0xFFFFULL);
  snprintf(apName, sizeof(apName), "EPD-%04X", suffix);
  return String(apName);
}

String getWiFiProvisioningAPPassword()
{
  char apPassword[17];
  const uint32_t suffix = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFFFFULL);
  snprintf(apPassword, sizeof(apPassword), "epd-%08lX",
           static_cast<unsigned long>(suffix));
  return String(apPassword);
}

WiFiProvisioningOutcome startWiFiProvisioning(const String &apName,
                                              const String &apPassword)
{
  WiFiProvisioningOutcome outcome;
  WiFiManager manager;

  manager.setConfigPortalBlocking(true);
  manager.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT);
  manager.setConnectTimeout(WIFI_TIMEOUT / 1000UL);
  manager.setSaveConnectTimeout(WIFI_SAVE_CONNECT_TIMEOUT);
  manager.setWebPortalClientCheck(false);
  manager.setAPClientCheck(false);
  manager.setShowInfoErase(false);
  manager.setShowInfoUpdate(false);
  manager.setTitle("Weather EPD WiFi");
  manager.setDarkMode(true);
  manager.setDebugOutput(DEBUG_LEVEL >= 1);

  const char *menu[] = {"wifi", "exit"};
  manager.setMenu(menu, 2);
  manager.setAPCallback([&outcome](WiFiManager *) {
    outcome.portalStarted = true;
  });

  const bool portalResult = manager.startConfigPortal(apName.c_str(),
                                                       apPassword.c_str());
  outcome.lastStatus = static_cast<wl_status_t>(manager.getLastConxResult());
  outcome.connected = portalResult && WiFi.status() == WL_CONNECTED;
  return outcome;
}

WiFiFailureReason classifyWiFiFailure(wl_status_t status)
{
  switch (status)
  {
  case WL_NO_SSID_AVAIL:
    return WiFiFailureReason::NETWORK_NOT_FOUND;
  case WL_CONNECT_FAILED:
    return WiFiFailureReason::CREDENTIALS_REJECTED;
  case WL_CONNECTION_LOST:
  case WL_DISCONNECTED:
  case WL_IDLE_STATUS:
  default:
    return WiFiFailureReason::CONNECTION_TIMEOUT;
  }
}

WiFiFailureReason getRecordedWiFiFailure()
{
  Preferences networkPrefs;
  if (!networkPrefs.begin(NETWORK_NVS_NAMESPACE, true))
  {
    return WiFiFailureReason::NONE;
  }

  const uint8_t rawReason = networkPrefs.getUChar(
      FAILURE_NVS_KEY, static_cast<uint8_t>(WiFiFailureReason::NONE));
  networkPrefs.end();

  if (rawReason > static_cast<uint8_t>(WiFiFailureReason::CONNECTION_TIMEOUT))
  {
    return WiFiFailureReason::CONNECTION_TIMEOUT;
  }
  return static_cast<WiFiFailureReason>(rawReason);
}

bool recordWiFiFailure(WiFiFailureReason reason)
{
  const WiFiFailureReason previous = getRecordedWiFiFailure();
  if (previous == reason)
  {
    return false;
  }

  Preferences networkPrefs;
  if (!networkPrefs.begin(NETWORK_NVS_NAMESPACE, false))
  {
    return false;
  }

  if (reason == WiFiFailureReason::NONE)
  {
    networkPrefs.remove(FAILURE_NVS_KEY);
  }
  else
  {
    networkPrefs.putUChar(FAILURE_NVS_KEY, static_cast<uint8_t>(reason));
  }
  networkPrefs.end();
  return true;
}

void clearWiFiFailure()
{
  recordWiFiFailure(WiFiFailureReason::NONE);
}

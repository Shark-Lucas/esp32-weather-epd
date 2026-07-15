/* Runtime WiFi provisioning for esp32-weather-epd. */

#ifndef __WIFI_PROVISIONING_H__
#define __WIFI_PROVISIONING_H__

#include <Arduino.h>
#include <WiFi.h>

enum class WiFiProvisioningTrigger : uint8_t
{
  NONE = 0,
  NO_CREDENTIALS,
  PREVIOUS_FAILURE,
  DOUBLE_RESET
};

enum class WiFiFailureReason : uint8_t
{
  NONE = 0,
  NO_CREDENTIALS,
  PORTAL_START_FAILED,
  PORTAL_NOT_COMPLETED,
  NETWORK_NOT_FOUND,
  CREDENTIALS_REJECTED,
  CONNECTION_TIMEOUT
};

struct WiFiProvisioningOutcome
{
  bool connected = false;
  bool portalStarted = false;
  wl_status_t lastStatus = WL_IDLE_STATUS;
};

bool hasStoredWiFiCredentials();
WiFiProvisioningTrigger getWiFiProvisioningTrigger(bool credentialsAvailable);

String getWiFiProvisioningAPName();
String getWiFiProvisioningAPPassword();
WiFiProvisioningOutcome startWiFiProvisioning(const String &apName,
                                              const String &apPassword);

WiFiFailureReason classifyWiFiFailure(wl_status_t status);
WiFiFailureReason getRecordedWiFiFailure();
bool recordWiFiFailure(WiFiFailureReason reason);
void clearWiFiFailure();

#endif

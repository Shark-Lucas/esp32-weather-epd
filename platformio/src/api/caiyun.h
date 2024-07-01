#ifndef __CAIYUN_H__
#define __CAIYUN_H__

#include <cstdint>
#include <vector>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "api_response.h"

#ifdef USE_HTTP
int getCYWeather(WiFiClient &client, owm_resp_onecall_t &r);
#else
int getCYWeather(WiFiClientSecure &client, owm_resp_onecall_t &r);
#endif


#endif

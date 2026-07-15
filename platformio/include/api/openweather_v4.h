/* Independent One Call 4.0 response adapter.
 *
 * One Call 4.0 splits the 3.0 aggregate response into current, hourly,
 * daily, and alert-detail endpoints. These functions map each response into
 * the renderer's existing provider-neutral owm_resp_onecall_t structure.
 */

#ifndef __OPENWEATHER_V4_H__
#define __OPENWEATHER_V4_H__

#include <cstddef>
#include <cstdint>
#include <vector>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "api_response.h"

DeserializationError deserializeOneCallV4Current(
    WiFiClient &json, owm_resp_onecall_t &response,
    std::vector<String> &alertIds);

DeserializationError deserializeOneCallV4Hourly(
    WiFiClient &json, owm_resp_onecall_t &response, size_t destinationOffset,
    size_t &recordsRead, int64_t &lastTimestamp,
    std::vector<String> &alertIds);

DeserializationError deserializeOneCallV4Daily(
    WiFiClient &json, owm_resp_onecall_t &response, size_t &recordsRead,
    std::vector<String> &alertIds);

DeserializationError deserializeOneCallV4Alert(
    WiFiClient &json, owm_resp_onecall_t &response);

#endif

"""Run the actual WiFi recovery helper against deterministic host IO doubles."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "platformio/src/client_utils.cpp").read_text(encoding="utf-8")
start = source.index("bool reconnectWiFi(")
end = source.index("\nconst String &getOwmEndpoint", start)
helper = source[start:end]
config = (ROOT / "platformio/src/config.cpp").read_text(encoding="utf-8")
import re
limit = int(re.search(r"WIFI_MAX_RECONNECT_ATTEMPTS\s*=\s*(\d+)", config).group(1))
assert limit == 20

prefix = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
constexpr int WL_CONNECTED = 3;
constexpr uint8_t WIFI_MAX_RECONNECT_ATTEMPTS = 20;
constexpr uint32_t WIFI_TIMEOUT = 10000, OWM_RETRY_DELAY = 3000;
uint32_t now = 0;
uint8_t wifiReconnectAttempts = 0;
bool owmRegionalAddressReady = true, owmGlobalAddressReady = true;
int dnsRestores = 0;
uint32_t millis() { return now; }
void delay(uint32_t ms) { now += ms; }
void configureWiFiDns() { ++dnsRestores; }
struct { template<class... Args> void printf(const char *, Args...) {} } Serial;
struct FakeWiFi {
  int state = 0, calls = 0, begins = 0, succeedsAt = 0;
  bool accepted = true, pending = false;
  uint32_t ready = 0;
  int status() { if (pending && int32_t(now-ready) >= 0) { state = WL_CONNECTED; pending = false; } return state; }
  bool reconnect() { ++calls; if (accepted && calls == succeedsAt) { pending = true; ready = now+150; } return accepted; }
  void begin() { ++begins; if (calls == succeedsAt) { pending = true; ready = now+150; } }
  int RSSI() { return -78; }
} WiFi;
void reset() { now = 0; wifiReconnectAttempts = 0; WiFi = {}; dnsRestores = 0;
  owmRegionalAddressReady = owmGlobalAddressReady = true; }
"""
checks = r"""
int main() {
  reset(); WiFi.state = WL_CONNECTED;
  assert(reconnectWiFi() && WiFi.calls == 0 && dnsRestores == 0);

  reset(); assert(!reconnectWiFi());
  assert(WiFi.calls == 20 && wifiReconnectAttempts == 20 && now == 257000);
  assert(!reconnectWiFi() && WiFi.calls == 20); // Shared budget never resets per API.

  reset(); WiFi.succeedsAt = 3;
  assert(reconnectWiFi() && WiFi.calls == 3 && wifiReconnectAttempts == 3);
  assert(dnsRestores == 1 && !owmRegionalAddressReady && !owmGlobalAddressReady);
  assert(reconnectWiFi() && WiFi.calls == 3); // Connected path does not disconnect.
  WiFi.state = 0; WiFi.succeedsAt = 4;
  assert(reconnectWiFi() && wifiReconnectAttempts == 4); // Later drop resumes budget.

  reset(); assert(!reconnectWiFi(120000));
  assert(now == 120000 && WiFi.calls == 10 && wifiReconnectAttempts == 10);
  reset(); assert(!reconnectWiFi(0) && WiFi.calls == 0);

  reset(); WiFi.accepted = false; WiFi.succeedsAt = 2;
  assert(reconnectWiFi() && WiFi.calls == 2 && WiFi.begins == 2);

  reset(); now = UINT32_MAX-100; WiFi.succeedsAt = 1;
  assert(reconnectWiFi() && wifiReconnectAttempts == 1 && now == 49);

  reset(); wifiReconnectAttempts = 1; WiFi.pending = true; WiFi.ready = 1000;
  assert(reconnectWiFi() && WiFi.calls == 0 && wifiReconnectAttempts == 1);
}
"""
with tempfile.TemporaryDirectory(prefix="weather-wifi-check-") as work:
    path = Path(work)
    cpp = path / "check.cpp"
    exe = path / ("check.exe" if os.name == "nt" else "check")
    cpp.write_text(prefix + helper + checks, encoding="utf-8")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++11", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: actual helper reconnects at most 20 times; success, exhaustion, fallback, DNS reset, shared budget, deadline and millis wrap checked.")

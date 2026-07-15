# Repository guide for coding agents

## Project layout

- The PlatformIO firmware project is in `platformio/`; run build and upload
  commands from that directory.
- Runtime firmware code is in `platformio/src/` and public headers are in
  `platformio/include/`.
- Chinese bitmap font tooling is in `fonts/`; generated headers live under
  `platformio/lib/esp32-weather-epd-assets/fonts/`.

## Local bootstrap

1. Copy `platformio/include/secrets.example.h` to
   `platformio/include/secrets.h`.
2. Fill in the required API keys in `secrets.h`. Never commit that file.
3. Install PlatformIO Core, then build from `platformio/`:

   ```powershell
   platformio run
   ```

4. To upload, use the serial port detected on the current computer rather
   than assuming the original development port:

   ```powershell
   platformio run --target upload --upload-port COMx
   ```

WiFi credentials are provisioned at runtime and stored in ESP32 NVS; they do
not belong in source files.

## Important configuration

- `platformio/include/config.h` selects the weather provider, One Call API
  version, panel, locale, units, and HTTP/HTTPS mode.
- `OWM_ONECALL_API_VERSION` switches between independent One Call 3.0 and 4.0
  request/parser paths. Keep those adapters separate.
- The current hardware defaults are OpenWeather, One Call 4.0, `zh_CN`,
  `DISP_BW_V2`, and `DRIVER_DESPI_C02`.
- Non-secret location, timing, endpoint, and pin settings are in
  `platformio/src/config.cpp`.

## Verification

- There is no separate unit-test suite; a full PlatformIO build is the primary
  regression check:

  ```powershell
  platformio run
  ```

- Check Chinese glyph coverage without modifying generated files:

  ```powershell
  powershell -ExecutionPolicy Bypass -File ..\fonts\update_fonts.ps1 -Check
  ```

- Regenerate the configured bitmap font and build with
  `fonts/update_fonts.cmd`, or run `fonts/update_fonts.ps1 -Apply -Build`.
- Do not commit `.pio/`, `platformio/include/secrets.h`, serial logs, or local
  editor instruction files.

## Runtime behavior

- With no saved network, WiFi provisioning starts automatically. With a valid
  saved network, pressing RESET twice within four seconds opens the portal.
- Timer wake-ups reuse ESP32 RTC time and periodically resynchronize SNTP.
- One Call 4.0 fetches current conditions, only the hourly records consumed by
  `HOURLY_GRAPH_MAX`, the daily timeline, and optional alert details.
- Preserve the E-Paper failure screens and deep-sleep fallback paths when
  changing networking code.

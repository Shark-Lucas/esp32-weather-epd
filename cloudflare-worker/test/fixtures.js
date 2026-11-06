// Synthetic, complete 4.0/AQI fixtures: no account keys or real coordinates.
export const TOKEN = "local-test-device-token-0123456789abcdef";
export const KEY = "local-test-openweather-secret";
export const NOW = Date.parse("2026-11-06T03:15:00Z");
export const PROFILE = { lat: "31", lon: "121", lang: "en", units: "standard" };
export const QUERY = "/data/4.0/onecall/current?lat=31&lon=121&lang=en&units=standard";

export function fixture(url, { alerts = false, huge = false } = {}) {
  const u = new URL(url);
  const lat = Number(u.searchParams.get("lat") ?? 31);
  const lon = Number(u.searchParams.get("lon") ?? 121);
  const root = { lat, lon, timezone: "Asia/Shanghai", timezone_offset: 28800, unused_root: "remove" };
  const hour = Math.floor(Date.now() / 3600000) * 3600;
  const weather = [{ id: 800, main: "Clear", description: "clear sky", icon: "01d", unused: "remove" }];
  const common = { dt: hour, temp: 291.14, feels_like: 290.6, humidity: 64, pressure: 1013,
    dew_point: 283.6, uvi: 1.2, clouds: 4, visibility: 10000, wind_speed: 2.37,
    wind_deg: 60, wind_gust: 4.62, rain: { "1h": 0.05, unused: 1 }, snow: { "1h": 0 },
    unused: "remove".repeat(80), ...(alerts ? { alerts: ["wind-1", "wind-1"] } : {}) };
  if (u.pathname.includes("/alert/")) return { event: "Wind", sender_name: "Station", start: hour,
    end: hour + 7200, description: "unconsumed description".repeat(80), unused: "remove" };
  if (u.pathname.includes("air_pollution")) {
    const end = Number(u.searchParams.get("end"));
    const at = Math.floor(end / 3600) * 3600;
    return { coord: { lat, lon, unused: true }, unused: "remove", list: Array.from({ length: 24 }, (_, i) => ({
      dt: at - (23 - i) * 3600, main: { aqi: 2, unused: true },
      components: { co: 201.01, no: 0.2, no2: 9.3, o3: 52.4, so2: 2.1, pm2_5: 12.1234,
        pm10: 25.5, nh3: 0.1, unused: true }, unused: "remove" })) };
  }
  if (u.pathname.endsWith("/current")) return { ...root, data: [{ ...common,
    dt: Math.floor(Date.now() / 1000), sunrise: hour - 18000, sunset: hour + 21600, weather,
    ...(huge ? { weather: [{ ...weather[0], description: "x".repeat(530000) }] } : {}) }] };
  const count = Number(u.searchParams.get("cnt"));
  const start = Number(u.searchParams.get("start") ?? hour);
  if (u.pathname.endsWith("/1h")) return { ...root, data: Array.from({ length: count }, (_, i) => ({
    ...common, dt: start + i * 3600, pop: 0.2525, weather })) };
  return { ...root, data: Array.from({ length: count }, (_, i) => ({ ...common,
    dt: Math.floor(hour / 86400) * 86400 + i * 86400,
    sunrise: hour - 18000 + i * 86400, sunset: hour + 21600 + i * 86400,
    moonrise: hour, moonset: hour + 18000, moon_phase: 0.1234, pop: 0.2525,
    temp: { morn: 287.1, day: 292.2, eve: 289.3, night: 286.4, min: 284.2, max: 295.9, unused: true },
    feels_like: { morn: 286.1, day: 291.2, eve: 288.3, night: 285.4, unused: true },
    rain: 1.2345, snow: 0, weather })) };
}

export class MemoryKV {
  values = new Map();
  reads = 0;
  writes = 0;
  failReads = false;
  failWrites = false;
  async get(key, options = {}) {
    this.reads++;
    if (this.failReads) throw new Error("KV unavailable");
    const value = this.values.get(key);
    return value === undefined ? null : options.type === "json" ? JSON.parse(value) : value;
  }
  async put(key, value) {
    if (this.failWrites) throw new Error("KV unavailable");
    this.writes++; this.values.set(key, value);
  }
}

export class MemoryStorage {
  values = new Map();
  writes = 0;
  alarm = null;
  async get(key) { return structuredClone(this.values.get(key)); }
  async put(key, value) { this.writes++; this.values.set(key, structuredClone(value)); }
  async getAlarm() { return this.alarm; }
  async setAlarm(at) { this.alarm = at; }
  async deleteAlarm() { this.alarm = null; }
}

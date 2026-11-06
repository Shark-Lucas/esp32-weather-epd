import { MAX_BYTES, RequestError, getConfig } from "./shared.js";

const UPSTREAMS = ["api.openweathermap.org", "cn-api.openweathermap.org"];
const ROOT = ["lat", "lon", "timezone", "timezone_offset"];
const COMMON = ["dt", "temp", "feels_like", "pressure", "humidity", "dew_point", "uvi", "clouds",
  "visibility", "wind_speed", "wind_gust", "wind_deg", "rain", "snow", "alerts"];
const WEATHER = ["id", "main", "description", "icon"];
const COMPONENTS = ["co", "no", "no2", "o3", "so2", "pm2_5", "pm10", "nh3"];
const pick = (value, names) => Object.fromEntries(names.filter((name) => value?.[name] !== undefined)
  .map((name) => [name, value[name]]));
const finite = (value) => typeof value === "number" && Number.isFinite(value);
const timestamp = (value) => Number.isSafeInteger(value) && value > 0;
function check(condition) { if (!condition) throw new Error("invalid_weather_payload"); }

// Match the independent 4.0 firmware filters. Never fill missing observations
// with zero or strip timestamps: old-but-valid data must remain identifiable.
export function projectPayload(data, kind, expectedCount) {
  check(data && typeof data === "object" && !Array.isArray(data)
    && (data.cod === undefined || Number(data.cod) === 200));
  if (kind === "alert") {
    check(typeof data.event === "string" && data.event.length > 0 && timestamp(data.start)
      && timestamp(data.end) && data.end > data.start);
    return pick(data, ["sender_name", "event", "start", "end"]);
  }
  if (kind === "air") {
    check(finite(data.coord?.lat) && finite(data.coord?.lon)
      && Array.isArray(data.list) && data.list.length === expectedCount);
    const list = data.list.map((item) => {
      check(timestamp(item.dt) && Number.isInteger(item.main?.aqi) && item.main.aqi >= 1 && item.main.aqi <= 5
        && COMPONENTS.every((key) => finite(item.components?.[key]) && item.components[key] >= 0));
      return { dt: item.dt, main: { aqi: item.main.aqi }, components: pick(item.components, COMPONENTS) };
    });
    check(list.every((item, i) => !i || item.dt - list[i - 1].dt === 3600));
    return { coord: pick(data.coord, ["lat", "lon"]), list };
  }
  check(finite(data.lat) && finite(data.lon) && typeof data.timezone === "string"
    && finite(data.timezone_offset) && Array.isArray(data.data) && data.data.length === expectedCount);
  const records = data.data.map((item) => {
    check(timestamp(item.dt));
    if (kind === "daily") check(finite(item.temp?.min) && finite(item.temp?.max) && item.temp.min <= item.temp.max);
    else check(finite(item.temp));
    const result = pick(item, [...COMMON, ...(kind === "hourly" ? ["pop"]
      : kind === "current" ? ["sunrise", "sunset"]
      : ["sunrise", "sunset", "moonrise", "moonset", "moon_phase", "pop"])]);
    if (kind === "daily") {
      result.temp = pick(item.temp, ["morn", "day", "eve", "night", "min", "max"]);
      if (item.feels_like !== undefined) result.feels_like = pick(item.feels_like, ["morn", "day", "eve", "night"]);
    }
    for (const name of ["rain", "snow"]) {
      if (result[name] && typeof result[name] === "object") result[name] = pick(result[name], ["1h"]);
    }
    if (kind !== "hourly" && item.weather !== undefined) {
      check(Array.isArray(item.weather) && Number.isInteger(item.weather[0]?.id) && item.weather[0].id > 0);
      result.weather = [pick(item.weather[0], WEATHER)];
    }
    if (kind === "current") check(result.weather?.[0]?.id > 0);
    // Sunrise/set can legitimately be absent at polar locations.
    for (const name of ["sunrise", "sunset"]) if (item[name] !== undefined) check(timestamp(item[name]));
    for (const key of ["pressure", "humidity", "clouds", "wind_speed", "wind_deg"]) check(finite(item[key]));
    if (item.alerts !== undefined) {
      check(Array.isArray(item.alerts) && item.alerts.every((id) => typeof id === "string" && /^[A-Za-z0-9_-]{1,128}$/.test(id)));
      result.alerts = [...new Set(item.alerts)].slice(0, 8);
    }
    return result;
  });
  check(records.every((item, i) => !i || (kind === "hourly"
    ? item.dt - records[i - 1].dt === 3600 : item.dt > records[i - 1].dt)));
  return { ...pick(data, ROOT), data: records };
}

async function readJson(response) {
  if (!response.body || Number(response.headers.get("Content-Length")) > MAX_BYTES) {
    await response.body?.cancel();
    throw new Error("invalid_body_size");
  }
  const reader = response.body.getReader();
  const decoder = new TextDecoder("utf-8", { fatal: true });
  let bytes = 0;
  let text = "";
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      bytes += value.byteLength;
      if (bytes > MAX_BYTES) { await reader.cancel(); throw new Error("body_too_large"); }
      text += decoder.decode(value, { stream: true });
    }
    return JSON.parse(text + decoder.decode());
  } finally { reader.releaseLock(); }
}

function retryAt(header, now) {
  const value = Number(header);
  const at = header && Number.isFinite(value) ? now + Math.max(1, value) * 1000 : Date.parse(header);
  // Bound untrusted Retry-After, but never immediately retry a 429.
  return Math.min(now + 86400000, Math.max(now + 60000, Number.isFinite(at) ? at : now + 900000));
}

export async function fetchWeather(env, profile, path, query, kind, count, reserve, delay = sleep) {
  const { timeoutMs } = getConfig(env);
  const params = new URLSearchParams({ ...query });
  if (kind !== "alert") {
    params.set("lat", profile.lat); params.set("lon", profile.lon);
    if (kind !== "air") { params.set("lang", profile.lang); params.set("units", profile.units); }
  }
  for (let attempt = 0; attempt < 3; attempt++) {
    if (attempt) await delay([500, 1500][attempt - 1]);
    await reserve(count); // Persist quota BEFORE every billable attempt, including retries.
    const url = new URL(path, `https://${UPSTREAMS[attempt % UPSTREAMS.length]}`);
    url.search = params.toString();
    url.searchParams.set("appid", env.OPENWEATHER_API_KEY.trim());
    const controller = new AbortController();
    let timer;
    let failure = "upstream_fetch_failed";
    let terminal = null;
    try {
      const payload = await Promise.race([
        new Promise((_, reject) => {
          timer = setTimeout(() => { controller.abort(); reject(new Error("timeout")); }, timeoutMs);
        }),
        (async () => {
          const response = await fetch(url.toString(), { method: "GET", headers: { Accept: "application/json" },
            redirect: "manual", signal: controller.signal });
          if (response.status !== 200) {
            const status = response.status;
            const after = status === 429 ? retryAt(response.headers.get("Retry-After"), Date.now()) : 0;
            await response.body?.cancel();
            failure = `upstream_http_${status}`;
            if (status === 429 || (status >= 400 && status < 500 && status !== 408)) {
              terminal = new RequestError(status, failure);
              terminal.retryAt = after;
              if (after && reserve.cooldown) await reserve.cooldown(after);
            }
            throw new Error("upstream_status");
          }
          failure = "upstream_invalid_payload";
          return projectPayload(await readJson(response), kind, count);
        })(),
      ]);
      return payload;
    } catch {
      if (terminal) throw terminal;
      if (attempt === 2) throw new RequestError(502, controller.signal.aborted ? "upstream_timeout" : failure);
    } finally { clearTimeout(timer); }
  }
}

export function sleep(ms) { return new Promise((resolve) => setTimeout(resolve, ms)); }

async function mapFour(items, run) {
  let cursor = 0;
  const results = new Array(items.length);
  let failure;
  await Promise.all(Array.from({ length: Math.min(4, items.length) }, async () => {
    while (!failure && cursor < items.length) {
      const index = cursor++;
      try { results[index] = await run(items[index]); } catch (error) { failure ??= error; }
    }
  }));
  if (failure) throw failure;
  return results;
}

export async function collectProfile(env, profile, reserve, delay) {
  const end = Math.floor(Date.now() / 1000);
  const airWindow = { start: end - 86399, end };
  const fetchPart = (path, query, kind, count) => fetchWeather(env, profile, path, query, kind, count, reserve, delay);
  const [current, firstHourly, daily, air] = await mapFour([
    ["/data/4.0/onecall/current", {}, "current", 1],
    ["/data/4.0/onecall/timeline/1h", { cnt: "20" }, "hourly", 20],
    ["/data/4.0/onecall/timeline/1day", { cnt: "8" }, "daily", 8],
    ["/data/2.5/air_pollution/history", airWindow, "air", 24],
  ], (args) => fetchPart(...args));
  const start = firstHourly.data.at(-1).dt + 3600;
  const secondHourly = await fetchPart("/data/4.0/onecall/timeline/1h", { cnt: "4", start: String(start) }, "hourly", 4);
  check(secondHourly.data[0].dt === start);
  // Reject cross-location roots or mismatched forecast timezones. Air APIs
  // may round their coordinates, so use the provider's documented precision.
  for (const part of [current, firstHourly, secondHourly, daily]) {
    check(Math.abs(part.lat - Number(profile.lat)) <= 0.001
      && Math.abs(part.lon - Number(profile.lon)) <= 0.001
      && part.timezone === current.timezone && part.timezone_offset === current.timezone_offset);
  }
  check(Math.abs(air.coord.lat - Number(profile.lat)) <= 0.001
    && Math.abs(air.coord.lon - Number(profile.lon)) <= 0.001);
  const hourly = { ...firstHourly, data: [...firstHourly.data, ...secondHourly.data] };
  const records = [...current.data, ...hourly.data, ...daily.data];
  const alertIds = [...new Set(records.flatMap((record) => record.alerts ?? []))].slice(0, 8);
  const alerts = Object.fromEntries(await mapFour(alertIds, async (id) =>
    [id, await fetchPart(`/data/4.0/onecall/alert/${id}`, {}, "alert", 1)]));
  // Expose only references whose details are part of this complete snapshot.
  for (const record of records) if (record.alerts) record.alerts = record.alerts.filter((id) => Object.hasOwn(alerts, id));
  return { profile, fetchedAt: Date.now(), current, hourly, daily, air, airWindow, alerts };
}

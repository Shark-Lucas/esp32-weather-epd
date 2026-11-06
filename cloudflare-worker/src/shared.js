export const SNAPSHOT_KEY = "weather:snapshot:v2";
export const MAX_BYTES = 512 * 1024;
export const INTERVAL_MS = 15 * 60 * 1000;
export const INACTIVE_MS = 7 * 86400 * 1000;
export const MAX_PROFILES = 10;

export class RequestError extends Error {
  constructor(status, code) { super(code); this.status = status; }
}

export function jsonResponse(body, status = 200, headers = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "private, no-store", Vary: "Authorization", ...headers },
  });
}

export function jsonError(status, code, headers = {}) {
  return jsonResponse({ error: code }, status, { "X-Weather-Error": code, ...headers });
}

export function setting(value, fallback, min, max) {
  const number = value === undefined ? fallback : Number(value);
  if (!Number.isSafeInteger(number) || number < min || number > max) {
    throw new RequestError(503, "invalid_worker_configuration");
  }
  return number;
}

export function getConfig(env) {
  if (!env.WEATHER_CACHE?.get || !env.WEATHER_CACHE?.put || !env.WEATHER_COORDINATOR?.get
      || typeof env.DEVICE_TOKEN !== "string" || env.DEVICE_TOKEN.length < 32
      || env.DEVICE_TOKEN.length > 256) throw new RequestError(503, "worker_not_configured");
  const freshSeconds = setting(env.CACHE_TTL_SECONDS, 900, 60, 86400);
  return { freshSeconds,
    staleSeconds: setting(env.STALE_TTL_SECONDS, 10800, freshSeconds, 86400),
    timeoutMs: setting(env.UPSTREAM_TIMEOUT_MS, 5000, 50, 10000) };
}

export function budgetConfig(env) {
  if (!env.OPENWEATHER_API_KEY?.trim()) throw new RequestError(503, "worker_not_configured");
  // Owner opted out of local quota enforcement. Provider 429 cooldown remains.
  if (env.OWM_BUDGET_ENFORCED === "false") return { enforced: false };
  const limit = setting(env.OWM_DAILY_BUDGET, 0, 0, 10000000);
  const unit = env.OWM_BUDGET_UNIT;
  if (env.OWM_BUDGET_CONFIRMED !== "true" || limit === 0
      || !["request", "record"].includes(unit)) {
    throw new RequestError(503, "upstream_budget_not_configured");
  }
  return { enforced: true, limit, unit, resetHour: setting(env.OWM_BUDGET_RESET_UTC_HOUR, 0, 0, 23) };
}

export function tokenMatches(actual, expected) {
  let difference = actual.length ^ expected.length;
  for (let i = 0; i < expected.length; i++) difference |= (actual.charCodeAt(i) || 0) ^ expected.charCodeAt(i);
  return difference === 0;
}

export function dayAt(now) { return new Date(now).toISOString().slice(0, 10); }
export function budgetDay(now, resetHour) { return dayAt(now - resetHour * 3600000); }
export function profileId(profile) { return [profile.lat, profile.lon, profile.lang, profile.units].join("|"); }

export function parseRequest(url) {
  if (url.href.length > 4096) throw new RequestError(414, "request_too_long");
  const path = url.pathname;
  if (path === "/data/3.0/onecall") throw new RequestError(410, "onecall_v3_not_enabled");
  let kind;
  if (path === "/data/4.0/onecall/current") kind = "current";
  else if (path === "/data/4.0/onecall/timeline/1h") kind = "hourly";
  else if (path === "/data/4.0/onecall/timeline/1day") kind = "daily";
  else if (/^\/data\/4\.0\/onecall\/alert\/[A-Za-z0-9_-]{1,128}$/.test(path)) kind = "alert";
  else if (path === "/data/2.5/air_pollution/history") kind = "history";
  else if (path === "/data/2.5/air_pollution") kind = "air";
  else if (path === "/status") kind = "status";
  else throw new RequestError(404, "unsupported_weather_path");
  const allowed = kind === "alert" ? [] : ["lat", "lon", "lang", "units"];
  if (["hourly", "daily"].includes(kind)) allowed.push("start", "cnt");
  if (kind === "history") allowed.push("start", "end");
  const params = new URLSearchParams();
  for (const [name, value] of url.searchParams) {
    if (name === "appid") continue; // Never use a device-supplied upstream key.
    if (!allowed.includes(name) || params.has(name)) throw new RequestError(400, "unsupported_or_duplicate_parameter");
    params.set(name, value);
  }
  if (kind === "alert") return { kind, alertId: path.split("/").at(-1) };
  const profile = {};
  for (const [name, limit] of [["lat", 90], ["lon", 180]]) {
    const raw = params.get(name) ?? "";
    if (!/^-?\d+(\.\d+)?$/.test(raw) || !Number.isFinite(Number(raw)) || Math.abs(Number(raw)) > limit) {
      throw new RequestError(400, `invalid_${name}`);
    }
    profile[name] = String(Number(raw));
  }
  profile.lang = (params.get("lang") ?? "en").toLowerCase().replaceAll("-", "_");
  profile.units = params.get("units") ?? "standard";
  if (!/^[a-z_]{2,16}$/.test(profile.lang)) throw new RequestError(400, "invalid_lang");
  if (!["standard", "metric", "imperial"].includes(profile.units)) throw new RequestError(400, "invalid_units");
  const result = { kind, profile, id: profileId(profile), params };
  for (const name of ["start", "end", "cnt"]) {
    if (params.has(name)) {
      const raw = params.get(name);
      const value = Number(raw);
      if (!/^\d+$/.test(raw) || !Number.isSafeInteger(value) || value < 1 || (name === "cnt" && value > 20)) {
        throw new RequestError(400, `invalid_${name}`);
      }
      result[name] = value;
    }
  }
  if (kind === "history" && (!result.start || !result.end || result.end <= result.start
      || result.end - result.start > 172800)) throw new RequestError(400, "invalid_history_window");
  return result;
}

export function coordinator(env) {
  return env.WEATHER_COORDINATOR.get(env.WEATHER_COORDINATOR.idFromName("weather-v2"));
}

export function callCoordinator(env, action, body) {
  return coordinator(env).fetch(new Request(`https://coordinator.internal/${action}`, {
    method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
  }));
}

export function emptySnapshot() { return { version: 2, generation: 0, publishedAt: 0, profiles: {} }; }

export function validSnapshot(value) {
  return value?.version === 2 && Number.isSafeInteger(value.generation) && value.generation >= 0
    && Number.isFinite(value.publishedAt) && value.profiles && typeof value.profiles === "object"
    && !Array.isArray(value.profiles) && Object.keys(value.profiles).length <= MAX_PROFILES;
}

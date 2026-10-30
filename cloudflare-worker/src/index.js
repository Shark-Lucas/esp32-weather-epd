const UPSTREAMS = ["api.openweathermap.org", "cn-api.openweathermap.org"];
const MAX_BODY_BYTES = 512 * 1024;
const COMMON_PARAMS = ["lat", "lon", "lang", "units"];

class RequestError extends Error {
  constructor(status, code) {
    super(code);
    this.status = status;
  }
}

function jsonError(status, code, extraHeaders = {}) {
  return new Response(JSON.stringify({ error: code }), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "private, no-store",
      ...extraHeaders,
    },
  });
}

function tokenMatches(actual, expected) {
  // Compare every expected character, including when the supplied token is short.
  let difference = actual.length ^ expected.length;
  for (let i = 0; i < expected.length; i++) {
    difference |= (actual.charCodeAt(i) || 0) ^ expected.charCodeAt(i);
  }
  return difference === 0;
}

function setting(value, fallback, min, max) {
  if (value === undefined) return fallback;
  const number = Number(value);
  if (!Number.isSafeInteger(number) || number < min || number > max) {
    throw new RequestError(503, "invalid_worker_configuration");
  }
  return number;
}

function getConfig(env) {
  if (!env.OPENWEATHER_API_KEY?.trim() || !env.DEVICE_TOKEN
      || env.DEVICE_TOKEN.length < 32 || env.DEVICE_TOKEN.length > 256
      || !env.WEATHER_CACHE?.get || !env.WEATHER_CACHE?.put) {
    throw new RequestError(503, "worker_not_configured");
  }
  const freshSeconds = setting(env.CACHE_TTL_SECONDS, 600, 60, 86400);
  const staleSeconds = setting(env.STALE_TTL_SECONDS, 10800, freshSeconds, 86400);
  return {
    freshSeconds,
    staleSeconds,
    timeoutMs: setting(env.UPSTREAM_TIMEOUT_MS, 3500, 50, 10000),
  };
}

function routeFor(path) {
  if (path === "/data/3.0/onecall") {
    return { kind: "v3", allowed: [...COMMON_PARAMS, "exclude"] };
  }
  if (path === "/data/4.0/onecall/current") {
    return { kind: "v4", allowed: COMMON_PARAMS };
  }
  if (/^\/data\/4\.0\/onecall\/timeline\/(1h|1day)$/.test(path)) {
    return { kind: "v4", allowed: [...COMMON_PARAMS, "cnt", "start"] };
  }
  if (/^\/data\/4\.0\/onecall\/alert\/[A-Za-z0-9_-]{1,128}$/.test(path)) {
    return { kind: "alert", allowed: [] };
  }
  if (path === "/data/2.5/air_pollution/history") {
    return { kind: "history", allowed: ["lat", "lon", "start", "end"] };
  }
  if (path === "/data/2.5/air_pollution") {
    return { kind: "air", allowed: ["lat", "lon"] };
  }
  throw new RequestError(404, "unsupported_weather_path");
}

function integerParam(params, name, min, max, required = false) {
  if (!params.has(name) && !required) return;
  const raw = params.get(name) ?? "";
  const value = Number(raw);
  if (!/^\d+$/.test(raw) || !Number.isSafeInteger(value) || value < min || value > max) {
    throw new RequestError(400, `invalid_${name}`);
  }
  params.set(name, String(value));
}

async function makeQuery(url, route) {
  if (url.href.length > 4096) throw new RequestError(414, "request_too_long");
  const params = new URLSearchParams();
  for (const [name, value] of url.searchParams) {
    // Compatibility with firmware URLs: the caller's appid is never used.
    if (name === "appid") continue;
    if (!route.allowed.includes(name) || params.has(name)) {
      throw new RequestError(400, "unsupported_or_duplicate_parameter");
    }
    params.set(name, value);
  }
  if (route.kind !== "alert") {
    for (const [name, limit] of [["lat", 90], ["lon", 180]]) {
      const raw = params.get(name) ?? "";
      const value = Number(raw);
      if (!/^-?\d+(\.\d+)?$/.test(raw) || !Number.isFinite(value) || Math.abs(value) > limit) {
        throw new RequestError(400, `invalid_${name}`);
      }
      params.set(name, String(value));
    }
  }
  if (route.allowed.includes("units")) {
    if (!params.has("units")) params.set("units", "standard");
    if (!["standard", "metric", "imperial"].includes(params.get("units"))) {
      throw new RequestError(400, "invalid_units");
    }
  }
  if (params.has("lang") && !/^[A-Za-z_-]{2,16}$/.test(params.get("lang"))) {
    throw new RequestError(400, "invalid_lang");
  }
  if (params.has("exclude")) {
    const parts = params.get("exclude").split(",");
    if (parts.some((part) => !["current", "minutely", "hourly", "daily", "alerts"].includes(part))) {
      throw new RequestError(400, "invalid_exclude");
    }
    params.set("exclude", [...new Set(parts)].sort().join(","));
  }
  integerParam(params, "cnt", 1, 20);
  integerParam(params, "start", 1, Number.MAX_SAFE_INTEGER, route.kind === "history");
  integerParam(params, "end", 1, Number.MAX_SAFE_INTEGER, route.kind === "history");
  const logical = new URLSearchParams(params);
  if (route.kind === "history") {
    const duration = Number(params.get("end")) - Number(params.get("start"));
    if (duration < 1 || duration > 172800) throw new RequestError(400, "invalid_history_window");
    // This firmware requests a rolling 24-hour window with changing timestamps.
    // Only near-current windows share an anchor-independent last-success entry.
    if (Math.abs(Number(params.get("end")) - Date.now() / 1000) <= 900) {
      logical.delete("start");
      logical.delete("end");
      logical.set("rolling_window_seconds", String(duration));
    }
  }
  logical.sort();
  params.sort();
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`${url.pathname}?${logical}`));
  const hash = [...new Uint8Array(digest)].map((value) => value.toString(16).padStart(2, "0")).join("");
  return { params, cacheKey: `owm:v1:${hash}` };
}

function validPayload(body, kind) {
  const data = JSON.parse(body);
  if (!data || typeof data !== "object" || Array.isArray(data)) return false;
  if (data.cod !== undefined && Number(data.cod) !== 200) return false;
  if (kind === "v3") return Number(data.current?.dt) > 0;
  if (kind === "v4") {
    return Array.isArray(data.data) && data.data.length > 0
      && data.data.every((record) => Number(record?.dt) > 0);
  }
  if (kind === "alert") {
    return typeof data.event === "string" && Number(data.start) > 0 && Number(data.end) > 0;
  }
  return Array.isArray(data.list) && data.list.length > 0
    && data.list.every((record) => Number(record?.dt) > 0 && record.components);
}

async function readBody(response) {
  if (Number(response.headers.get("Content-Length")) > MAX_BODY_BYTES || !response.body) {
    await response.body?.cancel();
    throw new Error("invalid_body_size");
  }
  const reader = response.body.getReader();
  const decoder = new TextDecoder("utf-8", { fatal: true });
  let length = 0;
  let body = "";
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      length += value.byteLength;
      if (length > MAX_BODY_BYTES) {
        await reader.cancel();
        throw new Error("body_too_large");
      }
      body += decoder.decode(value, { stream: true });
    }
    return body + decoder.decode();
  } finally {
    reader.releaseLock();
  }
}

async function attemptUpstream(host, path, params, apiKey, kind, timeoutMs) {
  const url = new URL(path, `https://${host}`);
  url.search = params.toString();
  url.searchParams.set("appid", apiKey);
  const controller = new AbortController();
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => {
      controller.abort();
      reject(new Error("upstream_timeout"));
    }, timeoutMs);
  });
  try {
    return await Promise.race([timeout, (async () => {
      const response = await fetch(url.toString(), {
        method: "GET",
        headers: { Accept: "application/json" },
        redirect: "manual",
        signal: controller.signal,
      });
      if (response.status !== 200) {
        await response.body?.cancel();
        const status = response.status;
        const temporary = status === 408 || status === 429 || status >= 500 || status < 400;
        return {
          ok: false,
          status: status >= 400 && status < 500 ? status : 502,
          temporary,
          retry: temporary && status !== 429,
        };
      }
      const body = await readBody(response);
      if (!validPayload(body, kind)) throw new Error("invalid_weather_payload");
      return { ok: true, body };
    })()]);
  } catch {
    // Never expose fetch error messages: they may contain an upstream URL/key.
    return { ok: false, status: 502, temporary: true, retry: true };
  } finally {
    clearTimeout(timer);
  }
}

async function loadCache(namespace, key, kind, maxAge) {
  let timer;
  try {
    const entry = await Promise.race([
      namespace.get(key, { type: "json", cacheTtl: 60 }),
      new Promise((_, reject) => { timer = setTimeout(() => reject(new Error("kv_timeout")), 1200); }),
    ]);
    if (!entry || entry.version !== 1 || !Number.isFinite(entry.fetchedAt)
        || typeof entry.body !== "string" || entry.body.length > MAX_BODY_BYTES
        || !validPayload(entry.body, kind)) return null;
    const age = (Date.now() - entry.fetchedAt) / 1000;
    return age >= 0 && age <= maxAge ? entry : null;
  } catch {
    // KV failure must not prevent a live upstream response from reaching ESP32.
    return null;
  } finally {
    clearTimeout(timer);
  }
}

function weatherResponse(entry, state) {
  return new Response(entry.body, {
    status: 200,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "private, no-store",
      Vary: "Authorization",
      Age: String(Math.max(0, Math.floor((Date.now() - entry.fetchedAt) / 1000))),
      "X-Weather-Cache": state,
      "X-Weather-Fetched-At": new Date(entry.fetchedAt).toISOString(),
      ...(state === "STALE" ? { "X-Weather-Stale": "true" } : {}),
    },
  });
}

export default {
  async fetch(request, env, ctx) {
    try {
      if (request.method !== "GET") return jsonError(405, "method_not_allowed", { Allow: "GET" });
      const url = new URL(request.url);
      if (url.pathname === "/health") {
        return new Response('{"ok":true}', {
          headers: { "Content-Type": "application/json", "Cache-Control": "no-store" },
        });
      }
      const route = routeFor(url.pathname);
      const config = getConfig(env);
      const authorization = request.headers.get("Authorization") ?? "";
      const token = /^Bearer (.+)$/i.exec(authorization)?.[1] ?? "";
      if (!tokenMatches(token, env.DEVICE_TOKEN)) {
        return jsonError(401, "unauthorized", { "WWW-Authenticate": "Bearer" });
      }
      const { params, cacheKey } = await makeQuery(url, route);
      const cached = await loadCache(env.WEATHER_CACHE, cacheKey, route.kind, config.staleSeconds);
      if (cached && (Date.now() - cached.fetchedAt) / 1000 <= config.freshSeconds) {
        return weatherResponse(cached, "HIT");
      }
      let result;
      for (const host of UPSTREAMS) {
        result = await attemptUpstream(host, url.pathname, params, env.OPENWEATHER_API_KEY.trim(),
          route.kind, config.timeoutMs);
        if (result.ok || !result.retry) break;
      }
      if (result.ok) {
        const entry = { version: 1, fetchedAt: Date.now(), body: result.body };
        const save = Promise.resolve().then(() => env.WEATHER_CACHE.put(cacheKey, JSON.stringify(entry), {
          expirationTtl: Math.max(60, config.staleSeconds + 60),
        })).catch(() => { console.warn("[weather-cache] Last-success write failed"); });
        ctx.waitUntil(save);
        return weatherResponse(entry, "MISS");
      }
      // Recheck age after retries so a cache entry cannot expire mid-request.
      if (result.temporary && cached && (Date.now() - cached.fetchedAt) / 1000 <= config.staleSeconds) {
        return weatherResponse(cached, "STALE");
      }
      return jsonError(result.status, "upstream_unavailable", { "X-Weather-Cache": "MISS" });
    } catch (error) {
      return error instanceof RequestError
        ? jsonError(error.status, error.message)
        : jsonError(500, "relay_internal_error");
    }
  },
};

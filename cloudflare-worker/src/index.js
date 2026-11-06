import { SNAPSHOT_KEY, MAX_BYTES, RequestError, callCoordinator, dayAt, getConfig,
  jsonError, jsonResponse, parseRequest, tokenMatches, validSnapshot } from "./shared.js";
export { WeatherCoordinator } from "./coordinator.js";

async function readSnapshot(namespace) {
  let timer;
  try {
    const value = await Promise.race([
      namespace.get(SNAPSHOT_KEY, { type: "text", cacheTtl: 60 }),
      new Promise((_, reject) => { timer = setTimeout(() => reject(new Error("timeout")), 1200); }),
    ]);
    if (value === null) return null;
    if (typeof value !== "string" || new TextEncoder().encode(value).byteLength > MAX_BYTES) {
      throw new RequestError(503, "snapshot_invalid");
    }
    let snapshot;
    try { snapshot = JSON.parse(value); } catch { throw new RequestError(503, "snapshot_invalid"); }
    if (!validSnapshot(snapshot)) throw new RequestError(503, "snapshot_invalid");
    return snapshot;
  } catch (error) {
    throw error instanceof RequestError ? error : new RequestError(503, "kv_unavailable");
  } finally { clearTimeout(timer); }
}

function locate(snapshot, query) {
  if (query.kind === "alert") {
    return Object.values(snapshot?.profiles ?? {}).filter((entry) => entry.alerts && Object.hasOwn(entry.alerts, query.alertId))
      .sort((a, b) => b.fetchedAt - a.fetchedAt)[0];
  }
  const exact = snapshot?.profiles[query.id];
  if (exact) return exact;
  // AQI is independent of weather language/units. Firmware omits them here.
  if (["air", "history"].includes(query.kind)) {
    return Object.values(snapshot?.profiles ?? {}).filter((entry) =>
      entry.profile?.lat === query.profile.lat && entry.profile?.lon === query.profile.lon)
      .sort((a, b) => b.fetchedAt - a.fetchedAt)[0];
  }
  return null;
}

export function responsePayload(entry, query) {
  if (query.kind === "alert") return entry.alerts[query.alertId];
  if (query.kind === "current") return entry.current;
  if (query.kind === "air") return { ...entry.air, list: entry.air.list.slice(-1) };
  if (["hourly", "daily"].includes(query.kind)) {
    const source = entry[query.kind];
    const count = query.cnt ?? (query.kind === "hourly" ? 20 : 8);
    const first = source?.data[0]?.dt;
    if (query.start && query.start < first) throw new RequestError(503, "snapshot_range_unavailable");
    const data = source?.data.filter((item) => !query.start || item.dt >= query.start).slice(0, count);
    if (data?.length !== count) throw new RequestError(503, "snapshot_range_unavailable");
    return { ...source, data };
  }
  if (query.kind === "history") {
    const rolling = Math.abs(query.end - Date.now() / 1000) <= 900
      && [86399, 86400].includes(query.end - query.start);
    if (rolling) return entry.air;
    if (query.start < entry.airWindow.start || query.end > entry.airWindow.end) {
      throw new RequestError(503, "snapshot_range_unavailable");
    }
    const list = entry.air.list.filter((item) => item.dt >= query.start && item.dt <= query.end);
    if (!list.length) throw new RequestError(503, "snapshot_range_unavailable");
    return { ...entry.air, list };
  }
  throw new RequestError(404, "unsupported_weather_path");
}

export default {
  async fetch(request, env, ctx) {
    try {
      if (request.method !== "GET") return jsonError(405, "method_not_allowed", { Allow: "GET" });
      const url = new URL(request.url);
      if (url.pathname === "/health") return jsonResponse({ ok: true });
      const config = getConfig(env);
      const token = /^Bearer (.+)$/i.exec(request.headers.get("Authorization") ?? "")?.[1] ?? "";
      if (!tokenMatches(token, env.DEVICE_TOKEN)) return jsonError(401, "unauthorized", { "WWW-Authenticate": "Bearer" });
      const query = parseRequest(url);
      if (query.kind === "status") return await callCoordinator(env, "status", { profile: query.profile });
      const started = Date.now();
      const snapshot = await readSnapshot(env.WEATHER_CACHE);
      const timing = `kv;dur=${Date.now() - started}`;
      const entry = locate(snapshot, query);
      if (!entry) {
        if (query.kind === "alert") return jsonError(404, "snapshot_alert_not_found");
        return await callCoordinator(env, "register", { profile: query.profile });
      }
      if (!Number.isFinite(entry.fetchedAt) || entry.fetchedAt > Date.now()
          || !entry.profile || !entry.current || !entry.hourly || !entry.daily || !entry.air) {
        throw new RequestError(503, "snapshot_invalid");
      }
      if (query.kind !== "alert" && dayAt(entry.lastSeenAt ?? 0) !== dayAt(Date.now())) {
        ctx.waitUntil(callCoordinator(env, "touch", { profile: entry.profile }).catch(() => {
          console.warn("[weather] activity update failed");
        }));
      }
      const age = Math.floor((Date.now() - entry.fetchedAt) / 1000);
      if (age > config.staleSeconds) return jsonError(503, "snapshot_expired", {
        Age: String(age), "X-Weather-Fetched-At": new Date(entry.fetchedAt).toISOString(),
        "Server-Timing": timing,
      });
      const stale = age > config.freshSeconds;
      return jsonResponse(responsePayload(entry, query), 200, {
        Age: String(age), "X-Weather-Fetched-At": new Date(entry.fetchedAt).toISOString(),
        "X-Weather-Cache": stale ? "STALE" : "HIT", "X-Weather-Generation": String(snapshot.generation),
        "Server-Timing": timing, ...(stale ? { "X-Weather-Stale": "true" } : {}),
      });
    } catch (error) {
      return error instanceof RequestError ? jsonError(error.status, error.message)
        : jsonError(503, "relay_internal_error");
    }
  },

  async scheduled(controller, env) {
    getConfig(env);
    const response = await callCoordinator(env, "cron", { scheduledTime: controller.scheduledTime });
    if (!response.ok) throw new Error("weather_schedule_enqueue_failed");
    // Only enqueue; durable alarms own the work after this invocation ends.
  },
};

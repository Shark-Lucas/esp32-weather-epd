import assert from "node:assert/strict";
import { test } from "node:test";
import worker, { WeatherCoordinator } from "../src/index.js";
import { SNAPSHOT_KEY, MAX_BYTES, INACTIVE_MS, INTERVAL_MS, RequestError, profileId,
  parseRequest, emptySnapshot } from "../src/shared.js";
import { collectProfile, fetchWeather, projectPayload } from "../src/weather.js";
import { TOKEN, KEY, NOW, PROFILE, QUERY, fixture, MemoryKV, MemoryStorage } from "./fixtures.js";

function setup(t, upstream) {
  let now = NOW;
  const calls = [];
  t.mock.method(Date, "now", () => now);
  t.mock.method(globalThis, "fetch", async (url, options) => {
    calls.push({ url: new URL(url), options });
    return upstream ? upstream(url, options, calls.length) : new Response(JSON.stringify(fixture(url)));
  });
  t.mock.method(console, "log", () => {});
  t.mock.method(console, "warn", () => {});
  const env = { DEVICE_TOKEN: TOKEN, OPENWEATHER_API_KEY: KEY, WEATHER_CACHE: new MemoryKV(),
    WEATHER_COORDINATOR: {}, OWM_BUDGET_CONFIRMED: "true", OWM_BUDGET_UNIT: "request",
    OWM_DAILY_BUDGET: "10000", UPSTREAM_TIMEOUT_MS: "50" };
  const storage = new MemoryStorage();
  const state = { storage, blockConcurrencyWhile: (run) => run() };
  let object;
  const internal = [];
  env.WEATHER_COORDINATOR = { idFromName: (name) => name, get: () => ({ fetch(request) {
    internal.push(new URL(request.url).pathname); return object.fetch(request);
  } }) };
  object = new WeatherCoordinator(state, env);
  const pending = [];
  const ctx = { waitUntil: (promise) => pending.push(promise) };
  return { env, calls, storage, internal,
    get object() { return object; },
    advance(seconds) { now += seconds * 1000; },
    setTime(at) { now = at; },
    async restart() { object = new WeatherCoordinator(state, env); await object.ready; },
    async flush() { await Promise.all(pending.splice(0)); },
    request(path = QUERY, options = {}) {
      return worker.fetch(new Request(`https://weather.example.com${path}`, {
        headers: { Authorization: `Bearer ${TOKEN}` }, ...options,
      }), env, ctx);
    },
    cron(at = now) { return worker.scheduled({ scheduledTime: at }, env); },
    async alarm() { await object.alarm(); },
    async drain() {
      for (let i = 0; i < 30; i++) {
        await object.ready;
        if (!object.state.jobs.length) return;
        await object.alarm();
      }
      assert.fail("Job queue did not drain");
    },
  };
}

async function seeded(t, upstream) {
  const h = setup(t, upstream);
  assert.equal((await h.request()).headers.get("X-Weather-Error"), "snapshot_warming");
  await h.drain();
  assert.equal(h.env.WEATHER_CACHE.writes, 1);
  return h;
}

test("public health and unauthorized/invalid requests never read KV or start background calls", async (t) => {
  const h = setup(t); await h.object.ready;
  const reads = h.env.WEATHER_CACHE.reads;
  assert.deepEqual(await (await h.request("/health", { headers: {} })).json(), { ok: true });
  assert.equal((await h.request(QUERY, { headers: {} })).status, 401);
  assert.equal((await h.request(QUERY, { method: "POST" })).status, 405);
  for (const [path, code] of [
    ["/anything?url=https://evil.example", 404],
    ["/data/3.0/onecall?lat=31&lon=121", 410],
    [QUERY.replace("lat=31", "lat=91"), 400],
    [`${QUERY}&lat=32`, 400], [`${QUERY}&url=https://evil.example`, 400],
    ["/data/4.0/onecall/timeline/1h?lat=31&lon=121&cnt=21", 400],
    ["/data/2.5/air_pollution/history?lat=31&lon=121&start=2&end=1", 400],
  ]) assert.equal((await h.request(path)).status, code, path);
  assert.equal(h.env.WEATHER_CACHE.reads, reads);
  assert.equal(h.calls.length, 0); assert.equal(h.env.WEATHER_CACHE.writes, 0);
});

test("concurrent new-device requests coalesce into one profile, one complete fill and one KV write", async (t) => {
  const h = setup(t);
  const responses = await Promise.all(Array.from({ length: 30 }, () => h.request()));
  assert.ok(responses.every((r) => r.headers.get("X-Weather-Error") === "snapshot_warming"
    && r.headers.get("Retry-After") === "5"));
  assert.equal(Object.keys(h.object.state.profiles).length, 1);
  assert.equal(h.object.state.jobs.length, 1);
  assert.equal(h.calls.length, 0);
  await h.drain();
  assert.equal(h.calls.length, 5);
  assert.equal(h.env.WEATHER_CACHE.writes, 1);
  assert.deepEqual([...h.env.WEATHER_CACHE.values.keys()], [SNAPSHOT_KEY]);
  assert.equal(h.object.state.budget.used, 5);
});

test("any number of warm ordinary reads have zero OW calls, DO calls, or KV writes", async (t) => {
  const h = await seeded(t);
  const calls = h.calls.length, internal = h.internal.length, writes = h.env.WEATHER_CACHE.writes;
  for (let i = 0; i < 100; i++) {
    const r = await h.request();
    assert.equal(r.status, 200); assert.equal(r.headers.get("X-Weather-Cache"), "HIT");
    assert.equal(r.headers.get("Server-Timing"), "kv;dur=0");
    assert.match(r.headers.get("Cache-Control"), /no-store/);
  }
  assert.equal(h.calls.length, calls); assert.equal(h.internal.length, internal);
  assert.equal(h.env.WEATHER_CACHE.writes, writes);
});

test("several new locations share one publication; normalized profiles and caller keys are safe", async (t) => {
  const h = setup(t);
  await h.request(`${QUERY}&appid=device-supplied-secret`);
  await h.request(QUERY.replace("lat=31", "lat=32"));
  await h.request(QUERY.replace("lat=31", "lat=31.000"));
  assert.equal(h.object.state.jobs[0].ids.length, 2);
  await h.drain();
  assert.equal(h.calls.length, 10); assert.equal(h.env.WEATHER_CACHE.writes, 1);
  assert.ok(h.calls.every(({ url, options }) => url.searchParams.get("appid") === KEY
    && !new Headers(options.headers).has("Authorization") && options.redirect === "manual"));
  assert.doesNotMatch(h.env.WEATHER_CACHE.values.get(SNAPSHOT_KEY), /device-supplied-secret|local-test-openweather-secret/);
});

test("Cron is idempotent and performs one write for an entire multi-location batch", async (t) => {
  const h = setup(t); await h.request(); await h.request(QUERY.replace("lat=31", "lat=32")); await h.drain();
  h.advance(900); const at = Date.now();
  await Promise.all([h.cron(at), h.cron(at), h.cron(at - 1)]);
  assert.equal(h.object.state.jobs.length, 1);
  const writes = h.env.WEATHER_CACHE.writes;
  await h.alarm(); assert.equal(h.env.WEATHER_CACHE.writes, writes);
  await h.alarm(); assert.equal(h.env.WEATHER_CACHE.writes, writes + 1);
  await h.cron(at); await h.drain();
  assert.equal(h.env.WEATHER_CACHE.writes, writes + 1);
});

test("initial fill overlapping Cron does not refetch or publish an older generation", async (t) => {
  const h = setup(t); await h.request(); await h.cron();
  assert.equal(h.object.state.jobs.length, 2);
  await h.drain();
  assert.equal(h.calls.length, 5); assert.equal(h.env.WEATHER_CACHE.writes, 1);
  assert.equal(h.object.state.snapshot.generation, 1);
});

test("one failed required API preserves whole old location, while other locations refresh", async (t) => {
  let failing = false;
  const h = await seeded(t, async (url) => {
    const u = new URL(url);
    if (failing && u.searchParams.get("lat") === "31" && u.pathname.endsWith("/1day")) {
      return new Response("denied", { status: 403 });
    }
    return new Response(JSON.stringify(fixture(url)));
  });
  await h.request(QUERY.replace("lat=31", "lat=32")); await h.drain();
  const old = structuredClone(h.object.state.snapshot.profiles[profileId(PROFILE)]);
  failing = true; h.advance(900); await h.cron(); await h.drain();
  assert.deepEqual(h.object.state.snapshot.profiles[profileId(PROFILE)], old);
  assert.equal(h.object.state.profiles[profileId(PROFILE)].lastError.code, "upstream_http_403");
  const other = Object.values(h.object.state.snapshot.profiles).find((x) => x.profile.lat === "32");
  assert.equal(other.fetchedAt, Date.now());
  const r = await h.request(); assert.equal(r.status, 200); assert.equal(r.headers.get("Age"), "900");
  assert.equal(h.env.WEATHER_CACHE.writes, 3);
});

test("no successful locations means zero writes and errors remain visible through status", async (t) => {
  const h = setup(t, async () => new Response("denied", { status: 401 }));
  await h.request(); await h.drain();
  assert.equal(h.env.WEATHER_CACHE.writes, 0);
  const status = await (await h.request("/status?lat=31&lon=121")).json();
  assert.equal(status.lastError.code, "upstream_http_401");
  assert.equal(status.phase, "failed");
  const n = h.calls.length;
  await h.request(); assert.equal(h.calls.length, n);
});

test("unconfirmed budgets cannot register or fetch; capacity admission and daily reservation are enforced", async (t) => {
  const h = setup(t); h.env.OWM_BUDGET_CONFIRMED = "false";
  assert.equal((await h.request()).headers.get("X-Weather-Error"), "upstream_budget_not_configured");
  assert.equal(h.calls.length, 0); assert.equal(Object.keys(h.object.state.profiles).length, 0);
  h.env.OWM_BUDGET_CONFIRMED = "true"; h.env.OWM_DAILY_BUDGET = "1000";
  await h.request(); await h.request(QUERY.replace("lat=31", "lat=32"));
  assert.equal((await h.request(QUERY.replace("lat=31", "lat=33"))).headers.get("X-Weather-Error"), "upstream_budget_insufficient");
  await h.object.ready;
  h.object.state.budget = { day: "2026-11-06", unit: "request", used: 999 };
  await h.drain(); assert.equal(h.calls.length, 1); assert.equal(h.object.state.budget.used, 1000);
  assert.equal(h.env.WEATHER_CACHE.writes, 0);
  h.setTime(Date.parse("2026-11-07T00:00:01Z")); await h.object.reserve(24);
  assert.equal(h.object.state.budget.used, 1);
});

test("record-count budget charges requested records before each call and preserves counters across restart", async (t) => {
  const h = setup(t); h.env.OWM_BUDGET_UNIT = "record";
  await h.request(); await h.drain();
  assert.equal(h.object.state.budget.used, 57);
  await h.restart(); assert.equal(h.object.state.budget.used, 57);
  h.env.OWM_BUDGET_UNIT = "request";
  await assert.rejects(h.object.reserve(1), /upstream_budget_unit_changed/);
});

test("position capacity is ten, regardless of devices", async (t) => {
  const h = setup(t);
  for (let i = 1; i <= 10; i++) await h.request(QUERY.replace("lat=31", `lat=${i}`));
  assert.equal((await h.request(QUERY.replace("lat=31", "lat=11"))).headers.get("X-Weather-Error"), "location_capacity_reached");
  assert.equal(Object.keys(h.object.state.profiles).length, 10);
});

test("hourly pagination and daily projections preserve timestamp/count and reject uncovered requests", async (t) => {
  const h = await seeded(t);
  const first = await (await h.request("/data/4.0/onecall/timeline/1h?lat=31&lon=121&cnt=20")).json();
  assert.equal(first.data.length, 20);
  const next = first.data.at(-1).dt + 3600;
  const last = await (await h.request(`/data/4.0/onecall/timeline/1h?lat=31&lon=121&cnt=4&start=${next}`)).json();
  assert.equal(last.data.length, 4); assert.equal(last.data[0].dt, next);
  assert.equal((await h.request(`/data/4.0/onecall/timeline/1h?lat=31&lon=121&cnt=5&start=${next}`)).headers.get("X-Weather-Error"), "snapshot_range_unavailable");
  assert.equal((await (await h.request("/data/4.0/onecall/timeline/1day?lat=31&lon=121&cnt=8")).json()).data.length, 8);
  assert.equal(h.calls.length, 5);
});

test("rolling AQI reuses weather profile, including across hour/day boundaries; explicit history stays bounded", async (t) => {
  const h = setup(t); h.setTime(Date.parse("2026-11-06T23:59:00Z"));
  await h.request(QUERY.replace("lang=en", "lang=zh_cn")); await h.drain(); h.advance(120);
  const end = Math.floor(Date.now() / 1000);
  const url = `/data/2.5/air_pollution/history?lat=31&lon=121&start=${end - 86399}&end=${end}`;
  const r = await h.request(url); assert.equal(r.status, 200);
  assert.equal((await r.json()).list.length, 24);
  assert.equal(Object.keys(h.object.state.profiles).length, 1);
  assert.equal(h.calls.length, 5);
  assert.equal((await h.request(url.replace(String(end - 86399), String(end - 172798))))
    .headers.get("X-Weather-Error"), "snapshot_range_unavailable");
  const old = `/data/2.5/air_pollution/history?lat=31&lon=121&start=${end - 86400 * 7}&end=${end - 86400 * 6}`;
  assert.equal((await h.request(old)).headers.get("X-Weather-Error"), "snapshot_range_unavailable");
  await h.flush();
});

test("stale boundary is 15 minutes, expiry 3 hours, and ordinary stale reads never refresh", async (t) => {
  const h = await seeded(t); h.advance(901);
  const r = await h.request(); assert.equal(r.headers.get("X-Weather-Cache"), "STALE");
  assert.equal(r.headers.get("Age"), "901"); h.advance(9900);
  assert.equal((await h.request()).headers.get("X-Weather-Error"), "snapshot_expired");
  assert.equal(h.calls.length, 5); assert.equal(h.env.WEATHER_CACHE.writes, 1);
});

test("KV outage/invalid snapshot has an explicit error and never falls through to OW", async (t) => {
  const h = await seeded(t); h.env.WEATHER_CACHE.failReads = true;
  assert.equal((await h.request()).headers.get("X-Weather-Error"), "kv_unavailable");
  h.env.WEATHER_CACHE.failReads = false; h.env.WEATHER_CACHE.values.set(SNAPSHOT_KEY, "{");
  assert.equal((await h.request()).headers.get("X-Weather-Error"), "snapshot_invalid");
  assert.equal(h.calls.length, 5); assert.equal(h.env.WEATHER_CACHE.writes, 1);
});

test("KV propagation delay polls the existing registration rather than refetching", async (t) => {
  const h = await seeded(t); const saved = h.env.WEATHER_CACHE.values.get(SNAPSHOT_KEY);
  h.env.WEATHER_CACHE.values.delete(SNAPSHOT_KEY);
  for (let i = 0; i < 6; i++) assert.equal((await h.request()).headers.get("X-Weather-Error"), "snapshot_warming");
  assert.equal(h.object.state.jobs.length, 0); assert.equal(h.calls.length, 5);
  h.env.WEATHER_CACHE.values.set(SNAPSHOT_KEY, saved); assert.equal((await h.request()).status, 200);
});

test("seven-day idle profiles pause without deletion, then resume on access", async (t) => {
  const h = await seeded(t); h.advance(INACTIVE_MS / 1000 + 2);
  await h.cron(); await h.drain(); assert.equal(h.calls.length, 5);
  assert.equal(h.object.state.profiles[profileId(PROFILE)].phase, "paused");
  assert.equal((await h.request()).headers.get("X-Weather-Error"), "snapshot_expired");
  await h.flush(); await h.drain(); assert.equal(h.calls.length, 10);
  assert.equal((await h.request()).status, 200);
  assert.equal(Object.keys(h.object.state.profiles).length, 1);
});

test("retry policy alternates fixed hosts, charges attempts, and does not leak errors", async (t) => {
  const h = setup(t, async (url, options, count) => count < 3 ? new Response("bad", { status: 503 })
    : new Response(JSON.stringify(fixture(url))));
  const waits = [], charges = [];
  await fetchWeather(h.env, PROFILE, "/data/4.0/onecall/current", {}, "current", 1,
    async (count) => charges.push(count), async (ms) => waits.push(ms));
  assert.deepEqual(waits, [500, 1500]); assert.equal(charges.length, 3);
  assert.deepEqual(h.calls.map((x) => x.url.hostname), ["api.openweathermap.org", "cn-api.openweathermap.org", "api.openweathermap.org"]);
});

test("429 creates durable cooldown and no immediate retry; 401 is terminal", async (t) => {
  const h = setup(t, async () => new Response(KEY, { status: 429, headers: { "Retry-After": "120" } }));
  await h.request(); await h.drain();
  assert.ok(h.object.state.cooldownUntil >= Date.now() + 120000);
  const n = h.calls.length; h.advance(60); await h.cron(); await h.drain();
  assert.equal(h.calls.length, n);
  assert.doesNotMatch(JSON.stringify(h.object.status(profileId(PROFILE))), new RegExp(KEY));
});

test("deadline covers hanging bodies, and malformed/oversized payloads cannot be published", async (t) => {
  const h = setup(t, async (_url, options) => new Response(new ReadableStream({
    start(controller) {
      controller.enqueue(new TextEncoder().encode('{"data":'));
      options.signal.addEventListener("abort", () => controller.error(new Error(KEY)), { once: true });
    },
  })));
  await assert.rejects(fetchWeather(h.env, PROFILE, "/data/4.0/onecall/current", {}, "current", 1,
    async () => {}, async () => {}), /upstream_timeout/);
  assert.equal(h.calls.length, 3); assert.ok(h.calls.every((x) => x.options.signal.aborted));
  for (const body of ["x".repeat(MAX_BYTES + 1), '{"data":[]}', '{"cod":401}']) {
    t.mock.method(globalThis, "fetch", async () => new Response(body));
    await assert.rejects(fetchWeather(h.env, PROFILE, "/data/4.0/onecall/current", {}, "current", 1,
      async () => {}, async () => {}), /upstream_invalid_payload/);
  }
});

test("projection removes only unconsumed fields without rounding and cuts actual payload bytes", async (t) => {
  const h = setup(t);
  const entry = await collectProfile(h.env, PROFILE, async () => {}, async () => {});
  assert.equal(entry.current.data[0].temp, 291.14);
  assert.equal(entry.hourly.data[0].pop, 0.2525);
  assert.equal(entry.daily.data[0].temp.max, 295.9);
  assert.equal(entry.air.list[0].components.pm2_5, 12.1234);
  assert.equal(entry.hourly.data[0].weather, undefined);
  assert.equal(entry.current.data[0].rain.unused, undefined);
  const raw = fixture(h.calls[0].url);
  const projected = projectPayload(raw, "current", 1);
  assert.ok(JSON.stringify(projected).length < JSON.stringify(raw).length * 0.75);
});

test("alerts are fetched once, projected without descriptions, and served without KV writes", async (t) => {
  const h = await seeded(t, async (url) => new Response(JSON.stringify(fixture(url, { alerts: true }))));
  assert.equal(h.calls.filter((x) => x.url.pathname.includes("/alert/")).length, 1);
  const response = await h.request("/data/4.0/onecall/alert/wind-1");
  assert.equal(response.status, 200); const alert = await response.json();
  assert.equal(alert.event, "Wind"); assert.equal(alert.description, undefined);
  assert.equal(h.env.WEATHER_CACHE.writes, 1);
  assert.equal((await h.request("/data/4.0/onecall/alert/__proto__")).status, 404);
});

test("oversized aggregate preserves prior snapshot and refuses publication", async (t) => {
  const h = await seeded(t); const before = h.env.WEATHER_CACHE.values.get(SNAPSHOT_KEY);
  h.advance(900); await h.cron();
  // A publication bound, independent of per-upstream body limits.
  const job = h.object.state.jobs[0]; job.cursor = job.ids.length;
  job.updates[profileId(PROFILE)] = { ...h.object.state.snapshot.profiles[profileId(PROFILE)],
    extra: "x".repeat(MAX_BYTES) };
  await h.drain();
  assert.equal(h.env.WEATHER_CACHE.values.get(SNAPSHOT_KEY), before);
  assert.equal(h.object.state.lastPublishError, "snapshot_too_large");
  assert.equal(h.env.WEATHER_CACHE.writes, 1);
});

test("publication retries reuse generation after restart and never overwrite a newer batch", async (t) => {
  const h = await seeded(t); h.advance(900); await h.cron(); h.env.WEATHER_CACHE.failWrites = true;
  await h.alarm(); assert.equal(h.object.state.lastPublishError, "snapshot_publish_failed");
  const generation = h.object.state.jobs[0].publication.generation;
  await h.restart(); h.env.WEATHER_CACHE.failWrites = false; await h.drain();
  assert.equal(h.object.state.snapshot.generation, generation);
  assert.equal(h.env.WEATHER_CACHE.writes, 2);
});

test("registration during upstream IO is durable and concurrent alarms share one executor", async (t) => {
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  const h = setup(t, async (url) => { await gate; return new Response(JSON.stringify(fixture(url))); });
  await h.request();
  const running = h.object.alarm();
  const duplicate = h.object.alarm();
  assert.equal(running, duplicate);
  const response = await h.request(QUERY.replace("lat=31", "lat=32"));
  assert.equal(response.headers.get("X-Weather-Error"), "snapshot_warming");
  assert.equal(Object.keys(h.object.state.profiles).length, 2);
  release(); await running; await h.drain();
  assert.equal(h.calls.length, 10);
  assert.equal(Object.keys(h.object.state.snapshot.profiles).length, 2);
});

test("idle reactivation observes admission budget and status does not register or fetch", async (t) => {
  const h = await seeded(t);
  const before = h.storage.writes;
  const status = await (await h.request("/status?lat=40&lon=121")).json();
  assert.equal(status.registered, false); assert.equal(h.storage.writes, before);
  h.advance(INACTIVE_MS / 1000 + 1); h.env.OWM_DAILY_BUDGET = "1";
  await h.request(); await h.flush();
  assert.equal(h.calls.length, 5);
  assert.ok(Date.now() - h.object.state.profiles[profileId(PROFILE)].lastSeenAt > INACTIVE_MS);
});

test("location completion survives eviction mid-batch and all profile fields are validated", async (t) => {
  const h = setup(t); await h.request(); await h.request(QUERY.replace("lat=31", "lat=32"));
  await h.alarm(); assert.equal(h.env.WEATHER_CACHE.writes, 0);
  await h.restart(); await h.drain();
  assert.equal(h.calls.length, 10); assert.equal(h.env.WEATHER_CACHE.writes, 1);
  const bad = new Request("https://internal/register", { method: "POST", body: JSON.stringify({ profile: { lat: "91" } }) });
  assert.equal((await h.object.fetch(bad)).status, 400);
});

test("budget reset follows configured UTC hour and does not reset on local date change", async (t) => {
  const h = setup(t); h.env.OWM_BUDGET_RESET_UTC_HOUR = "6";
  h.setTime(Date.parse("2026-11-06T23:59:59Z")); await h.object.reserve(1);
  h.advance(2); await h.object.reserve(1); assert.equal(h.object.state.budget.used, 2);
  h.setTime(Date.parse("2026-11-07T06:00:00Z")); await h.object.reserve(1);
  assert.equal(h.object.state.budget.used, 1);
  assert.equal(h.object.budgetStatus().resetAt, "2026-11-08T06:00:00.000Z");
});

test("429 cooldown is visible before other failed parallel APIs try again", async (t) => {
  let otherAttempts = 0;
  const h = setup(t, async (url) => {
    if (new URL(url).pathname.endsWith("/current")) {
      return new Response("limited", { status: 429, headers: { "Retry-After": "180" } });
    }
    otherAttempts++;
    return new Response("temporary", { status: 503 });
  });
  await h.request(); await h.drain();
  assert.ok(otherAttempts <= 3); // Only already-running independent calls.
  assert.ok(h.object.state.cooldownUntil >= Date.now() + 180000);
  assert.equal(h.env.WEATHER_CACHE.writes, 0);
});

test("explicit quota opt-out permits cold fill without configuration or quota writes", async (t) => {
  const h = setup(t);
  h.env.OWM_BUDGET_ENFORCED = "false";
  h.env.OWM_BUDGET_CONFIRMED = "false";
  h.env.OWM_DAILY_BUDGET = "0";
  assert.equal((await h.request()).headers.get("X-Weather-Error"), "snapshot_warming");
  await h.drain();
  assert.equal(h.calls.length, 5);
  assert.equal(h.env.WEATHER_CACHE.writes, 1);
  assert.equal(h.storage.values.has("budget:v2"), false);
  assert.deepEqual(h.object.budgetStatus(), { configured: true, enforced: false });
});

test("quota opt-out still respects provider cooldown and requires an upstream key", async (t) => {
  const h = setup(t);
  h.env.OWM_BUDGET_ENFORCED = "false";
  await h.object.ready;
  h.object.state.cooldownUntil = Date.now() + 120000;
  await assert.rejects(h.object.reserve(1), /upstream_http_429/);
  h.object.state.cooldownUntil = 0;
  delete h.env.OPENWEATHER_API_KEY;
  assert.equal((await h.request()).headers.get("X-Weather-Error"), "worker_not_configured");
  assert.equal(h.calls.length, 0);
});

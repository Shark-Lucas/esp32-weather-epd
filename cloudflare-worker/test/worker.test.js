import assert from "node:assert/strict";
import { test } from "node:test";
import worker from "../src/index.js";

const TOKEN = "local-test-device-token-0123456789abcdef";
const ORIGIN_KEY = "local-test-openweather-secret";
const CURRENT = '{"lat":31,"lon":121,"timezone":"Asia/Shanghai","timezone_offset":28800,"data":[{"dt":1793318400,"temp":293.15,"humidity":60,"weather":[{"id":800,"main":"Clear","description":"晴","icon":"01d"}]}]}';
const QUERY = "/data/4.0/onecall/current?lat=31&lon=121&lang=zh_cn&units=standard";

class MemoryKV {
  values = new Map();
  reads = 0;
  async get(key) {
    this.reads++;
    const value = this.values.get(key);
    return value === undefined ? null : JSON.parse(value);
  }
  async put(key, value) { this.values.set(key, value); }
}

function setup(t, upstream = async () => new Response(CURRENT)) {
  let now = Date.parse("2026-10-30T00:00:00Z");
  const calls = [];
  t.mock.method(Date, "now", () => now);
  t.mock.method(globalThis, "fetch", async (url, options) => {
    calls.push({ url: new URL(url), options });
    return upstream(url, options);
  });
  const env = {
    OPENWEATHER_API_KEY: ORIGIN_KEY,
    DEVICE_TOKEN: TOKEN,
    WEATHER_CACHE: new MemoryKV(),
    UPSTREAM_TIMEOUT_MS: "100",
  };
  const pending = [];
  const ctx = { waitUntil(promise) { pending.push(promise); } };
  return {
    env, calls,
    advance(seconds) { now += seconds * 1000; },
    async flush() { await Promise.all(pending.splice(0)); },
    request(path = QUERY, options = {}) {
      return worker.fetch(new Request(`https://weather.example.com${path}`, {
        headers: { Authorization: `Bearer ${TOKEN}` }, ...options,
      }), env, ctx);
    },
  };
}

test("health is public and never contacts OpenWeather", async (t) => {
  const h = setup(t);
  const response = await h.request("/health", { headers: {} });
  assert.equal(response.status, 200);
  assert.equal((await response.json()).ok, true);
  assert.equal(h.calls.length, 0);
});

test("unauthenticated requests cannot contact upstream or read KV", async (t) => {
  const h = setup(t);
  const response = await h.request(QUERY, { headers: {} });
  assert.equal(response.status, 401);
  assert.equal(h.calls.length, 0);
  assert.equal(h.env.WEATHER_CACHE.reads, 0);
});

test("POST requests cannot trigger billable upstream calls", async (t) => {
  const h = setup(t);
  assert.equal((await h.request(QUERY, { method: "POST" })).status, 405);
  assert.equal(h.calls.length, 0);
});

test("missing credentials or KV fail closed", async (t) => {
  const h = setup(t);
  delete h.env.OPENWEATHER_API_KEY;
  assert.equal((await h.request()).status, 503);
  assert.equal(h.calls.length, 0);
});

test("a 32-character non-hexadecimal token authenticates and rejects a different token", async (t) => {
  const h = setup(t);
  h.env.DEVICE_TOKEN = "0123456789ghijklmnopqrstuvwxyzAB";
  const response = await h.request(QUERY, {
    headers: { Authorization: "Bearer 0123456789ghijklmnopqrstuvwxyzAB" },
  });
  assert.equal(response.status, 200);
  assert.equal(await response.text(), CURRENT);
  await h.flush();
  const reads = h.env.WEATHER_CACHE.reads;
  const denied = await h.request(QUERY, {
    headers: { Authorization: "Bearer 0123456789ghijklmnopqrstuvwxyzAC" },
  });
  assert.equal(denied.status, 401);
  assert.equal(h.env.WEATHER_CACHE.reads, reads);
  assert.equal(h.calls.length, 1);
});

test("short or oversized configured device tokens fail closed", async (t) => {
  const h = setup(t);
  for (const token of ["", "0123456789abcde", "0123456789abcdef", "a".repeat(31), "a".repeat(257)]) {
    h.env.DEVICE_TOKEN = token;
    assert.equal((await h.request(QUERY, {
      headers: { Authorization: `Bearer ${token}` },
    })).status, 503);
  }
  assert.equal(h.calls.length, 0);
  assert.equal(h.env.WEATHER_CACHE.reads, 0);
});

test("only allowlisted paths and valid bounded queries reach upstream", async (t) => {
  const h = setup(t);
  for (const [path, status] of [
    ["/anything?url=https://evil.example", 404],
    ["/data/4.0/onecall/current?lat=91&lon=121", 400],
    ["/data/4.0/onecall/current?lat=31&lat=32&lon=121", 400],
    ["/data/4.0/onecall/current?lat=31&lon=121&url=https://evil.example", 400],
    ["/data/4.0/onecall/timeline/1h?lat=31&lon=121&cnt=21", 400],
    ["/data/2.5/air_pollution/history?lat=31&lon=121&start=100&end=99", 400],
    ["/data/4.0/onecall/current?lat=&lon=121", 400],
  ]) {
    assert.equal((await h.request(path)).status, status, path);
  }
  assert.equal(h.calls.length, 0);
});

test("relay keeps JSON intact and substitutes only the server key", async (t) => {
  const h = setup(t);
  const response = await h.request(`${QUERY}&appid=untrusted-device-key`);
  assert.equal(response.status, 200);
  assert.equal(await response.text(), CURRENT);
  assert.equal(response.headers.get("X-Weather-Cache"), "MISS");
  assert.match(response.headers.get("Cache-Control"), /no-store/);
  assert.equal(response.headers.get("X-Weather-Fetched-At"), "2026-10-30T00:00:00.000Z");
  assert.equal(h.calls[0].url.origin, "https://api.openweathermap.org");
  assert.equal(h.calls[0].url.searchParams.get("appid"), ORIGIN_KEY);
  assert.equal(new Headers(h.calls[0].options.headers).has("Authorization"), false);
  assert.equal(h.calls[0].options.redirect, "manual");
  await h.flush();
  assert.doesNotMatch(JSON.stringify([...h.env.WEATHER_CACHE.values]), new RegExp(ORIGIN_KEY));
});

test("fresh cache is reusable across reordered queries but still requires auth", async (t) => {
  const h = setup(t);
  await h.request();
  await h.flush();
  h.advance(300);
  const response = await h.request("/data/4.0/onecall/current?units=standard&lon=121&lang=zh_cn&lat=31.000");
  assert.equal(response.headers.get("X-Weather-Cache"), "HIT");
  assert.equal(response.headers.get("Age"), "300");
  assert.equal(await response.text(), CURRENT);
  assert.equal(h.calls.length, 1);
  const reads = h.env.WEATHER_CACHE.reads;
  assert.equal((await h.request(QUERY, { headers: { Authorization: "Bearer wrong" } })).status, 401);
  assert.equal(h.env.WEATHER_CACHE.reads, reads);
});

test("temporary errors serve the last successful payload without overwriting it", async (t) => {
  let failing = false;
  const h = setup(t, async () => failing ? new Response("unavailable", { status: 503 }) : new Response(CURRENT));
  await h.request();
  await h.flush();
  h.advance(601);
  failing = true;
  const response = await h.request();
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("X-Weather-Cache"), "STALE");
  assert.equal(response.headers.get("Age"), "601");
  assert.equal(await response.text(), CURRENT);
  assert.equal(h.calls.length, 3);
  assert.equal(h.calls[2].url.hostname, "cn-api.openweathermap.org");
  await h.flush();
  h.advance(10200);
  assert.equal((await h.request()).status, 502);
});

test("authentication errors remain visible even with a stale cache", async (t) => {
  let status = 200;
  const h = setup(t, async () => new Response(status === 200 ? CURRENT : `secret=${ORIGIN_KEY}`, { status }));
  await h.request();
  await h.flush();
  h.advance(601);
  status = 401;
  const response = await h.request();
  assert.equal(response.status, 401);
  assert.doesNotMatch(await response.text(), new RegExp(ORIGIN_KEY));
  assert.equal(h.calls.length, 2);
});

test("429 may use stale data without immediately repeating a billed request", async (t) => {
  let status = 200;
  const h = setup(t, async () => new Response(status === 200 ? CURRENT : "quota", { status }));
  await h.request();
  await h.flush();
  h.advance(601);
  status = 429;
  assert.equal((await h.request()).headers.get("X-Weather-Cache"), "STALE");
  assert.equal(h.calls.length, 2);
});

test("transient failure retries the alternate fixed host", async (t) => {
  const h = setup(t, async (url) => new URL(url).hostname === "api.openweathermap.org"
    ? new Response("bad gateway", { status: 502 }) : new Response(CURRENT));
  assert.equal(await (await h.request()).text(), CURRENT);
  assert.equal(h.calls.length, 2);
});

test("invalid JSON and wrong success schemas cannot poison the cache", async (t) => {
  for (const body of ['{"data":', '{"cod":401,"message":"bad key"}', '{"data":[]}']) {
    const h = setup(t, async () => new Response(body));
    const response = await h.request();
    assert.equal(response.status, 502);
    await h.flush();
    assert.equal(h.env.WEATHER_CACHE.values.size, 0);
  }
});

test("deadline covers hanging response bodies, not just response headers", async (t) => {
  let attempts = 0;
  const h = setup(t, async (_url, options) => {
    attempts++;
    if (attempts === 2) return new Response(CURRENT);
    return new Response(new ReadableStream({
      start(controller) {
        controller.enqueue(new TextEncoder().encode('{"data":'));
        options.signal.addEventListener("abort", () => controller.error(new Error("aborted")), { once: true });
      },
    }));
  });
  assert.equal(await (await h.request()).text(), CURRENT);
  assert.equal(attempts, 2);
});

test("oversized bodies are rejected before caching", async (t) => {
  const h = setup(t, async () => new Response("x".repeat(524289)));
  assert.equal((await h.request()).status, 502);
  await h.flush();
  assert.equal(h.env.WEATHER_CACHE.values.size, 0);
});

test("a KV outage does not discard a successful upstream response", async (t) => {
  t.mock.method(console, "warn", () => {}); // This test deliberately makes KV writes fail.
  const h = setup(t);
  h.env.WEATHER_CACHE = {
    async get() { throw new Error("KV unavailable"); },
    async put() { throw new Error("KV unavailable"); },
  };
  assert.equal(await (await h.request()).text(), CURRENT);
  await h.flush();
});

test("cache keys isolate coordinates, locale and hourly pagination", async (t) => {
  const h = setup(t);
  for (const path of [QUERY, QUERY.replace("lat=31", "lat=32"), QUERY.replace("zh_cn", "en"),
    "/data/4.0/onecall/timeline/1h?lat=31&lon=121&cnt=20",
    "/data/4.0/onecall/timeline/1h?lat=31&lon=121&cnt=20&start=1793390400"]) {
    assert.equal((await h.request(path)).headers.get("X-Weather-Cache"), "MISS");
    await h.flush();
  }
  assert.equal(h.calls.length, 5);
  assert.equal(h.env.WEATHER_CACHE.values.size, 5);
});

test("moving air-quality windows reuse last-good data across hour boundaries", async (t) => {
  let failing = false;
  const air = '{"coord":{"lat":31,"lon":121},"list":[{"dt":1793318400,"main":{"aqi":2},"components":{"pm2_5":15,"pm10":25,"co":200,"no":0,"no2":5,"o3":20,"so2":2,"nh3":1}}]}';
  const h = setup(t, async () => failing ? new Response("down", { status: 503 }) : new Response(air));
  const end = Date.now() / 1000;
  const history = (at) => `/data/2.5/air_pollution/history?lat=31&lon=121&start=${at - 86399}&end=${at}`;
  await h.request(history(end));
  await h.flush();
  h.advance(120);
  assert.equal((await h.request(history(end + 120))).headers.get("X-Weather-Cache"), "HIT");
  h.advance(3600);
  failing = true;
  const stale = await h.request(history(end + 3720));
  assert.equal(stale.headers.get("X-Weather-Cache"), "STALE");
  assert.equal(await stale.text(), air);
  // Explicit historical windows must never accidentally return today's cache.
  assert.equal((await h.request(history(end - 86400 * 7))).status, 502);
});

test("One Call 3.0, daily timelines and alert payloads retain their own schemas", async (t) => {
  const cases = [
    ["/data/3.0/onecall?lat=31&lon=121&exclude=minutely,alerts", '{"current":{"dt":1793318400,"temp":293.15},"hourly":[],"daily":[]}'],
    ["/data/4.0/onecall/timeline/1day?lat=31&lon=121&cnt=7", CURRENT],
    ["/data/4.0/onecall/alert/abc-123", '{"sender_name":"Weather","event":"Wind","start":1793318400,"end":1793322000}'],
  ];
  for (const [path, body] of cases) {
    const h = setup(t, async () => new Response(body));
    assert.equal(await (await h.request(path)).text(), body);
  }
});

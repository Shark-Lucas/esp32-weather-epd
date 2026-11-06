import assert from "node:assert/strict";
import { test } from "node:test";
import { Miniflare, convertV4MiniflareOptions } from "miniflare";
import { fixture, QUERY, TOKEN, KEY } from "./fixtures.js";

test("workerd SQLite DO, real alarm and KV serve a complete snapshot after cold registration", { timeout: 30000 }, async () => {
  const calls = [];
  const runtime = new Miniflare(convertV4MiniflareOptions({
    name: "weather-test",
    modules: true, scriptPath: new URL("../dist/index.js", import.meta.url).pathname.replace(/^\/([A-Z]:)/, "$1"),
    compatibilityDate: "2026-10-30",
    kvNamespaces: ["WEATHER_CACHE"],
    durableObjects: { WEATHER_COORDINATOR: { className: "WeatherCoordinator", useSQLite: true } },
    bindings: { DEVICE_TOKEN: TOKEN, OPENWEATHER_API_KEY: KEY,
      OWM_BUDGET_ENFORCED: "false" },
    outboundService: async (request) => {
      const url = new URL(request.url); calls.push(url.pathname);
      return new Response(JSON.stringify(fixture(url)));
    },
  }));
  try {
    const request = (path) => runtime.dispatchFetch(`https://weather.example.com${path}`, {
      headers: { Authorization: `Bearer ${TOKEN}` },
    });
    const warming = await request(QUERY);
    assert.equal(warming.headers.get("X-Weather-Error"), "snapshot_warming");
    let response;
    for (let i = 0; i < 60; i++) {
      await new Promise((resolve) => setTimeout(resolve, 100));
      response = await request(QUERY);
      if (response.status === 200) break;
    }
    assert.equal(response.status, 200, await response.clone().text());
    assert.equal((await response.json()).data[0].temp, 291.14);
    assert.equal(calls.length, 5);
    const status = await (await request("/status?lat=31&lon=121")).json();
    assert.equal(status.phase, "ready"); assert.equal(status.budget.enforced, false);
    const kv = await runtime.getKVNamespace("WEATHER_CACHE");
    const keys = await kv.list(); assert.deepEqual(keys.keys.map((key) => key.name), ["weather:snapshot:v2"]);
    const snapshot = await kv.get("weather:snapshot:v2", "json");
    assert.equal(snapshot.generation, 1);
    assert.equal(Object.values(snapshot.profiles)[0].hourly.data.length, 24);
    for (let i = 0; i < 10; i++) assert.equal((await request(QUERY)).status, 200);
    assert.equal(calls.length, 5);
    // Exercise >128 KiB of persisted coordinator state and the whole ten-profile
    // batch through real SQLite storage, not only JavaScript memory doubles.
    await Promise.all(Array.from({ length: 9 }, (_, i) => request(QUERY.replace("lat=31", `lat=${32 + i}`))));
    let combined;
    for (let i = 0; i < 160; i++) {
      await new Promise((resolve) => setTimeout(resolve, 100));
      combined = await kv.get("weather:snapshot:v2", "json");
      if (Object.keys(combined?.profiles ?? {}).length === 10) break;
    }
    assert.equal(Object.keys(combined.profiles).length, 10);
    assert.equal(combined.generation, 2); assert.equal(calls.length, 50);
  } finally { await runtime.dispose(); }
});

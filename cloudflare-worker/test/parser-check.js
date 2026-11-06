import assert from "node:assert/strict";
import { mkdtemp, writeFile, rm, readdir } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve, sep } from "node:path";
import { spawnSync } from "node:child_process";
import { projectPayload } from "../src/weather.js";
import { fixture } from "./fixtures.js";

const root = resolve("..");
const temp = await mkdtemp(join(tmpdir(), "weather-parser-"));
try {
  let rawBytes = 0, smallBytes = 0;
  const hour = Math.floor(Date.now() / 3600000) * 3600;
  const cases = [
    ["current", "/data/4.0/onecall/current", "current", 1],
    ["hourly1", "/data/4.0/onecall/timeline/1h?cnt=20", "hourly", 20],
    ["hourly2", `/data/4.0/onecall/timeline/1h?cnt=4&start=${hour + 72000}`, "hourly", 4],
    ["daily", "/data/4.0/onecall/timeline/1day?cnt=8", "daily", 8],
    ["alert", "/data/4.0/onecall/alert/wind-1", "alert", 1],
    ["air", `/data/2.5/air_pollution/history?end=${Math.floor(Date.now() / 1000)}`, "air", 24],
  ];
  for (const [name, path, kind, count] of cases) {
    const raw = fixture(`https://api.example${path}`, { alerts: true });
    const small = projectPayload(raw, kind, count);
    const original = JSON.stringify(raw), projected = JSON.stringify(small);
    rawBytes += Buffer.byteLength(original); smallBytes += Buffer.byteLength(projected);
    await writeFile(join(temp, `raw-${name}.json`), original);
    await writeFile(join(temp, `small-${name}.json`), projected);
  }
  const deps = join(root, "platformio/.pio/libdeps/dfrobot_firebeetle2_esp32e");
  assert.ok((await readdir(deps)).includes("ArduinoJson"), "Run platformio run to install ArduinoJson first");
  const exe = join(temp, process.platform === "win32" ? "parser.exe" : "parser");
  const compile = spawnSync(process.env.CXX ?? "g++", ["-std=c++17", "-O0",
    "-Itest/parser-host", `-I${root}/platformio/include`, `-I${deps}/ArduinoJson/src`,
    "test/parser-host/check.cpp", `${root}/platformio/src/api/openweather_v4.cpp`,
    `${root}/platformio/src/api_response.cpp`, "-o", exe], { encoding: "utf8" });
  assert.equal(compile.status, 0, compile.stderr ?? String(compile.error));
  const check = spawnSync(exe, [join(temp, "raw"), join(temp, "small")], { encoding: "utf8" });
  assert.equal(check.status, 0, check.stderr);
  console.log(`PASS: real firmware 4.0 and AQI parsers produce identical consumed fields; synthetic fixture payload ${rawBytes} -> ${smallBytes} bytes (${Math.round(100 * (1-smallBytes/rawBytes))}% smaller).`);
} finally {
  const target = resolve(temp), parent = resolve(tmpdir());
  assert.ok(target.startsWith(parent + sep) && target.slice(parent.length + 1).startsWith("weather-parser-"));
  await rm(target, { recursive: true, force: true });
}

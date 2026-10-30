# OpenWeather Worker Implementation Plan

> **For agentic workers:** Implement inline, task by task, following
> superpowers:executing-plans and superpowers:test-driven-development. The user
> authorized writing the Worker after reviewing the relay design in chat.

**Goal:** Deliver a deployable authenticated OpenWeather relay with KV fallback.

**Architecture:** A module Worker accepts the firmware's existing GET paths,
validates queries, attaches the server-side API key, and fetches fixed HTTPS
upstreams. KV stores the last successful payload per logical query.

**Tech Stack:** JavaScript modules, Node >=22, node:test, Wrangler 4, Workers KV.

**Spec:** `docs/superpowers/specs/2026-10-30-openweather-worker-design.md`.

## Global Constraints

- Keep One Call 3.0 and 4.0 response formats separate and unchanged.
- No real credentials, firmware edits, or cloud deployment.
- Default fresh lifetime 600 seconds and maximum stale age 10,800 seconds.
- Fixed HTTPS upstreams only; require device authentication for cached responses.

### Task 1: Request contract and relay

Files: `cloudflare-worker/src/index.js`, `cloudflare-worker/test/worker.test.js`,
`cloudflare-worker/package.json`.

- [x] Write behavioral tests for the authenticated route/query contract and raw
  successful payloads. Run `node --test` against a 501 interface stub and confirm
  assertions fail.
- [x] Implement route validation, private responses, credential replacement,
  bounded body reads, schema checks and a single transient retry.
- [x] Run `npm.cmd test`; inspect failure output and correct implementation.

### Task 2: Cache behavior

Files: the same handler and test file.

- [x] Test fresh reuse, authentication before cache reads, stale fallback,
  absolute expiration, pagination isolation and moving air-quality windows.
- [x] Implement KV envelopes and normalized logical keys. Surface HIT/MISS/STALE
  and fetch timestamp headers without changing OpenWeather JSON.
- [x] Run `npm.cmd test` and confirm all assertions pass.

### Task 3: Deployment package and verification

Files: `cloudflare-worker/wrangler.jsonc`, `.dev.vars.example`, `.gitignore`,
`README.md`, lockfile; repository README points to these instructions.

- [x] Pin Wrangler, declare a provisionable KV binding, and ignore local secrets,
  generated bundles and dependency caches.
- [x] Document browser login deployment, creation of Secrets, custom-domain
  setup, curl verification and the later firmware connection changes.
- [x] Run `npm.cmd test`, `npm.cmd run build`, and a local workerd health/auth
  smoke test. Review `git diff --check` and final file status.

Cloud deployment requires a logged-in Cloudflare account and locally entered
Secrets. It is intentionally outside this implementation step.

Verification completed: 18 Node tests passed; Wrangler dry-run produced an
11.88 KiB bundle (3.98 KiB gzip). Local workerd returned 200/401/400/405 for the
health/auth/input/method cases. A locally seeded KV entry returned HIT with the
original JSON and an accurate Content-Length, without contacting OpenWeather.

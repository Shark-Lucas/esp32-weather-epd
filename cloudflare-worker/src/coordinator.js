import { SNAPSHOT_KEY, INTERVAL_MS, INACTIVE_MS, MAX_BYTES, MAX_PROFILES, RequestError,
  budgetConfig, budgetDay, dayAt, emptySnapshot, getConfig, jsonError, jsonResponse, parseRequest,
  profileId, validSnapshot } from "./shared.js";
import { collectProfile } from "./weather.js";

const STATE_KEY = "coordinator:v2";
const COALESCE_MS = 2000;

export class WeatherCoordinator {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
    this.serial = Promise.resolve();
    this.running = null;
    this.ready = ctx.blockConcurrencyWhile(async () => {
      this.state = await ctx.storage.get(STATE_KEY);
      if (!this.state) {
        let snapshot = emptySnapshot();
        // Import v2 on first deployment/recovery; leave old per-API keys alone.
        const saved = await env.WEATHER_CACHE.get(SNAPSHOT_KEY, { type: "json" });
        if (saved !== null) {
          if (!validSnapshot(saved)) throw new Error("invalid_saved_snapshot");
          snapshot = saved;
        }
        this.state = { snapshot, profiles: {}, jobs: [], lastCronSlot: -1,
          nextJob: 1, cooldownUntil: 0, budget: null, lastPublishError: null };
        for (const [id, entry] of Object.entries(snapshot.profiles)) {
          this.state.profiles[id] = { profile: entry.profile, lastSeenAt: entry.lastSeenAt ?? entry.fetchedAt,
            lastAttemptAt: 0, lastError: null, phase: "ready" };
        }
        await this.save();
      }
      // Quota reservations are frequent, tiny writes. Do not rewrite the
      // complete snapshot/staged data for every upstream attempt.
      const budget = await ctx.storage.get("budget:v2");
      if (budget) this.state.budget = budget;
    });
  }

  // Only short storage operations hold the lock, never upstream requests.
  exclusive(run) {
    const next = this.serial.then(async () => { await this.ready; return run(); });
    this.serial = next.catch(() => {});
    return next;
  }

  save() { return this.ctx.storage.put(STATE_KEY, this.state); }

  async arm(at) {
    const current = await this.ctx.storage.getAlarm();
    if (current === null || current > at) await this.ctx.storage.setAlarm(at);
  }

  knownProfile(value) {
    if (!value || typeof value !== "object") throw new RequestError(400, "invalid_profile");
    const url = new URL("https://internal/status");
    for (const key of ["lat", "lon", "lang", "units"]) {
      if (typeof value[key] !== "string") throw new RequestError(400, "invalid_profile");
      url.searchParams.set(key, value[key]);
    }
    return parseRequest(url);
  }

  budgetStatus() {
    let config;
    try { config = budgetConfig(this.env); } catch (error) { return { configured: false, error: error.message }; }
    if (!config.enforced) return { configured: true, enforced: false };
    const saved = this.state.budget;
    const today = budgetDay(Date.now(), config.resetHour);
    const used = saved?.day === today ? saved.used : 0;
    const resetAt = new Date(Date.parse(`${today}T00:00:00.000Z`) + 86400000 + config.resetHour * 3600000).toISOString();
    return { configured: true, ...config, used, remaining: Math.max(0, config.limit - used),
      resetAt,
      ...(used > 0 && (saved.unit !== config.unit || saved.resetHour !== config.resetHour)
        ? { error: "upstream_budget_unit_changed" } : {}) };
  }

  status(id) {
    const record = this.state.profiles[id];
    const entry = this.state.snapshot.profiles[id];
    return { registered: !!record, phase: record?.phase ?? "unknown", budget: this.budgetStatus(),
      fetchedAt: entry?.fetchedAt ?? null, generation: this.state.snapshot.generation,
      lastAttemptAt: record?.lastAttemptAt ?? null, lastError: record?.lastError ?? null,
      lastPublishError: this.state.lastPublishError, cooldownUntil: this.state.cooldownUntil,
      paused: !!record && Date.now() - record.lastSeenAt > INACTIVE_MS };
  }

  async register(profile) {
    const id = profileId(profile);
    const now = Date.now();
    let record = this.state.profiles[id];
    const wasPaused = !!record && now - record.lastSeenAt > INACTIVE_MS;
    if (!record || wasPaused) {
      const budget = budgetConfig(this.env);
      if (!record && Object.keys(this.state.profiles).length >= MAX_PROFILES) {
        throw new RequestError(503, "location_capacity_reached");
      }
      const active = Object.entries(this.state.profiles).filter(([key, value]) =>
        key !== id && now - value.lastSeenAt <= INACTIVE_MS).length;
      const perDay = 96 * (budget.unit === "request" ? 5 : 1 + 24 + 8 + 24);
      if (budget.enforced && (active + 1) * perDay > budget.limit) throw new RequestError(503, "upstream_budget_insufficient");
      record ??= { profile, lastAttemptAt: 0, lastError: null, phase: "queued" };
      if (wasPaused) { record.lastAttemptAt = 0; record.lastError = null; record.phase = "queued"; }
      this.state.profiles[id] = record;
    }
    let changed = false;
    if (dayAt(record.lastSeenAt ?? 0) !== dayAt(now)) { record.lastSeenAt = now; changed = true; }
    const entry = this.state.snapshot.profiles[id];
    if (!entry || now - entry.fetchedAt > INACTIVE_MS) {
      // Failed fills retry on Cron, never on every device poll.
      if (!this.state.jobs.some((job) => job.ids.includes(id)) && record.lastAttemptAt === 0) {
        let batch = this.state.jobs.find((job) => job.kind === "initial" && job.cursor === 0 && !job.started);
        if (!batch) {
          batch = { id: `initial:${this.state.nextJob++}`, requiredAfter: now, kind: "initial",
            ids: [], cursor: 0, updates: {}, publishAttempts: 0 };
          this.state.jobs.push(batch);
        }
        batch.ids.push(id); record.phase = "queued"; changed = true;
      }
    }
    if (record.phase === "paused") { record.phase = "queued"; changed = true; }
    if (changed) await this.save();
    if (this.state.jobs.length) await this.arm(now + COALESCE_MS);
    const status = this.status(id);
    const budget = status.budget;
    if (!budget.configured || budget.error) return jsonError(503, budget.error ?? "upstream_budget_not_configured");
    if (budget.remaining === 0) return jsonError(503, "upstream_budget_exhausted");
    if (record.lastError && record.phase === "failed") {
      return jsonError(503, record.lastError.code, { "Retry-After": "60" });
    }
    return jsonError(503, "snapshot_warming", { "Retry-After": "5" });
  }

  async enqueueCron(scheduledTime) {
    if (!Number.isFinite(scheduledTime)) throw new RequestError(400, "invalid_schedule");
    const slot = Math.floor(scheduledTime / INTERVAL_MS);
    if (slot <= this.state.lastCronSlot) return;
    this.state.lastCronSlot = slot;
    const now = Date.now();
    const ids = [];
    for (const [id, record] of Object.entries(this.state.profiles)) {
      if (now - record.lastSeenAt > INACTIVE_MS) { record.phase = "paused"; continue; }
      if ((this.state.snapshot.profiles[id]?.fetchedAt ?? 0) < slot * INTERVAL_MS) ids.push(id);
    }
    // One waiting periodic batch; do not replay obsolete slots after outages.
    this.state.jobs = this.state.jobs.filter((job) => job.kind !== "cron" || job.started);
    if (ids.length) this.state.jobs.push({ id: `cron:${slot}`, requiredAfter: slot * INTERVAL_MS,
      kind: "cron", ids, cursor: 0,
      updates: {}, publishAttempts: 0 });
    await this.save();
    if (this.state.jobs.length) await this.arm(now + COALESCE_MS);
  }

  async fetch(request) {
    try {
      if (request.method !== "POST") return jsonError(405, "method_not_allowed");
      if (Number(request.headers.get("Content-Length")) > 2048) return jsonError(413, "request_too_long");
      const text = await request.text();
      if (text.length > 2048) return jsonError(413, "request_too_long");
      const body = JSON.parse(text);
      return await this.exclusive(async () => {
        getConfig(this.env);
        const action = new URL(request.url).pathname;
        if (action === "/cron") {
          await this.enqueueCron(body.scheduledTime);
          return jsonResponse({ queued: true });
        }
        const { profile, id } = this.knownProfile(body.profile);
        if (action === "/status") return jsonResponse(this.status(id));
        if (action === "/register") return this.register(profile);
        if (action === "/touch") {
          const record = this.state.profiles[id];
          if (record && dayAt(record.lastSeenAt) !== dayAt(Date.now())) {
            const paused = Date.now() - record.lastSeenAt > INACTIVE_MS;
            if (paused) return this.register(profile);
            record.lastSeenAt = Date.now(); await this.save();
          }
          return jsonResponse({ ok: true });
        }
        return jsonError(404, "unsupported_coordinator_action");
      });
    } catch (error) {
      return error instanceof RequestError ? jsonError(error.status, error.message)
        : jsonError(500, "coordinator_internal_error");
    }
  }

  reserve(count) {
    return this.exclusive(async () => {
      const config = budgetConfig(this.env);
      const now = Date.now();
      if (now < this.state.cooldownUntil) {
        const error = new RequestError(429, "upstream_http_429");
        error.retryAt = this.state.cooldownUntil; throw error;
      }
      if (!config.enforced) return;
      const day = budgetDay(now, config.resetHour);
      if (this.state.budget?.used && (this.state.budget.unit !== config.unit
          || (this.state.budget.resetHour ?? 0) !== config.resetHour)) {
        throw new RequestError(503, "upstream_budget_unit_changed");
      }
      if (this.state.budget?.day !== day) this.state.budget = { day, unit: config.unit, resetHour: config.resetHour, used: 0 };
      const budget = this.state.budget;
      budget.unit = config.unit;
      const cost = config.unit === "request" ? 1 : count;
      if (budget.used + cost > config.limit) throw new RequestError(503, "upstream_budget_exhausted");
      budget.used += cost;
      await this.ctx.storage.put("budget:v2", budget);
    });
  }

  alarm() {
    if (this.running) return this.running;
    this.running = this.processAlarm().finally(() => { this.running = null; });
    return this.running;
  }

  async processAlarm() {
    // One location per alarm: <=39 upstream calls even with 8 alerts and retries.
    const work = await this.exclusive(async () => {
      const job = this.state.jobs[0];
      if (!job) return null;
      job.started = true;
      // A new-location fill can finish after the matching Cron was enqueued.
      // Skip already refreshed profiles instead of fetching/publishing twice.
      while (job.cursor < job.ids.length
          && (this.state.snapshot.profiles[job.ids[job.cursor]]?.fetchedAt ?? 0) >= job.requiredAfter) job.cursor++;
      const id = job.ids[job.cursor];
      if (id) {
        const record = this.state.profiles[id];
        record.phase = "fetching"; record.lastAttemptAt = Date.now();
      }
      await this.ctx.storage.setAlarm(Date.now() + 120000); // Crash recovery, before network IO.
      await this.save();
      return { jobId: job.id, id, profile: id ? this.state.profiles[id].profile : null };
    });
    if (!work) return;
    if (work.id) {
      let entry;
      let failure;
      try {
        const reserve = (count) => this.reserve(count);
        reserve.cooldown = (at) => this.exclusive(async () => {
          this.state.cooldownUntil = Math.max(this.state.cooldownUntil, at);
          await this.save();
        });
        entry = await collectProfile(this.env, work.profile, reserve);
      }
      catch (error) {
        failure = { code: error instanceof RequestError ? error.message : "upstream_invalid_payload",
          at: Date.now(), retryAt: error.retryAt ?? 0 };
      }
      await this.exclusive(async () => {
        const job = this.state.jobs[0];
        const record = this.state.profiles[work.id];
        if (job?.id !== work.jobId) throw new Error("batch_changed");
        if (entry) { job.updates[work.id] = entry; record.phase = "publishing"; record.lastError = null; }
        else {
          record.phase = "failed"; record.lastError = failure;
          this.state.cooldownUntil = Math.max(this.state.cooldownUntil, failure.retryAt);
          console.warn(JSON.stringify({ event: "weather_refresh_failed", batch: job.id, code: failure.code }));
        }
        job.cursor++;
        await this.save();
      });
    }
    await this.exclusive(async () => {
      const job = this.state.jobs[0];
      if (job.cursor < job.ids.length) {
        await this.ctx.storage.setAlarm(Date.now() + 1000); return;
      }
      if (Object.keys(job.updates).length) {
        if (!job.publication) {
          const profiles = { ...this.state.snapshot.profiles };
          for (const [id, entry] of Object.entries(job.updates)) {
            profiles[id] = { ...entry, lastSeenAt: this.state.profiles[id].lastSeenAt };
          }
          job.publication = { version: 2, generation: this.state.snapshot.generation + 1,
            publishedAt: Date.now(), profiles };
          await this.save(); // Persist stable generation/body before publishing.
        }
        const value = JSON.stringify(job.publication);
        const bytes = new TextEncoder().encode(value).byteLength;
        if (bytes > MAX_BYTES) this.state.lastPublishError = "snapshot_too_large";
        else {
          try {
            await this.env.WEATHER_CACHE.put(SNAPSHOT_KEY, value);
            this.state.snapshot = job.publication;
            this.state.lastPublishError = null;
            for (const id of Object.keys(job.updates)) this.state.profiles[id].phase = "ready";
            console.log(JSON.stringify({ event: "weather_snapshot_published", batch: job.id,
              generation: job.publication.generation, locations: Object.keys(job.updates).length, bytes, kvWrites: 1 }));
          } catch {
            this.state.lastPublishError = "snapshot_publish_failed";
            if (++job.publishAttempts < 3) {
              await this.save(); await this.ctx.storage.setAlarm(Date.now() + 60000); return;
            }
          }
        }
        if (this.state.lastPublishError) {
          for (const id of Object.keys(job.updates)) {
            this.state.profiles[id].phase = "failed";
            this.state.profiles[id].lastError = { code: this.state.lastPublishError, at: Date.now(), retryAt: 0 };
          }
        }
      }
      this.state.jobs.shift();
      await this.save();
      if (this.state.jobs.length) await this.ctx.storage.setAlarm(Date.now() + COALESCE_MS);
      else await this.ctx.storage.deleteAlarm();
    });
  }
}

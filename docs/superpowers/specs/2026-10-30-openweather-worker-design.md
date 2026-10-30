# OpenWeather Worker design

The approved approach is a standalone Cloudflare Worker bound to a custom
subdomain, with a device token, OpenWeather Secrets, and KV last-success storage.
This change implements the Worker first; firmware integration and cloud deployment
are subsequent steps.

- Keep the original JSON response and the independent One Call 3.0/4.0 adapters.
- Allow only the weather, timeline, alert, and air-quality GET paths consumed by
  this firmware. Never accept an arbitrary upstream URL or forward device headers.
- Authenticate every weather request before accessing upstream or cached data.
- Use HTTPS to the global OpenWeather endpoint, with one regional retry for
  transient failures. Bound the entire response-body read with a timeout.
- Store validated successful JSON in KV. Fresh data lasts 600 seconds; temporary
  upstream failures may use data fetched at most 10,800 seconds ago. Authentication
  and subscription errors must remain visible rather than serving stale data.
- Use stable cache keys for near-current rolling air-quality windows, but preserve
  exact semantics for explicitly historical queries and hourly pagination.
- Expose cache state and fetch time in headers. Keep HTTP responses private and
  uncacheable outside the authenticated handler.
- Provide a public liveness endpoint, local tests without real credentials,
  pinned deployment tooling, and Chinese setup instructions.
- No real credentials, cloud resources, firmware changes, or deployment in this
  implementation step.

Validation: Node's built-in test runner exercises routing, credential isolation,
cache aging, malformed bodies, deadlines and upstream failover. Wrangler dry-run
checks the deployable bundle; local workerd smoke checks health/auth responses.

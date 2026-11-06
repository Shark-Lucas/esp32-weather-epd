#ifndef RELAY_WARMUP_H
#define RELAY_WARMUP_H

#include <cstdint>

// Independent of Arduino so the 120-second deadline can be checked on a host.
class RelayWarmup
{
public:
  static constexpr uint32_t LIMIT_MS = 120000;
  void begin(uint32_t now) { if (!active_) { active_ = true; started_ = now; } }
  bool active() const { return active_; }
  uint32_t remaining(uint32_t now) const
  {
    if (!active_) return LIMIT_MS;
    const uint32_t elapsed = now - started_; // millis() wrap is intentional.
    return elapsed < LIMIT_MS ? LIMIT_MS - elapsed : 0;
  }
  uint32_t pollDelay(uint32_t now, uint32_t seconds) const
  {
    const uint32_t bounded = seconds < 1 || seconds > 10 ? 5 : seconds;
    const uint32_t desired = bounded * 1000;
    return desired < remaining(now) ? desired : remaining(now);
  }
private:
  bool active_ = false;
  uint32_t started_ = 0;
};

#endif

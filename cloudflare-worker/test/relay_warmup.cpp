#include <cassert>
#include <cstdint>
#include "../../platformio/include/relay_warmup.h"

int main()
{
  RelayWarmup warmup;
  assert(!warmup.active());
  warmup.begin(1000);
  warmup.begin(5000); // Repeated warming responses never extend the deadline.
  assert(warmup.remaining(1000) == 120000);
  assert(warmup.remaining(119000) == 2000);
  assert(warmup.pollDelay(119000, 5) == 2000);
  assert(warmup.pollDelay(1000, 0) == 5000);
  assert(warmup.pollDelay(1000, 99) == 5000);
  assert(warmup.remaining(121000) == 0);
  RelayWarmup wrap;
  wrap.begin(UINT32_MAX - 1000);
  assert(wrap.remaining(999) == 118000);
}

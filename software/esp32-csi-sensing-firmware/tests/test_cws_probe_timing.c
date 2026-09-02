#include "cws_probe_timing.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

int main(void)
{
    uint32_t interval = 0;
    uint32_t timeout = 0;

    assert(cws_probe_timing_for_frequency(1, &interval, &timeout));
    assert(interval == 1000 && timeout == 1000);
    assert(cws_probe_timing_for_frequency(40, &interval, &timeout));
    assert(interval == 25 && timeout == 25);
    assert(cws_probe_timing_for_frequency(50, &interval, &timeout));
    assert(interval == 20 && timeout == 20);
    assert(!cws_probe_timing_for_frequency(0, &interval, &timeout));
    assert(!cws_probe_timing_for_frequency(51, &interval, &timeout));
    assert(!cws_probe_timing_for_frequency(40, NULL, &timeout));
    assert(!cws_probe_timing_for_frequency(40, &interval, NULL));
    assert(cws_probe_timeout_ms(0) == 0);
    assert(cws_probe_timeout_ms(40) == 25);
    return 0;
}

#include "voltage_state.h"

bool voltage_state_is_low(int voltage_mv, uint16_t threshold_mv)
{
    return voltage_mv < (int)threshold_mv;
}

bool voltage_state_is_low_dynamic(voltage_state_t *state,
                                  int voltage_mv,
                                  uint16_t floor_mv,
                                  uint16_t drop_mv)
{
    if (voltage_mv > state->peak_mv) {
        state->peak_mv = voltage_mv;
    }
    int drop_threshold = state->peak_mv - (int)drop_mv;
    int threshold = (drop_threshold > (int)floor_mv) ? drop_threshold : (int)floor_mv;
    return voltage_mv < threshold;
}

void voltage_state_reset_peak(voltage_state_t *state)
{
    state->peak_mv = 0;
}

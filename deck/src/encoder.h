// OpenBricx Deck — HW-040 rotary encoder via PCNT (hardware quadrature decode).
#pragma once

void encoder_init(void);

// Net detents turned since the last call: positive = clockwise, negative = CCW.
// One physical detent on the HW-040 = one unit (the 4x quadrature is divided out).
int encoder_take_delta(void);

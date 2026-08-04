// OpenBricx Deck — battery / charge sensing on the ADC divider.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void battery_init(void);

typedef struct {
    int  percent;     // 0–100
    bool charging;    // USB lifting the rail
    bool no_battery;  // USB powered, no cell present
    int  raw;         // averaged raw ADC count (for calibration via DIAG)
    int  variance;    // max-min spread across the sample burst (no-cell detect)
} battery_status_t;

// Sample the divider (averaged) and classify charge state.
battery_status_t battery_read(void);

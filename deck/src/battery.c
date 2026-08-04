// OpenBricx Deck — battery monitoring (ADC oneshot).
//
// Port of the original heuristic: average 10 samples, use the spread to detect a
// disconnected cell (USB-only), and a raised resting voltage to detect charging.

#include "battery.h"
#include "pinout.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_chan;
static int s_last_clean_pct = 0;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int map_raw(int raw)
{
    // Linear map of the calibrated resting range to 0–100 %.
    long pct = (long)(raw - BAT_RAW_MIN) * 100 / (BAT_RAW_MAX - BAT_RAW_MIN);
    return clampi((int)pct, 0, 100);
}

void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed");
        return;
    }

    // GPIO4 -> ADC1 channel 3 on the ESP32-S3.
    s_chan = ADC_CHANNEL_3;
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,      // ~0–3.1 V range
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(s_adc, s_chan, &chan_cfg);
    ESP_LOGI(TAG, "battery ADC ready on GPIO%d", PIN_BAT_ADC);
}

battery_status_t battery_read(void)
{
    battery_status_t st = {0};

    long sum = 0;
    int b_min = 4096, b_max = 0;
    for (int i = 0; i < 10; i++) {
        int raw = 0;
        adc_oneshot_read(s_adc, s_chan, &raw);
        sum += raw;
        if (raw < b_min) b_min = raw;
        if (raw > b_max) b_max = raw;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    int raw_avg = (int)(sum / 10);
    int variance = b_max - b_min;
    st.raw = raw_avg;
    st.variance = variance;

    // Calibration aid: read this over serial at full charge and at the moment the
    // deck browns out to set BAT_RAW_MAX / BAT_RAW_MIN precisely. ESP_LOGx isn't
    // reachable over CDC while TinyUSB owns USB, so the raw is also surfaced in the
    // DIAG block (obx_emit_boot_diag) — send `DIAG` on the CDC link to read it.
    ESP_LOGI(TAG, "raw_avg=%d var=%d (min=%d max=%d)", raw_avg, variance, BAT_RAW_MIN, BAT_RAW_MAX);

    if (variance > 80) {
        // Large fluctuation = USB present but no battery cell connected.
        st.no_battery = true;
        st.charging = true;
        st.percent = 100;
        return st;
    }

    st.charging = raw_avg > 2430;
    if (st.charging) {
        // Linear chargers (TP4056) lift the node ~0.15–0.2 V; subtract an offset
        // to estimate the true cell voltage underneath the charge push.
        st.percent = map_raw(raw_avg - 40);
        st.percent = (st.percent > s_last_clean_pct) ? st.percent : s_last_clean_pct;
    } else {
        st.percent = map_raw(raw_avg);
        s_last_clean_pct = st.percent;
    }
    return st;
}

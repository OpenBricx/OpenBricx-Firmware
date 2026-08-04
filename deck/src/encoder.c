// OpenBricx Deck — rotary encoder using the ESP-IDF PCNT peripheral.
//
// Full quadrature decode: channel A counts edges on CLK gated by DT, channel B
// counts edges on DT gated by CLK. The HW-040 emits 4 counts per detent, so we
// divide the raw count by 4 to get clean detent deltas.

#include "encoder.h"
#include "pinout.h"

#include "driver/pulse_cnt.h"
#include "esp_log.h"

static const char *TAG = "encoder";

static pcnt_unit_handle_t s_unit;
static int s_last = 0;

#define PCNT_HIGH_LIMIT  1000
#define PCNT_LOW_LIMIT  -1000

void encoder_init(void)
{
    pcnt_unit_config_t unit_cfg = {
        .high_limit = PCNT_HIGH_LIMIT,
        .low_limit = PCNT_LOW_LIMIT,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &s_unit));

    pcnt_glitch_filter_config_t filter = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_unit, &filter));

    // Channel A: edges on CLK, direction from DT.
    pcnt_chan_config_t chan_a_cfg = {
        .edge_gpio_num = PIN_ENC_CLK,
        .level_gpio_num = PIN_ENC_DT,
    };
    pcnt_channel_handle_t chan_a;
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &chan_a_cfg, &chan_a));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_a,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_a,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    // Channel B: edges on DT, direction from CLK.
    pcnt_chan_config_t chan_b_cfg = {
        .edge_gpio_num = PIN_ENC_DT,
        .level_gpio_num = PIN_ENC_CLK,
    };
    pcnt_channel_handle_t chan_b;
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &chan_b_cfg, &chan_b));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_b,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_b,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    ESP_ERROR_CHECK(pcnt_unit_enable(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_unit));

    ESP_LOGI(TAG, "encoder ready (CLK=%d DT=%d)", PIN_ENC_CLK, PIN_ENC_DT);
}

int encoder_take_delta(void)
{
    int raw = 0;
    if (pcnt_unit_get_count(s_unit, &raw) != ESP_OK) return 0;

    int detents = raw / 4;          // 4 counts per physical detent
    int delta = detents - s_last;
    s_last = detents;
    return delta;
}

// OpenBricx Deck — PCF8574 input scanning.
//
// The PCF8574 is a quasi-bidirectional expander: writing 1 to a pin lets it be
// read as an input (weak pull-up), writing 0 drives it low. We keep a shadow
// output byte; rows are driven low one at a time and the three column pins are
// read back. Touch (P0) and encoder switch (P7) are read from the same byte.

#include "input.h"
#include "pinout.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "input";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pcf;
static bool s_ready = false;

// Shadow of the PCF8574 port. Inputs/idle-high pins are 1, driven-low rows are 0.
// Bits: P0 touch(in), P1-3 cols(in), P4-6 rows(out), P7 enc-sw(in).
#define PCF_IDLE 0xFF

static esp_err_t pcf_write(uint8_t value)
{
    return i2c_master_transmit(s_pcf, &value, 1, 50);
}

static esp_err_t pcf_read(uint8_t *value)
{
    return i2c_master_receive(s_pcf, value, 1, 50);
}

bool input_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_I2C_SCL,
        .sda_io_num = PIN_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed");
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF8574_ADDR,
        // 50 kHz: the new I2C master driver's internal pull-ups are weak, so a
        // slower clock tolerates the longer rise time on a board without strong
        // external pull-ups. (The original Arduino bus ran fine; this just gives
        // the same wiring more timing margin.)
        .scl_speed_hz = 50000,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_pcf) != ESP_OK) {
        ESP_LOGE(TAG, "failed to add PCF8574 device");
        return false;
    }

    // Probe + set all pins high (inputs / rows idle-high). Retry a few times: the
    // expander can need a moment after power-up, and the very first transaction on
    // a freshly-created bus occasionally NACKs.
    esp_err_t probe = ESP_FAIL;
    for (int i = 0; i < 8; i++) {
        probe = pcf_write(PCF_IDLE);
        if (probe == ESP_OK) break;
        esp_rom_delay_us(2000);
    }
    if (probe != ESP_OK) {
        ESP_LOGE(TAG, "PCF8574 not responding at 0x%02x", PCF8574_ADDR);
        s_ready = false;
        return false;
    }
    ESP_LOGI(TAG, "PCF8574 ready at 0x%02x", PCF8574_ADDR);
    s_ready = true;
    return true;
}

bool input_ready(void)
{
    return s_ready;
}

bool input_read_raw(uint8_t *value)
{
    if (!s_ready) return false;
    if (pcf_write(PCF_IDLE) != ESP_OK) return false;
    esp_rom_delay_us(30);
    uint8_t in = PCF_IDLE;
    if (pcf_read(&in) != ESP_OK) return false;
    if (value) *value = in;
    return true;
}

// Number of consecutive scans a key must read pressed before we report it. The
// PCF8574 matrix (weak pull-ups, internal-pull-up I2C, a fast 5 ms scan loop)
// throws the odd bad read; without confirmation a single glitch fires a macro on
// its own (the original Arduino loop hid this by scanning ~6x slower with far more
// settle time). At a 5 ms task period, 3 scans = a 15 ms stable press requirement.
#define MATRIX_DEBOUNCE 3

void input_scan_matrix(bool pressed_out[9])
{
    static bool ready = false;     // first call only establishes a baseline
    static uint8_t on_cnt[9];      // consecutive pressed reads (saturates)
    static bool held[9];           // confirmed/reported state
    bool raw[9] = {0};

    for (int row = 0; row < 3; row++) {
        // Drive the selected row low, all other rows + inputs high.
        uint8_t out = PCF_IDLE & ~(1u << (PCF_ROW1 + row));
        if (pcf_write(out) != ESP_OK) continue;   // skip row on bus error (no false press)
        esp_rom_delay_us(60);

        // Read the columns twice and require agreement, so a single corrupted I2C
        // read can't register a column as low. `a | b` keeps a column-low bit set
        // only when both reads saw it low (bit clear in both).
        uint8_t a = PCF_IDLE, b = PCF_IDLE;
        if (pcf_read(&a) != ESP_OK) continue;
        esp_rom_delay_us(20);
        if (pcf_read(&b) != ESP_OK) continue;
        uint8_t in = a | b;
        if (!(in & (1u << PCF_COL1))) raw[row * 3 + 0] = true;
        if (!(in & (1u << PCF_COL2))) raw[row * 3 + 1] = true;
        if (!(in & (1u << PCF_COL3))) raw[row * 3 + 2] = true;
    }
    pcf_write(PCF_IDLE);  // release rows

    // Sanity guard: you can't meaningfully hold 5+ of 9 macro keys at once, so a
    // scan that reports that many is a bus glitch / power transient (exactly what
    // happens on USB connect). Drop it wholesale rather than fire a fistful of
    // macros — this is what was muting + locking the PC.
    int n_raw = 0;
    for (int i = 0; i < 9; i++) if (raw[i]) n_raw++;
    if (n_raw > 4) {
        for (int i = 0; i < 9; i++) raw[i] = false;
    }

    // Establish the baseline on the first scan so any power-on/boot glitch (or a
    // genuinely held key) doesn't fire a macro the instant the task starts.
    if (!ready) {
        for (int i = 0; i < 9; i++) {
            on_cnt[i] = 0;
            held[i] = raw[i];
            pressed_out[i] = false;
        }
        ready = true;
        return;
    }

    for (int i = 0; i < 9; i++) {
        pressed_out[i] = false;
        if (raw[i]) {
            if (on_cnt[i] < MATRIX_DEBOUNCE) on_cnt[i]++;
        } else {
            on_cnt[i] = 0;   // any release breaks the streak
        }

        if (!held[i] && on_cnt[i] >= MATRIX_DEBOUNCE) {
            held[i] = true;
            pressed_out[i] = true;   // fire once, on the confirmed press edge
        } else if (held[i] && on_cnt[i] == 0) {
            held[i] = false;         // confirmed release — re-arm
        }
    }
}

void input_debug_scan(bool pressed[9])
{
    for (int i = 0; i < 9; i++) pressed[i] = false;
    if (!s_ready) return;

    for (int row = 0; row < 3; row++) {
        uint8_t out = PCF_IDLE & ~(1u << (PCF_ROW1 + row));
        pcf_write(out);
        esp_rom_delay_us(50);

        uint8_t in = PCF_IDLE;
        if (pcf_read(&in) != ESP_OK) continue;
        if (!(in & (1u << PCF_COL1))) pressed[row * 3 + 0] = true;
        if (!(in & (1u << PCF_COL2))) pressed[row * 3 + 1] = true;
        if (!(in & (1u << PCF_COL3))) pressed[row * 3 + 2] = true;
    }
    pcf_write(PCF_IDLE);
}

int input_bus_scan(uint8_t *addrs, int max)
{
    if (!s_bus || !addrs || max <= 0) return 0;
    int n = 0;
    for (uint16_t a = 0x08; a <= 0x77 && n < max; a++) {
        // i2c_master_probe returns ESP_OK only when the address ACKs.
        if (i2c_master_probe(s_bus, a, 20) == ESP_OK) addrs[n++] = (uint8_t)a;
    }
    return n;
}

void input_scan_raw(uint8_t rows_out[3])
{
    for (int row = 0; row < 3; row++) {
        rows_out[row] = 0xFF;
        uint8_t out = PCF_IDLE & ~(1u << (PCF_ROW1 + row));
        if (pcf_write(out) != ESP_OK) continue;
        esp_rom_delay_us(60);
        uint8_t in = PCF_IDLE;
        if (pcf_read(&in) == ESP_OK) rows_out[row] = in;
    }
    pcf_write(PCF_IDLE);
}

static uint8_t read_port_idle(void)
{
    pcf_write(PCF_IDLE);
    esp_rom_delay_us(30);
    uint8_t in = PCF_IDLE;
    pcf_read(&in);
    return in;
}

bool input_encoder_switch(void)
{
    return (read_port_idle() & (1u << PCF_ENC_SW)) == 0;  // active low
}

bool input_touch(void)
{
    // TTP223 drives the line HIGH when touched (idle low).
    return (read_port_idle() & (1u << PCF_TOUCH)) != 0;
}

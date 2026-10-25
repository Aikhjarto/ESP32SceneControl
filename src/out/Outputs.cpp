#include "out/Outputs.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

namespace ss {

static const char* TAG = "outputs";

constexpr int kLedcPerMode = SOC_LEDC_CHANNEL_NUM;
constexpr int kMcpwmPerUnit = Outputs::kMcpwmPerUnit;
constexpr int kLedcBits = 16;
constexpr uint32_t kMcpwmGroupHz = 160000000;  // PLL_F160M, default group prescale 1 (ESP-IDF 6)

static int capacity(HwKind k) {
    switch (k) {
        case HwKind::LedcHs:
        case HwKind::LedcLs:
            return kLedcPerMode;
        case HwKind::Mcpwm0:
        case HwKind::Mcpwm1:
            return kMcpwmPerUnit;
        default:
            return 0;
    }
}

static ledc_mode_t ledcMode(HwKind k) { return k == HwKind::LedcHs ? LEDC_HIGH_SPEED_MODE : LEDC_LOW_SPEED_MODE; }

static void addLog(std::string& log, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void addLog(std::string& log, const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s", buf);
    log += buf;
    log += '\n';
}

bool Outputs::gpioUsableForServo(int gpio) {
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) return false;  // also rejects input-only 34..39
    if (gpio == 1 || gpio == 3) return false;            // UART0 console
    if (gpio >= 6 && gpio <= 11) return false;           // SPI flash
    return true;
}

void Outputs::begin(const Config& cfg, std::string& log) {
    initPca(cfg, log);
    for (int i = 0; i < kMaxServos; ++i) {
        if (cfg.servo[i].pin.type != PinType::None) allocate(cfg, i, log);
    }
    initLedc(cfg);
    initMcpwm(cfg, 0, log);
    initMcpwm(cfg, 1, log);

    for (int i = 0; i < kMaxServos; ++i) {
        if (map_[i].kind == HwKind::None) continue;
        const ServoConfig& s = cfg.servo[i];
        float longest = std::min(std::max(s.minUs, s.maxUs) + std::max(s.trimUs, 0.0f), 3000.0f);
        if (longest > maxPulseUs(map_[i])) {
            addLog(log, "servo %d: %.0f us pulse does not fit the PWM period of %s, clamped to %.0f us", i, longest,
                   describe(i).c_str(), maxPulseUs(map_[i]));
        }
    }
    ESP_LOGI(TAG, "allocated: LEDC-HS %d/%d, LEDC-LS %d/%d, MCPWM0 %d/%d, MCPWM1 %d/%d", used_[0], kLedcPerMode,
             used_[1], kLedcPerMode, used_[2], kMcpwmPerUnit, used_[3], kMcpwmPerUnit);
}

bool Outputs::allocate(const Config& cfg, int servo, std::string& log) {
    const PinSpec pin = cfg.servo[servo].pin;

    if (pin.type == PinType::Pca) {
        if (pin.board >= kMaxPca || pin.num >= kPcaChannels) {
            addLog(log, "servo %d: invalid pin pca.%u.%u", servo, pin.board, pin.num);
            return false;
        }
        if (!cfg.pca[pin.board].enable) {
            addLog(log, "servo %d: pca.%u.%u is on PCA9685 board %u, which is disabled ($/pca/%u/enable)", servo,
                   pin.board, pin.num, pin.board, pin.board);
            return false;
        }
        if (!pca_[pin.board].ok()) return false;  // missing board already reported by initPca
        for (int j = 0; j < servo; ++j) {
            if (pin_[j] == pin) {
                addLog(log, "servo %d: pca.%u.%u already used by servo %d", servo, pin.board, pin.num, j);
                return false;
            }
        }
        map_[servo] = {HwKind::Pca, pin.board, pin.num};
        pin_[servo] = pin;
        return true;
    }

    const int gpio = pin.num;
    if (!gpioUsableForServo(gpio)) {
        addLog(log, "servo %d: gpio.%d cannot be used as servo output", servo, gpio);
        return false;
    }
    bool i2cUsed = false;
    for (const PcaConfig& p : cfg.pca) i2cUsed |= p.enable;
    if ((i2cUsed && (pin == cfg.i2c.sda || pin == cfg.i2c.scl)) || pin == cfg.button.pin) {
        addLog(log, "servo %d: gpio.%d is used by I2C or the button", servo, gpio);
        return false;
    }
    for (int j = 0; j < servo; ++j) {
        if (pin_[j] == pin) {
            addLog(log, "servo %d: gpio.%d already used by servo %d", servo, gpio, j);
            return false;
        }
    }

    static const HwKind pools[] = {HwKind::LedcHs, HwKind::LedcLs, HwKind::Mcpwm0, HwKind::Mcpwm1};
    HwKind chosen = HwKind::None;
    const Driver drv = cfg.servo[servo].driver;
    for (int n = 0; n < 4 && chosen == HwKind::None; ++n) {
        // Auto spreads servos round-robin over all four blocks; a forced driver uses its two.
        int idx = drv == Driver::Auto ? (rr_ + n) % 4 : n;
        HwKind k = pools[idx];
        bool allowed = drv == Driver::Auto || (drv == Driver::Ledc && (k == HwKind::LedcHs || k == HwKind::LedcLs)) ||
                       (drv == Driver::Mcpwm && (k == HwKind::Mcpwm0 || k == HwKind::Mcpwm1));
        if (allowed && used_[idx] < capacity(k)) {
            chosen = k;
            if (drv == Driver::Auto) rr_ = (idx + 1) % 4;
        }
    }
    if (chosen == HwKind::None) {
        addLog(log, "servo %d: no free %s channel left", servo, drv == Driver::Mcpwm ? "MCPWM" : "PWM");
        return false;
    }
    int idx = static_cast<int>(chosen) - 1;
    map_[servo] = {chosen, 0, static_cast<uint8_t>(used_[idx]++)};
    pin_[servo] = pin;
    return true;
}

void Outputs::initLedc(const Config& cfg) {
    ledcFreq_ = std::clamp<uint32_t>(cfg.pwm.ledcFreqHz, 40, 1000);
    for (HwKind k : {HwKind::LedcHs, HwKind::LedcLs}) {
        if (used_[static_cast<int>(k) - 1] == 0) continue;
        ledc_timer_config_t tc = {};
        tc.speed_mode = ledcMode(k);
        tc.duty_resolution = static_cast<ledc_timer_bit_t>(kLedcBits);
        tc.timer_num = LEDC_TIMER_0;
        tc.freq_hz = ledcFreq_;
        tc.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&tc));
    }
    for (int i = 0; i < kMaxServos; ++i) {
        const HwChannel& hw = map_[i];
        if (hw.kind != HwKind::LedcHs && hw.kind != HwKind::LedcLs) continue;
        ledc_channel_config_t cc = {};
        cc.gpio_num = pin_[i].num;
        cc.speed_mode = ledcMode(hw.kind);
        cc.channel = static_cast<ledc_channel_t>(hw.ch);
        cc.timer_sel = LEDC_TIMER_0;
        cc.duty = 0;  // output low (no pulses) until the motion engine enables the servo
        ESP_ERROR_CHECK(ledc_channel_config(&cc));
    }
}

void Outputs::initMcpwm(const Config& cfg, int unit, std::string& log) {
    const HwKind kind = unit == 0 ? HwKind::Mcpwm0 : HwKind::Mcpwm1;
    if (used_[static_cast<int>(kind) - 1] == 0) return;
    McpwmUnit& u = mcpwm_[unit];

    // The timer period register is 16 bit: pick the finest resolution that still fits one period.
    uint32_t freq = std::clamp<uint32_t>(cfg.pwm.mcpwmFreqHz, 40, 1000);
    uint32_t prescale = (kMcpwmGroupHz + 65000 * freq - 1) / (65000 * freq);
    uint32_t resolution = kMcpwmGroupHz / prescale;

    mcpwm_timer_config_t tc = {};
    tc.group_id = unit;
    tc.clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT;
    tc.resolution_hz = resolution;
    tc.count_mode = MCPWM_TIMER_COUNT_MODE_UP;
    tc.period_ticks = resolution / freq;
    ESP_ERROR_CHECK(mcpwm_new_timer(&tc, &u.timer));
    u.ticksPerUs = resolution / 1e6f;
    u.periodUs = 1e6f / freq;

    for (int i = 0; i < kMaxServos; ++i) {
        if (map_[i].kind != kind) continue;
        const int slot = map_[i].ch;
        const int op = slot / MCPWM_LL_GET(GENERATORS_PER_OPERATOR);
        if (!u.oper[op]) {
            mcpwm_operator_config_t oc = {};
            oc.group_id = unit;
            ESP_ERROR_CHECK(mcpwm_new_operator(&oc, &u.oper[op]));
            ESP_ERROR_CHECK(mcpwm_operator_connect_timer(u.oper[op], u.timer));
        }
        mcpwm_comparator_config_t cc = {};
        cc.flags.update_cmp_on_tez = true;  // new pulse width takes effect at the next period
        ESP_ERROR_CHECK(mcpwm_new_comparator(u.oper[op], &cc, &u.cmp[slot]));
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(u.cmp[slot], 0));

        mcpwm_generator_config_t gc = {};
        gc.gen_gpio_num = pin_[i].num;
        ESP_ERROR_CHECK(mcpwm_new_generator(u.oper[op], &gc, &u.gen[slot]));
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(
            u.gen[slot],
            MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(
            u.gen[slot], MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, u.cmp[slot], MCPWM_GEN_ACTION_LOW)));
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(u.gen[slot], 0, true));
        u.forcedOff[slot] = true;
    }

    ESP_ERROR_CHECK(mcpwm_timer_enable(u.timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(u.timer, MCPWM_TIMER_START_NO_STOP));
    ESP_LOGI(TAG, "MCPWM%d: %lu Hz, resolution %lu Hz", unit, static_cast<unsigned long>(freq),
             static_cast<unsigned long>(resolution));
    (void)log;
}

void Outputs::initPca(const Config& cfg, std::string& log) {
    bool any = false;
    for (const PcaConfig& p : cfg.pca) any |= p.enable;
    if (!any) return;

    if (cfg.i2c.sda.type != PinType::Gpio || cfg.i2c.scl.type != PinType::Gpio) {
        addLog(log, "I2C pins not set, PCA9685 disabled");
        return;
    }
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = -1;
    bc.sda_io_num = static_cast<gpio_num_t>(cfg.i2c.sda.num);
    bc.scl_io_num = static_cast<gpio_num_t>(cfg.i2c.scl.num);
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bc, &i2c_) != ESP_OK) {
        addLog(log, "I2C bus init failed (sda gpio.%u, scl gpio.%u)", cfg.i2c.sda.num, cfg.i2c.scl.num);
        i2c_ = nullptr;
        return;
    }
    const uint32_t sclHz = std::clamp<uint32_t>(cfg.i2c.khz, 10, 1000) * 1000;
    for (int b = 0; b < kMaxPca; ++b) {
        const PcaConfig& p = cfg.pca[b];
        if (!p.enable) continue;
        if (!pca_[b].begin(i2c_, static_cast<uint8_t>(p.addr), sclHz, p.freqHz, p.oscHz)) {
            addLog(log, "PCA9685 board %d not found at 0x%02lX", b, static_cast<unsigned long>(p.addr));
        }
    }
}

// Longest pulse a block may output: a pulse as long as the period would hold the line
// permanently high (LEDC) or be rejected (MCPWM).
float Outputs::maxPulseUs(const HwChannel& hw) const {
    float period = 0;
    switch (hw.kind) {
        case HwKind::LedcHs:
        case HwKind::LedcLs:
            period = 1e6f / ledcFreq_;
            break;
        case HwKind::Mcpwm0:
        case HwKind::Mcpwm1:
            period = mcpwm_[hw.kind == HwKind::Mcpwm0 ? 0 : 1].periodUs;
            break;
        case HwKind::Pca:
            period = 1e6f / pca_[hw.unit].freqHz();
            break;
        default:
            break;
    }
    return 0.9f * period;
}

void Outputs::setPulseUs(int servo, float us) {
    const HwChannel& hw = map_[servo];
    us = std::min(us, maxPulseUs(hw));
    switch (hw.kind) {
        case HwKind::LedcHs:
        case HwKind::LedcLs: {
            uint32_t duty = static_cast<uint32_t>(std::lround(us * 1e-6f * ledcFreq_ * (1 << kLedcBits)));
            ledc_set_duty(ledcMode(hw.kind), static_cast<ledc_channel_t>(hw.ch), duty);
            ledc_update_duty(ledcMode(hw.kind), static_cast<ledc_channel_t>(hw.ch));  // latched at period end
            break;
        }
        case HwKind::Mcpwm0:
        case HwKind::Mcpwm1: {
            McpwmUnit& u = mcpwm_[hw.kind == HwKind::Mcpwm0 ? 0 : 1];
            mcpwm_comparator_set_compare_value(u.cmp[hw.ch], static_cast<uint32_t>(std::lround(us * u.ticksPerUs)));
            if (u.forcedOff[hw.ch]) {
                mcpwm_generator_set_force_level(u.gen[hw.ch], -1, true);
                u.forcedOff[hw.ch] = false;
            }
            break;
        }
        case HwKind::Pca:
            pca_[hw.unit].setPulseUs(hw.ch, us);
            break;
        default:
            break;
    }
}

void Outputs::off(int servo) {
    const HwChannel& hw = map_[servo];
    switch (hw.kind) {
        case HwKind::LedcHs:
        case HwKind::LedcLs:
            ledc_stop(ledcMode(hw.kind), static_cast<ledc_channel_t>(hw.ch), 0);
            break;
        case HwKind::Mcpwm0:
        case HwKind::Mcpwm1: {
            McpwmUnit& u = mcpwm_[hw.kind == HwKind::Mcpwm0 ? 0 : 1];
            if (!u.forcedOff[hw.ch]) {
                mcpwm_generator_set_force_level(u.gen[hw.ch], 0, true);
                u.forcedOff[hw.ch] = true;
            }
            break;
        }
        case HwKind::Pca:
            pca_[hw.unit].off(hw.ch);
            break;
        default:
            break;
    }
}

void Outputs::flush() {
    for (Pca9685& p : pca_) p.flush();
}

std::string Outputs::describe(int servo) const {
    const HwChannel& hw = map_[servo];
    char buf[24];
    switch (hw.kind) {
        case HwKind::LedcHs:
            snprintf(buf, sizeof(buf), "ledc-hs.%u", hw.ch);
            break;
        case HwKind::LedcLs:
            snprintf(buf, sizeof(buf), "ledc-ls.%u", hw.ch);
            break;
        case HwKind::Mcpwm0:
            snprintf(buf, sizeof(buf), "mcpwm0.%u", hw.ch);
            break;
        case HwKind::Mcpwm1:
            snprintf(buf, sizeof(buf), "mcpwm1.%u", hw.ch);
            break;
        case HwKind::Pca:
            snprintf(buf, sizeof(buf), "pca%u.%u", hw.unit, hw.ch);
            break;
        default:
            return "none";
    }
    return buf;
}

}  // namespace ss

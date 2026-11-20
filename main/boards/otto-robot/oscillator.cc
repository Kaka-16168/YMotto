#include "oscillator.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "Oscillator";

/* 脉宽范围：标准舵机 500us ~ 2500us，中位 1500us */
static constexpr uint32_t kPulseMinUs = 500;
static constexpr uint32_t kPulseMaxUs = 2500;
static constexpr uint32_t kPulseRangeUs = kPulseMaxUs - kPulseMinUs;

/* 角度容差：小于 1 度跳过写入，减少舵机抖动 */
static constexpr float kAngleEpsilon = 1.0f;

Oscillator::Oscillator() = default;

Oscillator::~Oscillator() {
    Stop();
}

bool Oscillator::Attach(int pin, int channel, ledc_timer_bit_t resolution_bits,
                        uint32_t freq_hz, uint32_t timer_index) {
    if (pin < 0 || channel < 0 || channel > LEDC_CHANNEL_MAX) {
        ESP_LOGE(TAG, "Attach 参数非法: pin=%d channel=%d", pin, channel);
        return false;
    }

    pin_            = pin;
    channel_        = channel;
    timer_index_    = timer_index;
    freq_hz_        = freq_hz;
    resolution_bits_ = resolution_bits;
    duty_max_       = (1u << static_cast<uint32_t>(resolution_bits)) - 1;

    /* 首次使用时初始化定时器（同一定时器只配一次） */
    static uint32_t initialized_timers = 0;
    if ((initialized_timers & (1u << timer_index_)) == 0) {
        ledc_timer_config_t tc = {};
        tc.duty_resolution = resolution_bits_;
        tc.freq_hz         = freq_hz_;
        tc.speed_mode      = LEDC_LOW_SPEED_MODE;
        tc.timer_num       = static_cast<ledc_timer_t>(timer_index_);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        tc.clk_cfg = LEDC_AUTO_CLK;
#else
        tc.clk_cfg = LEDC_AUTO_CLK;
#endif
        esp_err_t err = ledc_timer_config(&tc);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ledc_timer_config 失败: %s", esp_err_to_name(err));
            return false;
        }
        initialized_timers |= (1u << timer_index_);
    }

    ConfigureChannel();

    /* 上电归中，避免舵机甩臂 */
    SetAngle(SERVO_CENTER_ANGLE);
    active_ = true;
    attached_ = true;
    return true;
}

void Oscillator::ConfigureChannel() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    ledc_channel_config_t cc = {};
    cc.gpio_num   = static_cast<gpio_num_t>(pin_);
    cc.speed_mode = LEDC_LOW_SPEED_MODE;
    cc.channel    = static_cast<ledc_channel_t>(channel_);
    cc.intr_type  = LEDC_INTR_DISABLE;
    cc.timer_sel  = static_cast<ledc_timer_t>(timer_index_);
    cc.duty       = 0;
    cc.hpoint     = 0;
#else
    ledc_channel_config_t cc = {};
    cc.gpio_num   = static_cast<gpio_num_t>(pin_);
    cc.speed_mode = LEDC_LOW_SPEED_MODE;
    cc.channel    = static_cast<ledc_channel_t>(channel_);
    cc.intr_type  = LEDC_INTR_DISABLE;
    cc.timer_sel  = static_cast<ledc_timer_t>(timer_index_);
    cc.duty       = 0;
    cc.hpoint     = 0;
#endif
    esp_err_t err = ledc_channel_config(&cc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config 失败 pin=%d: %s", pin_, esp_err_to_name(err));
    }
}

uint32_t Oscillator::AngleToDuty(float angle) {
    if (angle < SERVO_MIN_ANGLE) angle = SERVO_MIN_ANGLE;
    if (angle > SERVO_MAX_ANGLE) angle = SERVO_MAX_ANGLE;

    /* 角度 -> 脉宽(us) -> 占空比计数值 */
    float pulse_us = static_cast<float>(kPulseMinUs) +
                     (angle / 180.0f) * static_cast<float>(kPulseRangeUs);

    /* 1 个计数 = 1e6 / freq / duty_max 微秒 */
    float counts_per_us = static_cast<float>(duty_max_) /
                          (1000000.0f / static_cast<float>(freq_hz_));
    uint32_t duty = static_cast<uint32_t>(pulse_us * counts_per_us + 0.5f);
    if (duty > duty_max_) duty = duty_max_;
    return duty;
}

void Oscillator::SetAngle(float angle) {
    if (!attached_) return;

    float target = angle + static_cast<float>(trim_);
    if (target < SERVO_MIN_ANGLE) target = SERVO_MIN_ANGLE;
    if (target > SERVO_MAX_ANGLE) target = SERVO_MAX_ANGLE;

    /* 变化太小就跳过，消除抖动 */
    if (!active_ && (target - angle_) < kAngleEpsilon &&
        (angle_ - target) < kAngleEpsilon) {
        return;
    }

    angle_  = target;
    active_ = true;

    uint32_t duty = AngleToDuty(angle_);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE,
                             static_cast<ledc_channel_t>(channel_),
                             duty, 0);
#else
    ledc_set_duty(LEDC_LOW_SPEED_MODE,
                  static_cast<ledc_channel_t>(channel_), duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE,
                     static_cast<ledc_channel_t>(channel_));
#endif
}

void Oscillator::SetTrim(int8_t trim) {
    if (trim < SERVO_TRIM_MIN) trim = SERVO_TRIM_MIN;
    if (trim > SERVO_TRIM_MAX) trim = SERVO_TRIM_MAX;
    trim_ = trim;
}

void Oscillator::Stop() {
    if (!attached_) return;
    active_ = false;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE,
                             static_cast<ledc_channel_t>(channel_),
                             0, 0);
#else
    ledc_set_duty(LEDC_LOW_SPEED_MODE,
                  static_cast<ledc_channel_t>(channel_), 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE,
                     static_cast<ledc_channel_t>(channel_));
#endif
}

void Oscillator::SetEnable(bool enable) {
    if (enable) {
        active_ = true;
    } else {
        Stop();
    }
}

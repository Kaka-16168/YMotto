//--------------------------------------------------------------
//-- Oscillator.pde
//-- Generate sinusoidal oscillations in the servos
//--------------------------------------------------------------
//-- (c) Juan Gonzalez-Gomez (Obijuan), Dec 2011
//-- (c) txp666 for esp32, 202503
//-- GPL license
//--------------------------------------------------------------
#include "oscillator.h"

#include <driver/ledc.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>

static const char* TAG = "Oscillator";

extern unsigned long IRAM_ATTR millis();

static ledc_channel_t next_free_channel = LEDC_CHANNEL_0;

Oscillator::Oscillator(int trim) {
    trim_ = trim;
    diff_limit_ = 0;
    is_attached_ = false;
    enabled_ = true;

    sampling_period_ = 30;
    period_ = 2000;
    number_samples_ = period_ / sampling_period_;
    inc_ = 2 * M_PI / number_samples_;

    amplitude_ = 45;
    phase_ = 0;
    phase0_ = 0;
    offset_ = 0;
    stop_ = false;
    rev_ = false;

    pos_ = 90;
    previous_millis_ = 0;
}

Oscillator::~Oscillator() {
    Detach();
}

bool Oscillator::NextSample() {
    current_millis_ = millis();

    if (current_millis_ - previous_millis_ > sampling_period_) {
        previous_millis_ = current_millis_;
        return true;
    }

    return false;
}

void Oscillator::Attach(int pin, bool rev) {
    if (is_attached_) {
        Detach();
    }

    pin_ = pin;
    rev_ = rev;

    ledc_timer_config_t ledc_timer = {
#if !defined(CONFIG_IDF_TARGET_ESP32) && \
    (ESP_IDF_VERSION_MAJOR > 5 || (ESP_IDF_VERSION_MAJOR == 5 && ESP_IDF_VERSION_MINOR >= 0))
        // ESP-IDF 6.0+ 起 ledc_timer_config_t 不再含 speed_mode 字段
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
#else
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
#endif
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    static int last_channel = 0;
    last_channel = (last_channel + 1) % 7 + 1;
    ledc_channel_ = (ledc_channel_t)last_channel;

    ledc_channel_config_t ledc_channel = {
#if !defined(CONFIG_IDF_TARGET_ESP32) && \
    (ESP_IDF_VERSION_MAJOR > 5 || (ESP_IDF_VERSION_MAJOR == 5 && ESP_IDF_VERSION_MINOR >= 0))
        .gpio_num = pin_,
        .channel = ledc_channel_,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
#else
        .gpio_num = pin_,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = ledc_channel_,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
#endif
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    ledc_speed_mode_ = LEDC_LOW_SPEED_MODE;

    // pos_ = 90;
    // Write(pos_);
    previous_servo_command_millis_ = millis();

    is_attached_ = true;
}

bool Oscillator::Attach(int pin, int channel, ledc_timer_bit_t resolution_bits,
                        uint32_t freq_hz, uint32_t timer_num) {
    if (is_attached_) {
        Detach();
    }

    pin_ = pin;
    rev_ = false;

    ledc_timer_config_t ledc_timer = {
#if !defined(CONFIG_IDF_TARGET_ESP32) && \
    (ESP_IDF_VERSION_MAJOR > 5 || (ESP_IDF_VERSION_MAJOR == 5 && ESP_IDF_VERSION_MINOR >= 0))
        .duty_resolution = resolution_bits,
        .timer_num = (ledc_timer_t)timer_num,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
#else
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = resolution_bits,
        .timer_num = (ledc_timer_t)timer_num,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
#endif
    };
    esp_err_t err = ledc_timer_config(&ledc_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %d", err);
        return false;
    }

    ledc_channel_ = (ledc_channel_t)channel;

    ledc_channel_config_t ledc_channel = {
#if !defined(CONFIG_IDF_TARGET_ESP32) && \
    (ESP_IDF_VERSION_MAJOR > 5 || (ESP_IDF_VERSION_MAJOR == 5 && ESP_IDF_VERSION_MINOR >= 0))
        .gpio_num = pin_,
        .channel = ledc_channel_,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = (ledc_timer_t)timer_num,
        .duty = 0,
        .hpoint = 0,
#else
        .gpio_num = pin_,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = ledc_channel_,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = (ledc_timer_t)timer_num,
        .duty = 0,
        .hpoint = 0,
#endif
    };
    err = ledc_channel_config(&ledc_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %d", err);
        return false;
    }

    ledc_speed_mode_ = LEDC_LOW_SPEED_MODE;
    previous_servo_command_millis_ = millis();

    is_attached_ = true;
    enabled_ = true;
    return true;
}

void Oscillator::ConfigureChannel() {
    if (!is_attached_) {
        return;
    }
    // 将当前 pos_ 立即写入 PWM，恢复/刷新通道输出
    uint32_t duty = AngleToDuty((float)pos_);
    ESP_ERROR_CHECK(ledc_set_duty(ledc_speed_mode_, ledc_channel_, duty));
    ESP_ERROR_CHECK(ledc_update_duty(ledc_speed_mode_, ledc_channel_));
}

uint32_t Oscillator::AngleToDuty(float angle) {
    // 将角度 [-90, 90] 映射到舵机脉宽 [500, 2500]us 对应的占空比
    if (angle < SERVO_MIN_DEGREE) angle = SERVO_MIN_DEGREE;
    if (angle > SERVO_MAX_DEGREE) angle = SERVO_MAX_DEGREE;
    float us = (angle - SERVO_MIN_DEGREE) *
                   (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US) /
                   (SERVO_MAX_DEGREE - SERVO_MIN_DEGREE) +
               SERVO_MIN_PULSEWIDTH_US;
    // 20ms 周期下，占空计数值 = us / 20 * (2^resolution - 1)
    uint32_t max_duty = (1u << 13) - 1;  // 13-bit 分辨率
    uint32_t duty = (uint32_t)((us / 20000.0f) * (float)max_duty);
    return duty;
}

void Oscillator::SetAngle(float angle) {
    if (!is_attached_ || !enabled_) {
        return;
    }
    int degrees = (int)(angle + 90.0f + 0.5f);  // 转为 [0, 180]
    degrees = std::min(std::max(degrees, 0), 180);
    pos_ = degrees;
    int adjusted = degrees + trim_;
    adjusted = std::min(std::max(adjusted, 0), 180);
    uint32_t duty = AngleToDuty((float)(adjusted - 90));
    ESP_ERROR_CHECK(ledc_set_duty(ledc_speed_mode_, ledc_channel_, duty));
    ESP_ERROR_CHECK(ledc_update_duty(ledc_speed_mode_, ledc_channel_));
}

void Oscillator::SetEnable(bool enable) {
    enabled_ = enable;
    if (!enable && is_attached_) {
        // 禁用时输出中位（90°），避免舵机保持受力
        uint32_t duty = AngleToDuty(0.0f);
        ESP_ERROR_CHECK(ledc_set_duty(ledc_speed_mode_, ledc_channel_, duty));
        ESP_ERROR_CHECK(ledc_update_duty(ledc_speed_mode_, ledc_channel_));
    }
}

void Oscillator::Detach() {
    if (!is_attached_)
        return;

    ESP_ERROR_CHECK(ledc_stop(ledc_speed_mode_, ledc_channel_, 0));

    is_attached_ = false;
}

void Oscillator::SetT(unsigned int T) {
    period_ = T;

    number_samples_ = period_ / sampling_period_;
    inc_ = 2 * M_PI / number_samples_;
}

void Oscillator::SetPosition(int position) {
    Write(position);
}

void Oscillator::Refresh() {
    if (NextSample()) {
        if (!stop_) {
            int pos = std::round(amplitude_ * std::sin(phase_ + phase0_) + offset_);
            if (rev_)
                pos = -pos;
            Write(pos + 90);
        }

        phase_ = phase_ + inc_;
    }
}

void Oscillator::Write(int position) {
    if (!is_attached_)
        return;

    long currentMillis = millis();
    if (diff_limit_ > 0) {
        int limit = std::max(
            1, (((int)(currentMillis - previous_servo_command_millis_)) * diff_limit_) / 1000);
        if (abs(position - pos_) > limit) {
            pos_ += position < pos_ ? -limit : limit;
        } else {
            pos_ = position;
        }
    } else {
        pos_ = position;
    }
    previous_servo_command_millis_ = currentMillis;

    int angle = pos_ + trim_;

    angle = std::min(std::max(angle, 0), 180);

    uint32_t duty = (uint32_t)(((angle / 180.0) * 2.0 + 0.5) * 8191 / 20.0);

    ESP_ERROR_CHECK(ledc_set_duty(ledc_speed_mode_, ledc_channel_, duty));
    ESP_ERROR_CHECK(ledc_update_duty(ledc_speed_mode_, ledc_channel_));
}

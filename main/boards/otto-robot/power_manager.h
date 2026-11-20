#ifndef __POWER_MANAGER_H__
#define __POWER_MANAGER_H__

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_timer.h>

class PowerManager {
private:
    // 电池电量区间-分压电阻为2个100k
    static constexpr struct {
        uint16_t adc;
        uint8_t level;
    } BATTERY_LEVELS[] = {{2050, 0}, {2450, 100}};
    static constexpr size_t BATTERY_LEVELS_COUNT = 2;
    static constexpr size_t ADC_VALUES_COUNT = 10;

    esp_timer_handle_t timer_handle_ = nullptr;
    gpio_num_t charging_pin_;
    adc_unit_t adc_unit_;
    adc_channel_t adc_channel_;
    uint16_t adc_values_[ADC_VALUES_COUNT];
    size_t adc_values_index_ = 0;
    size_t adc_values_count_ = 0;
    uint8_t battery_level_ = 100;
    bool is_charging_ = false;
    inline static bool battery_update_paused_ = false;  // 静态标志：是否暂停电量更新

    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    // 无 ADC 输入时的安全模式：true 表示不做采样，电量固定返回 100%
    bool safe_mode_ = false;

    void CheckBatteryStatus() {
      // 如果电量更新被暂停（动作进行中），则跳过更新
      if (battery_update_paused_) {
        return;
      }

      // 安全模式（无 ADC 输入）下，保持电量 100%、非充电
      if (safe_mode_) {
        battery_level_ = 100;
        is_charging_ = false;
        return;
      }

      ReadBatteryAdcData();

      if (charging_pin_ == GPIO_NUM_NC) {
        is_charging_ = false;
      } else {
        is_charging_ = gpio_get_level(charging_pin_) == 0;
      }
    }

    void ReadBatteryAdcData() {
        if (adc_handle_ == nullptr) {
            return;
        }
        int adc_value;
        esp_err_t ret = adc_oneshot_read(adc_handle_, adc_channel_, &adc_value);
        if (ret != ESP_OK) {
            ESP_LOGW("PowerManager", "ADC 读取失败: %s，跳过本次采样", esp_err_to_name(ret));
            return;
        }

        adc_values_[adc_values_index_] = (uint16_t)adc_value;
        adc_values_index_ = (adc_values_index_ + 1) % ADC_VALUES_COUNT;
        if (adc_values_count_ < ADC_VALUES_COUNT) {
            adc_values_count_++;
        }

        uint32_t average_adc = 0;
        for (size_t i = 0; i < adc_values_count_; i++) {
            average_adc += adc_values_[i];
        }
        average_adc /= adc_values_count_;

        CalculateBatteryLevel(average_adc);
    }

    void CalculateBatteryLevel(uint32_t average_adc) {
        if (average_adc <= BATTERY_LEVELS[0].adc) {
            battery_level_ = 0;
        } else if (average_adc >= BATTERY_LEVELS[BATTERY_LEVELS_COUNT - 1].adc) {
            battery_level_ = 100;
        } else {
            float ratio = static_cast<float>(average_adc - BATTERY_LEVELS[0].adc) /
                          (BATTERY_LEVELS[1].adc - BATTERY_LEVELS[0].adc);
            battery_level_ = (uint8_t)(ratio * 100);
        }
    }

public:
    /**
     * @brief 构造电量管理器
     * @param charging_pin   充电检测引脚，GPIO_NUM_NC 表示未连接
     * @param adc_unit       ADC 单元（仅在有真实采样输入时有效）
     * @param adc_channel    ADC 通道（仅在有真实采样输入时有效）
     * @param enable_adc      是否启用 ADC 采样。false 时进入安全模式，电量恒返回 100%，
     *                        用于没有电池电压分压输入的板型，避免悬空 ADC 引脚触发低电量逻辑
     */
    PowerManager(gpio_num_t charging_pin, adc_unit_t adc_unit = ADC_UNIT_2,
                 adc_channel_t adc_channel = ADC_CHANNEL_3, bool enable_adc = true)
        : charging_pin_(charging_pin), adc_unit_(adc_unit), adc_channel_(adc_channel),
          safe_mode_(!enable_adc) {

      if (charging_pin_ != GPIO_NUM_NC) {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << charging_pin_);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
        ESP_LOGI("PowerManager", "充电检测引脚配置完成: GPIO%d", charging_pin_);
      } else {
        ESP_LOGI("PowerManager", "充电检测引脚未配置，不进行充电状态检测");
      }

      if (safe_mode_) {
        ESP_LOGW("PowerManager",
                 "ADC 采样未启用（无电池电压输入），进入安全模式：电量恒为 100%，"
                 "低电量弹窗将被屏蔽。如确需电量检测，请接入分压电路并将 enable_adc 置 true");
        battery_level_ = 100;
        is_charging_ = false;
        return;
      }

      // 仅在有真实采样输入时才初始化定时器
      esp_timer_create_args_t timer_args = {
          .callback =
              [](void* arg) {
                  PowerManager* self = static_cast<PowerManager*>(arg);
                  self->CheckBatteryStatus();
              },
          .arg = this,
          .dispatch_method = ESP_TIMER_TASK,
          .name = "battery_check_timer",
          .skip_unhandled_events = true,
      };
      ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
      ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));  // 1秒

      InitializeAdc();
    }

    void InitializeAdc() {
      adc_oneshot_unit_init_cfg_t init_config = {
          .unit_id = adc_unit_,
          .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
          .ulp_mode = ADC_ULP_MODE_DISABLE,
      };
      ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

      adc_oneshot_chan_cfg_t chan_config = {
          .atten = ADC_ATTEN_DB_12,
          .bitwidth = ADC_BITWIDTH_12,
      };

      ESP_ERROR_CHECK(
          adc_oneshot_config_channel(adc_handle_, adc_channel_, &chan_config));
    }

    ~PowerManager() {
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
            timer_handle_ = nullptr;
        }
        if (adc_handle_) {
            adc_oneshot_del_unit(adc_handle_);
            adc_handle_ = nullptr;
        }
    }

    bool IsCharging() { return is_charging_; }

    uint8_t GetBatteryLevel() { return battery_level_; }

    bool IsSafeMode() const { return safe_mode_; }

    // 暂停/恢复电量更新（用于动作执行时屏蔽更新）
    static void PauseBatteryUpdate() { battery_update_paused_ = true; }
    static void ResumeBatteryUpdate() { battery_update_paused_ = false; }
};
#endif  // __POWER_MANAGER_H__

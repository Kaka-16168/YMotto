#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "application.h"
#include "board.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "display.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "lcd_display.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "otto_controller.h"
#include "system_reset.h"
#include "wifi_board.h"

static const char* TAG = "OttoRobot";

/*
 * OttoRobotBoard —— ESP32-S3-N16R8 上的小智 AI + Otto 机器人主控板
 *
 * 实现要点：
 *   1. 用板载 BOOT 键做系统复位（长按 recovery）与普通按键两种用途
 *   2. 屏幕走 SPI3，背光 GPIO38（共阳，低电平点亮）
 *   3. 6 路 SG90 舵机走 LEDC，50Hz，14 位分辨率
 *   4. 关闭电池图标与低电量弹窗（开发板无电量检测）
 *   5. 唤醒词走 MultiNet 自定义路线：卡卡 / ka ka，关闭官方 Hi,ESP 等
 */

class OttoRobotBoard : public WifiBoard {
public:
    OttoRobotBoard();
    virtual ~OttoRobotBoard();

    void Initialize() override;
    void Start() override;

    /* 电源与电池：开发板无检测，一律返回 false */
    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        level      = 100;
        charging   = false;
        discharging = false;
        return false;
    }

    /* 低电量提示：强制关闭，永不弹窗 */
    bool ShouldShowLowBattery() const override { return false; }
    bool ShouldShowBatteryIcon() const override { return false; }

    void SetPowerSaveMode(bool enabled) override {
        ESP_LOGI(TAG, "PowerSaveMode %s（本板型不支持，忽略）",
                 enabled ? "on" : "off");
    }

protected:
    virtual std::string GetBoardType() const override { return "otto-robot"; }

private:
    void InitializeSpi();
    void InitializeLcdDisplay();
    void InitializeButtons();
    void InitializeOtto();
    void InitializeMcpTools();

    void OnButtonEvent(Button* button, Button::PressType type);

    /* 硬件句柄 */
    SpiLcdDisplay*    display_   = nullptr;
    SingleLed*        led_       = nullptr;
    OttoController    otto_;
    Button            boot_btn_{BOOT_BUTTON_GPIO, true};
    Button            vol_up_{VOLUME_UP_BUTTON_GPIO, true};
    Button            vol_down_{VOLUME_DOWN_BUTTON_GPIO, true};

    bool              initialized_ = false;
};

OttoRobotBoard::OttoRobotBoard() = default;

OttoRobotBoard::~OttoRobotBoard() = default;

void OttoRobotBoard::InitializeSpi() {
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.sclk_io_num = DISPLAY_CLK_PIN;
    buscfg.quadwp_io_num  = GPIO_NUM_NC;
    buscfg.quadhd_io_num  = GPIO_NUM_NC;
    buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    buscfg.flags           = SPICOMMON_BUSFLAG_MASTER;
    esp_err_t err = spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI%d 初始化失败: %s", DISPLAY_SPI_HOST, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SPI%d 总线就绪", DISPLAY_SPI_HOST);
    }
}

void OttoRobotBoard::InitializeLcdDisplay() {
    ESP_LOGD(TAG, "安装 LCD panel IO");
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t    panel    = nullptr;

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = DISPLAY_CS_PIN;
    io_config.dc_gpio_num = DISPLAY_DC_PIN;
    io_config.spi_mode    = DISPLAY_SPI_MODE;
    io_config.pclk_hz     = DISPLAY_SPI_CLOCK_HZ;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits  = 8;
    io_config.lcd_param_bits = 8;
    io_config.flags.dc_high_on_cmd  = 0;
    io_config.flags.dc_low_on_data   = 0;
    io_config.flags.dc_low_on_param  = 0;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io));

    ESP_LOGD(TAG, "安装 ST7789 驱动");
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = DISPLAY_RST_PIN;
    panel_config.rgb_ele_order  = DISPLAY_RGB_ORDER;
    panel_config.bits_per_pixel = 16;
    panel_config.flags.reset_active_high = 0;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    display_ = new SpiLcdDisplay(panel_io, panel,
                                 DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                 DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                 DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                 DISPLAY_SWAP_XY);

    /* 背光：共阳 LED，低电平点亮，用 LEDC 调光 */
    if (DISPLAY_BACKLIGHT_PIN >= 0) {
        ledc_timer_config_t tc = {};
        tc.duty_resolution = LEDC_TIMER_8_BIT;
        tc.freq_hz         = 1000;
        tc.speed_mode      = LEDC_LOW_SPEED_MODE;
        tc.timer_num       = LEDC_TIMER_1;
        tc.clk_cfg         = LEDC_AUTO_CLK;
        ledc_timer_config(&tc);

        ledc_channel_config_t cc = {};
        cc.gpio_num   = static_cast<gpio_num_t>(DISPLAY_BACKLIGHT_PIN);
        cc.speed_mode = LEDC_LOW_SPEED_MODE;
        cc.channel    = LEDC_CHANNEL_6;
        cc.intr_type  = LEDC_INTR_DISABLE;
        cc.timer_sel  = LEDC_TIMER_1;
        cc.duty       = DISPLAY_BACKLIGHT_OUTPUT_INVERT
                            ? 255 - DISPLAY_BACKLIGHT_INIT_PERCENT
                            : DISPLAY_BACKLIGHT_INIT_PERCENT;
        cc.hpoint     = 0;
        ledc_channel_config(&cc);
        ESP_LOGI(TAG, "背光 GPIO%d 已启用 (%d%%)", DISPLAY_BACKLIGHT_PIN,
                 DISPLAY_BACKLIGHT_INIT_PERCENT);
    }

    /* 关闭电池图标与低电量弹窗 —— 硬件无检测，两项全部禁用 */
    display_->SetBatteryIconVisible(false);
    display_->SetLowBatteryPopupEnabled(false);

    ESP_LOGI(TAG, "ST7789 显示初始化完成 %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

void OttoRobotBoard::InitializeButtons() {
    boot_btn_.OnPressDown([this](Button*) {
        Application::GetInstance().ToggleChatState();
    });
    boot_btn_.OnLongPress([this](Button*) {
        ESP_LOGI(TAG, "BOOT 长按：触发系统复位");
        SystemReset();
    });

    vol_up_.OnClick([this](Button*) {
        Application::GetInstance().IncreaseVolume();
    });
    vol_down_.OnClick([this](Button*) {
        Application::GetInstance().DecreaseVolume();
    });

    ESP_LOGI(TAG, "按键初始化完成（BOOT=GPIO%d VOL+=GPIO%d VOL-=GPIO%d）",
             BOOT_BUTTON_GPIO, VOLUME_UP_BUTTON_GPIO, VOLUME_DOWN_BUTTON_GPIO);
}

void OttoRobotBoard::InitializeOtto() {
    if (!otto_.Init()) {
        ESP_LOGE(TAG, "Otto 控制器初始化失败");
        return;
    }
    ESP_LOGI(TAG, "Otto 机器人控制器就绪");
}

void OttoRobotBoard::InitializeMcpTools() {
    /* 注册动作工具 self.otto.action */
    auto otto_action = std::make_shared<McpTool>(
        "self.otto.action",
        "让 Otto 机器人执行一个动作：walk/turn/sit/swing/hand_wave/greeting/home",
        PropertyList({
            Property("action", kPropertyTypeString, true),
            Property("steps",    kPropertyTypeNumber, false),
            Property("speed",    kPropertyTypeNumber, false),
            Property("amount",   kPropertyTypeNumber, false),
            Property("direction",kPropertyTypeNumber, false),
        }));
    otto_action->SetCallback([this](const PropertyList& props) -> ReturnValue {
        auto action = props["action"].value;
        auto steps  = props["steps"].value.toInt();
        auto speed  = props["speed"].value.toInt();
        auto amount = props["amount"].value.toInt();
        auto dir    = props["direction"].value.toInt();

        OttoController::Action act = OttoController::Action::kHome;
        bool known = true;
        if      (action == "walk")       act = OttoController::Action::kWalk;
        else if (action == "turn")      act = OttoController::Action::kTurn;
        else if (action == "sit")       act = OttoController::Action::kSit;
        else if (action == "swing")     act = OttoController::Action::kSwing;
        else if (action == "hand_wave") act = OttoController::Action::kHandWave;
        else if (action == "greeting")  act = OttoController::Action::kGreeting;
        else if (action == "home")      act = OttoController::Action::kHome;
        else known = false;

        if (!known) {
            return ReturnValue::Failure("未知动作: " + action +
                "，仅支持 walk/turn/sit/swing/hand_wave/greeting/home");
        }
        otto_.Enqueue(act, steps, speed, amount, dir);
        return ReturnValue::Success("已安排动作: " + action);
    });
    McpServer::GetInstance().AddTool(otto_action);

    /* 停止工具 */
    auto otto_stop = std::make_shared<McpTool>(
        "self.otto.stop", "立即停止 Otto 机器人当前动作并归位", PropertyList({}));
    otto_stop->SetCallback([this](const PropertyList&) -> ReturnValue {
        otto_.Stop();
        return ReturnValue::Success("已停止");
    });
    McpServer::GetInstance().AddTool(otto_stop);

    /* 状态查询 */
    auto otto_status = std::make_shared<McpTool>(
        "self.otto.get_status", "获取 Otto 机器人当前状态", PropertyList({}));
    otto_status->SetCallback([this](const PropertyList&) -> ReturnValue {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "{\"state\":\"%s\",\"ready\":%s}",
            otto_.GetStateString(),
            otto_.IsReady() ? "true" : "false");
        return ReturnValue::Success(std::string(buf));
    });
    McpServer::GetInstance().AddTool(otto_status);

    /* 校准微调 */
    auto otto_trim = std::make_shared<McpTool>(
        "self.otto.set_trim",
        "设置某个关节的微调值（-50~50）",
        PropertyList({
            Property("joint", kPropertyTypeString, true),
            Property("trim",  kPropertyTypeNumber, true),
        }));
    otto_trim->SetCallback([this](const PropertyList& props) -> ReturnValue {
        std::string joint = props["joint"].value;
        int8_t trim = static_cast<int8_t>(props["trim"].value.toInt());
        if (otto_.SetTrim(joint, trim)) {
            return ReturnValue::Success("trim 已更新");
        }
        return ReturnValue::Success("未知关节: " + joint);
    });
    McpServer::GetInstance().AddTool(otto_trim);

    /* 电量查询：返回 -1 表示不可用 */
    auto battery = std::make_shared<McpTool>(
        "self.battery.get_level", "获取电池电量（-1 表示本板型无电量检测）",
        PropertyList({}));
    battery->SetCallback([](const PropertyList&) -> ReturnValue {
        return ReturnValue::Success("{\"level\":-1,\"available\":false}");
    });
    McpServer::GetInstance().AddTool(battery);

    ESP_LOGI(TAG, "MCP 工具注册完成（otto.action/stop/get_status/set_trim/battery）");
}

void OttoRobotBoard::Initialize() {
    if (initialized_) return;
    initialized_ = true;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Otto Robot (ESP32-S3-N16R8) 启动中");
    ESP_LOGI(TAG, "  Flash=16MB  PSRAM=8MB(OPI)  SDK=6.1");
    ESP_LOGI(TAG, "  唤醒词=卡卡(ka ka)  官方唤醒词已关闭");
    ESP_LOGI(TAG, "========================================");

    spi_flash_init();

    InitializeSpi();
    InitializeLcdDisplay();
    InitializeButtons();
    InitializeOtto();

    /* 音频编解码：INMP441 + MAX98357A，单线 I2S */
    auto codec = std::make_unique<NoAudioCodec>(
        AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_WS, AUDIO_I2S_SPK_GPIO_DOUT,
        AUDIO_I2S_MIC_GPIO_SCK,  AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
    SetAudioCodec(std::move(codec));

    led_ = new SingleLed(BUILTIN_LED_GPIO);
    SetLed(led_);

    SetDisplay(display_);

    InitializeMcpTools();
}

void OttoRobotBoard::Start() {
    WifiBoard::Start();
    ESP_LOGI(TAG, "Otto Robot 启动完成");
}

DECLARE_BOARD(OttoRobotBoard);

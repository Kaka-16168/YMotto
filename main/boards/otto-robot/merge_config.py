#!/usr/bin/env python3
"""
合并构建配置脚本

功能：把 config.custom-wake.json 的内容合并进 config.json，
      让 CI 构建时使用自定义唤醒词"卡卡/kaka"，而非默认的内置词。

用法：
    python3 merge_config.py

流程：
    1. 读取 config.json
    2. 读取 config.custom-wake.json（存在则合并）
    3. 把唤醒词相关配置追加到 sdkconfig_append
    4. 写回 config.json
"""

import json
import os
import sys

BOARD_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(BOARD_DIR, "config.json")
CUSTOM_WAKE_PATH = os.path.join(BOARD_DIR, "config.custom-wake.json")


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def save_json(path, data):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")


def merge_config():
    if not os.path.exists(CONFIG_PATH):
        print(f"[ERROR] 找不到 config.json: {CONFIG_PATH}")
        sys.exit(1)

    config = load_json(CONFIG_PATH)
    print(f"[OK] 已读取 config.json")

    # 记录原始唤醒词配置
    original_wake = config.get("wake_word", "未设置")
    original_wake_words = config.get("wake_words", [])
    print(f"     原始 wake_word: {original_wake}")
    print(f"     原始 wake_words: {original_wake_words}")

    # 读取自定义唤醒词配置
    if os.path.exists(CUSTOM_WAKE_PATH):
        custom = load_json(CUSTOM_WAKE_PATH)
        print(f"[OK] 已读取 config.custom-wake.json")

        # 1. 更新顶层 wake_word 字段
        if "wake_word" in custom:
            config["wake_word"] = custom["wake_word"]
            print(f"     → 更新 wake_word = {custom['wake_word']}")

        # 2. 更新 wake_words 数组
        if "wake_words" in custom:
            config["wake_words"] = custom["wake_words"]
            print(f"     → 更新 wake_words = {custom['wake_words']}")

        # 3. 更新自定义唤醒词短语
        if "custom_wake_phrases" in custom:
            config["custom_wake_phrases"] = custom["custom_wake_phrases"]
            print(f"     → 更新 custom_wake_phrases = {custom['custom_wake_phrases']}")

        # 4. 追加到 sdkconfig_append，确保构建时启用自定义唤醒词
        sdkconfig_append = config.get("sdkconfig_append", [])
        if not isinstance(sdkconfig_append, list):
            sdkconfig_append = []

        # 唤醒词相关的 sdkconfig 项
        wake_sdkconfig = [
            "CONFIG_USE_CUSTOM_WAKE_WORD=y",
            "CONFIG_USE_ESP_WAKE_WORD=n",
            "CONFIG_USE_AFE_WAKE_WORD=n",
            "CONFIG_WAKE_WORD_DISABLED=n",
        ]

        # 如果配置指定了自定义模型，添加模型选择
        if custom.get("wake_word") == "kaka":
            wake_sdkconfig.append('CONFIG_CUSTOM_WAKE_WORD_MODEL="kaka"')

        # 只追加不存在的项
        for item in wake_sdkconfig:
            if item not in sdkconfig_append:
                sdkconfig_append.append(item)

        config["sdkconfig_append"] = sdkconfig_append
        print(f"     → 已追加 {len(wake_sdkconfig)} 项 sdkconfig 配置")
        for item in wake_sdkconfig:
            print(f"       {item}")
    else:
        print(f"[WARN] 找不到 config.custom-wake.json，跳过合并")
        print(f"       路径: {CUSTOM_WAKE_PATH}")

    # 写回
    save_json(CONFIG_PATH, config)
    print(f"\n[OK] 配置已合并写回: {CONFIG_PATH}")
    print(f"     最终 wake_word: {config.get('wake_word')}")
    print(f"     最终 wake_words: {config.get('wake_words')}")


if __name__ == "__main__":
    merge_config()

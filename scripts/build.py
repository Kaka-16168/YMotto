#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
XiaoZhi ESP32 Build Script
Modified to force Custom Wake Word & Chinese Language after sdkconfig_append processing.
"""

import os
import sys
import json
import zipfile
import subprocess

def get_board_type():
    """从 main/CMakeLists.txt 解析当前板型"""
    try:
        with open("main/CMakeLists.txt", "r") as f:
            lines = f.readlines()
            for line in lines:
                if "set(BOARD_TYPE" in line and '"' in line:
                    return line.split('"')[1]
    except Exception:
        pass
    return None

def get_project_version():
    """获取项目版本"""
    try:
        with open("main/CMakeLists.txt", "r") as f:
            for line in f:
                if "set(PROJECT_VER" in line and '"' in line:
                    return line.split('"')[1]
    except Exception:
        pass
    return "unknown"

def zip_bin(board_type, project_version):
    """打包二进制文件"""
    zip_name = f"xiaozhi_{board_type}_{project_version}.zip"
    print(f"正在打包: {zip_name}")
    with zipfile.ZipFile(zip_name, 'w', zipfile.ZIP_DEFLATED) as zipf:
        if os.path.exists("build/merged-binary.bin"):
            zipf.write("build/merged-binary.bin")
        else:
            print("警告: 没有找到 build/merged-binary.bin")
    return zip_name

def release(board_type, board_config):
    config_path = f"main/boards/{board_type}/config.json"
    if not os.path.exists(config_path):
        print(f"跳过 {board_type} 因为 config.json 不存在")
        return

    with open(config_path, "r", encoding="utf-8") as f:
        config = json.load(f)

    target = config.get("target", "esp32s3")
    builds = config["builds"]

    for build in builds:
        name = build["name"]
        # 原有逻辑：首先加入板型配置
        sdkconfig_append = [f"{board_config}=y"]
        
        # 追加 config.json 中的 sdkconfig_append
        for append in build.get("sdkconfig_append", []):
            sdkconfig_append.append(append)

        # ==================== 核心修改开始：大清洗 ====================
        print("--- 正在清理冲突配置并强制注入卡卡唤醒词 ---")
        
        # 需要彻底移除的互斥旧配置前缀
        ignore_prefixes = (
            "CONFIG_WAKE_WORD_DISABLED",
            "CONFIG_USE_ESP_WAKE_WORD",
            "CONFIG_USE_AFE_WAKE_WORD",
            "CONFIG_USE_CUSTOM_WAKE_WORD",
            "CONFIG_CUSTOM_WAKE_WORD",
            "CONFIG_SR_WN_",           # 清除所有旧 Wakenet 模型
            "CONFIG_LANGUAGE_",        # 清除旧语言设置
        )
        
        # 过滤掉 config.json 里带有的旧配置
        sdkconfig_append = [
            item for item in sdkconfig_append
            if not item.startswith(ignore_prefixes)
        ]
        
        # 强制写入唯一的卡卡配置和中文
        sdkconfig_append.extend([
            "CONFIG_LANGUAGE_ZH_CN=y",
            "CONFIG_WAKE_WORD_DISABLED=n",
            "CONFIG_USE_ESP_WAKE_WORD=n",
            "CONFIG_USE_AFE_WAKE_WORD=n",
            "CONFIG_USE_CUSTOM_WAKE_WORD=y",
            'CONFIG_CUSTOM_WAKE_WORD="ka ka"',
            'CONFIG_CUSTOM_WAKE_WORD_DISPLAY="卡卡"'
        ])
        # ==================== 核心修改结束 ====================

        print(f"name: {name}")
        print(f"target: {target}")
        for append in sdkconfig_append:
            print(f"sdkconfig_append: {append}")

        # 设置 target
        os.environ.pop("IDF_TARGET", None)
        if os.system(f"idf.py set-target {target}") != 0:
            print("set-target failed")
            sys.exit(1)

        # 将最终配置追加到 sdkconfig
        with open("sdkconfig", "a", encoding="utf-8") as f:
            f.write("\n")
            for append in sdkconfig_append:
                f.write(f"{append}\n")

        # 编译打包
        if os.system("idf.py merge-bin") != 0:
            print("merge-bin failed")
            sys.exit(1)

        project_version = get_project_version()
        zip_bin(name, project_version)
        print("-" * 80)

def get_all_board_types():
    """获取所有板型"""
    board_configs = {}
    with open("main/CMakeLists.txt", "r") as f:
        lines = f.readlines()
    for i, line in enumerate(lines):
        if "if(CONFIG_BOARD_TYPE_" in line:
            config_name = line.strip().split("if(")[1].split(")")[0]
            next_line = lines[i + 1].strip()
            if next_line.startswith("set(BOARD_TYPE"):
                board_type = next_line.split('"')[1]
                board_configs[config_name] = board_type
    return board_configs

if __name__ == "__main__":
    if len(sys.argv) > 1:
        board_configs = get_all_board_types()
        found = False
        for board_config, board_type in board_configs.items():
            # 跳过不需要编译的特殊板型
            if board_type == 'main/boards/bread-compact-wifi-lcd':
                continue
            if sys.argv[1] == 'all' or board_type == sys.argv[1]:
                found = True
                release(board_type, board_config)
        if not found:
            print(f"未找到板型: {sys.argv[1]}")
    else:
        print("用法: python scripts/build.py <board-directory>")

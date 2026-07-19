# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}) # 添加当前目录到BOARD_ROOT
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(blinky_test)

target_include_directories(app PRIVATE src)
target_include_directories(app PRIVATE src/driver)
target_include_directories(app PRIVATE src/modbus)


target_sources(app PRIVATE src/main.c)

target_sources(app PRIVATE src/driver/adc.c)
target_sources(app PRIVATE src/driver/can_driver.c)
target_sources(app PRIVATE src/driver/fs_storage.c)
target_sources(app PRIVATE src/driver/iic_board.c)
target_sources(app PRIVATE src/driver/modbus_driver.c)
target_sources(app PRIVATE src/driver/uart.c)
target_sources(app PRIVATE src/driver/w25qxx.c)

target_sources(app PRIVATE src/modbus/modbus_ser_a.c)
target_sources(app PRIVATE src/modbus/modbus_cli_a.c)

target_sources(app PRIVATE src/gpio-ctr.c)
target_sources(app PRIVATE src/sys_param.c)
target_sources(app PRIVATE src/oled1306.c)



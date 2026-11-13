/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
#pragma once

#include <stdbool.h>

/* Switch the Tab5's C6 supply on through the I2C IO expander. */
bool tab5_c6_power_on(void);

/* Release the panel reset and switch the backlight on. */
bool tab5_lcd_power_on(void);

/* Enable the microSD slot's I/O rail (on-chip LDO channel 4, 3.3 V). */
bool tab5_sd_power_on(void);

/* Clock UART1 and route it to the RS485 transceiver (TX 20, RX 21, DIR 34). */
bool tab5_rs485_uart_init(void);
/* UART2 clocks for the Grove port (pads are routed by Linux, grove-mode). */
bool tab5_grove_uart_init(void);

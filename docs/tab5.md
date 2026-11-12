# M5Stack Tab5 (work in progress)

The Tab5 has the same ESP32-P4 (NRW32: 32 MiB PSRAM, 16 MiB flash) and an
ESP32-C6 on SDIO, so it runs this image with a different boot shim and a few
DTS changes. `EASYSTICK_BOARD=tab5` selects them; the `tab5-build` workflow
builds everything below.

**Status (2026-10-18): Linux runs on the Tab5 with Wi-Fi/SSH, a console on
the display and input from the Tab5 Keyboard.** A root shell on tty1 takes
`uname -a` typed on the keyboard and shows the output on the panel.

- Display: WHY2025's ESP32-P4 DSI glue (0003, 0073) with 0074 (DPI clock
  from the mode, GDMA scanning the DRM framebuffer directly) and an ST7123
  panel driver (0075); fbcon rotated to landscape, Terminus 16x32, 80x22.
  The framebuffer lives in a 2 MB `shared-dma-pool`, and the DSI node is
  `dma-coherent` (the cache ops make devices non-coherent, and NOMMU has no
  uncached remap).
- Keyboard: 0076, HID mode over a bit-banged I2C bus on GPIO0/1. Register
  reads need STOP between the register write and the read.
- Backlight: GPIO22 is hogged in the DTS; the P4 GPIO driver otherwise
  clears its output enable whenever the I2C bus changes SDA's direction.
- `tools/tab5-kernel-build.sh` rebuilds the kernel from the CI kernel kit
  for quick driver iterations.

Stage 1 (headless) notes follow.

Checked on one Tab5 (P4 rev v1.3, ST7123 panel/touch) with the `tab5-build`
image of `1b9718c` (`m5stamp-p4-c6-linux-smp-v0.0.1-16mb.bin`, SHA-256
`9ac02212397eac0bedf54e122210a4f451daf4e9a312292722829e7ae722e2ff`) after the
C6 OTA below: `tab5-power: C6 power on (expander 0x44 id 0xa0, WLAN_PWR_EN)`,
`Machine model: M5Stack Tab5`, `mmc0: new SDIO card at address 0001`,
`ESP-Hosted Version: NG-1.0.6.0.1`, `wlan0` up as SoftAP on 192.168.4.1,
Dropbear started, `[bootsync] ownership=no-udc`, root shell on the USB-C
console. SSH over the SoftAP was not tried yet.

## What differs from the Stamp-P4

| | Stamp-P4 + AddOn C6 | Tab5 |
| --- | --- | --- |
| C6 SDIO (Slot 1, GPIO matrix) | CLK 43, CMD 44, DAT0..3 45..48 | CLK 12, CMD 13, DAT0..3 11/10/9/8 |
| C6 EN | GPIO42 | GPIO15 (`SOC_EXTRF_RST`) |
| C6 supply | always on | load switch, `WLAN_PWR_EN` = P0 of the PI4IOE5V6408 at I2C 0x44 (SDA 31, SCL 32) |
| C6 antenna | — | `RF_PTH_L_INT_H_EXT` = P0 of the expander at 0x43, low = internal |
| USB-C | USB-Serial/JTAG (GPIO24/25) | the same: console and flashing |
| GPIO26/27 | FS OTG, USB-A drive | I2S to the codec: no USB drive |

Sources: the [Tab5 schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Schematics_PDF.pdf)
and ESP-Hosted-MCU's `ESP32P4_TAB5_C6_BOARD` Kconfig option.

The C6 EN is wired as the schematic shows: GPIO15 → 1 k → EN with a 10 k
pull-up, low = reset. ESP-Hosted-MCU's Tab5 board option defaults to the
opposite polarity; with it the C6 stays in reset (`send_op_cond (1) returned
0x107`), and with an active-high reset it answers (checked 2026-10-18, see
below). The boot shim drives EN low, then high, then releases it, so it
works with either polarity, and the OTA host forces the active-high reset.

There is no USB drive on the Tab5. Read the Wi-Fi and SSH credentials from
the `ES_CREDENTIALS NEW` line on the serial console at the first boot.
`easystick-bootsync` only saves `/boot` changes that a USB host made, so on
the Tab5 an edit of `/boot/config.txt` is not saved to flash yet; stage 1
runs with the defaults (SoftAP).

## C6 firmware

Checked on 2026-10-18 with a read-only P4 probe (ESP-Hosted-MCU v2.13.0 host,
active-high reset): the C6 identifies as `esp32c6`, reports ESP-Hosted-MCU
firmware **1.4.1**, capabilities `0xd` (WLAN, BLE over SDIO) and no transport
size, so the host warns `Version mismatch: Host [2.13.0] > Co-proc [0.0.0]`.

The Linux driver needs ESP-Hosted-NG 1.0.6 on the C6. The Tab5 ships its C6
with an ESP-Hosted-MCU slave, so the C6 has to be updated once. Like the
Stamp-AddOn, the Tab5 C6 has no USB port; its UART is only on the internal
`C6_ISP` header. The `tab5-build` workflow therefore also builds the
temporary P4 OTA host (`ota-host/`), which sends `network_adapter.bin` to the
C6 over SDIO with ESP-Hosted-MCU's host-performed OTA.

After that update the factory Tab5 firmware has no Wi-Fi until the C6 gets an
ESP-Hosted-MCU slave image again (through the same OTA host built with that
image, or the `C6_ISP` header).

The OTA was run on 2026-10-18: the host sent `NG-1.0.6.0.1` (1040848 bytes)
and logged `Partition OTA completed successfully`; firmware 1.4.1 has no
activate call (`requires v2.6.0+`), and the C6 came up with the NG image on
the next reset. The OTA host then loops (it speaks ESP-Hosted-MCU to an NG
C6), so flash the Linux image right after it reports success.

## Flashing from macOS

Use esptool 4.8.1 (`versions.lock.json`; 5.3.1 lost the USB-Serial/JTAG
response during long reads on the Stamp-P4). The port is
`/dev/cu.usbmodem*`.

```bash
python3 -m venv ~/esptool481 && ~/esptool481/bin/pip install esptool==4.8.1 pyserial
PORT=$(ls /dev/cu.usbmodem* | head -n1)
ESPTOOL="$HOME/esptool481/bin/esptool.py --chip esp32p4 --port $PORT"

# 1. Keep the factory image.  Two reads must match.
$ESPTOOL --baud 921600 read_flash 0 0x1000000 tab5-stock-1.bin
$ESPTOOL --baud 921600 read_flash 0 0x1000000 tab5-stock-2.bin
cmp tab5-stock-1.bin tab5-stock-2.bin && shasum -a 256 tab5-stock-1.bin

# 2. Update the C6: flash the OTA host, then watch it send the image.
cd tab5-assets/ota-host
$ESPTOOL --baud 460800 write_flash $(grep -v network_adapter flash_args)
# esptool refuses a C6 image in a P4 write without --force
$ESPTOOL --baud 460800 write_flash --force 0x5f0000 network_adapter.bin
python3 -m serial.tools.miniterm "$PORT" 115200   # wait for the OTA to finish

# 3. Flash the Linux image.
cd ..
gzip -d m5stamp-p4-c6-linux-smp-*-16mb.bin.gz
$ESPTOOL --baud 460800 write_flash 0x0 m5stamp-p4-c6-linux-smp-*-16mb.bin
python3 -m serial.tools.miniterm "$PORT" 115200
```

To go back: `$ESPTOOL write_flash 0x0 tab5-stock-1.bin` (and an
ESP-Hosted-MCU image on the C6, see above).

## Boot log to look for

- `tab5-power: C6 power on (expander 0x44 ...)`, then
  `C6 reset GPIO15 pulsed both ways and released`
- `mmc0: new SDIO card at address 0001`
- `ESP-Hosted Version: NG-1.0.6.0.1` and `wlan0`
- `ES_USB_GADGET SKIP: no USB device controller`
- `ES_CREDENTIALS NEW ...` with the SoftAP SSID and passphrase

## Open items

- ST7123 is the default panel; ILI9881C (with GT911 touch) is available as a
  variant, see below.  ST7121 panels still need their own init table.
- `/boot/config.txt` edits are not saved on the Tab5 (no USB drive).
- The keyboard reports firmware version 0; its INT line (GPIO50) is unused.
- The display costs about 4 MB of the kernel's memory (MemFree ~4 MB idle).

## ILI9881C panel + GT911 touch variant (EASYSTICK_TAB5_PANEL=ili9881c)

Some Tab5 units carry an ILI9881C panel (page-1 ID bytes `98 81 5c`) and a
GT911 touch controller at I2C 0x14 instead of the ST7123.  Build both the
boot shim and the image with

```
EASYSTICK_BOARD=tab5 EASYSTICK_TAB5_PANEL=ili9881c bash linux/build-c68.sh --vendor <out> --profile m3-lab
```

What changes relative to the default build:

- kernel patch 0077 (`panel-m5stack-tab5-ili9881c`) replaces 0075; its init
  sequence and timing are taken from M5GFX's `Panel_ILI9881C` (two lanes,
  RGB565, 80 MHz pixel clock, 720x1280);
- kernel patch 0078 (`m5stack-tab5-gt911`) is a polled GT911 driver on a
  bit-banged I2C bus (SDA GPIO31, SCL GPIO32), reported as a multi-touch evdev
  device;
- `linux/dts/tab5-ili9881c.dtsi` is appended to the DTS, and
  `linux/tab5/ili9881c.config.fragment` to the kernel config;
- the boot shim pulses TP_RST (expander 0x43, P5) together with LCD_RST while
  driving GPIO23 high, which makes the GT911 answer at 0x14, then releases
  GPIO23 as an input.

Not verified on hardware yet: the DSI lane rate (the glue uses a fixed
1000 Mbit/s, M5GFX uses 1040), the ESP32-P4 revision v1.0 behaviour (upstream
was tested on v1.3), and the coordinate mapping against the rotated fbcon
(`fbcon=rotate:1`; touch reports the native 720x1280 portrait frame).

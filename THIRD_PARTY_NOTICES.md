# Third-party notices

cpz-baby-monitor is MIT licensed (see LICENSE). It includes or links:

| Component | License | Use |
| --- | --- | --- |
| [Cap-LoRa-1262](https://github.com/CardputerZero/Cap-LoRa-1262) (M5Stack CardputerZero Community) | MIT | SX1262 bring-up, PI4IO driver, LVGL HAL and keypad code forked into `src/radio/hw`, `src/hal`, `src/input`, plus `lv_conf.h` |
| [LVGL](https://github.com/lvgl/lvgl) | MIT | UI, statically linked |
| [RadioLib](https://github.com/jgromes/RadioLib) | MIT | SX1262 driver subset, statically linked |
| [Pigweed](https://github.com/dianjixz/pigweed) | Apache-2.0 | Linux SPI/I2C/GPIO subset, statically linked |
| [spdlog](https://github.com/gabime/spdlog) | MIT | logging, statically linked |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / public domain | audio I/O, compiled in |
| [codec2](https://github.com/drowe67/codec2) | LGPL-2.1 | speech codec, **dynamically linked** from Debian `libcodec2-1.2` |
| [libsodium](https://libsodium.org) | ISC | crypto, dynamically linked from Debian `libsodium23` |

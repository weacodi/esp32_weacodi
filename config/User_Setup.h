// SPDX-License-Identifier: AGPL-3.0-only

#define ILI9341_2_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define SPI_FREQUENCY  40000000

#define USE_HSPI_PORT

#define SMOOTH_FONT

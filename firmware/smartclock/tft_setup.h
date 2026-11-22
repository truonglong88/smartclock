// Cấu hình TFT_eSPI cho "Smart Weather Clock" (ESP8266) — tìm bằng firmware pintest
#define USER_SETUP_LOADED 1
#define ST7789_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 240
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   -1
#define TFT_DC    0
#define TFT_RST   2
#define TFT_SPI_MODE SPI_MODE3
#define TFT_INVERSION_ON
#define TFT_RGB_ORDER TFT_BGR   // đã kiểm chứng: chớp đỏ-lá-dương hiện ra dương-lá-đỏ khi đặt RGB
#define SPI_FREQUENCY 20000000
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_GFXFF
#define SMOOTH_FONT
#define TFT_BL 5
#define TFT_BACKLIGHT_ON LOW

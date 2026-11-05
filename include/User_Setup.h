// TFT_eSPI pin configuration for the ESP32-2432S028 "Cheap Yellow Display".
// Included directly via platformio.ini's build_flags (-include), so this
// file is never picked up from the TFT_eSPI library's own User_Setup.h.
//
// Pins + driver + SPI frequency below match witnessmenow's reference config
// for this exact board (github.com/witnessmenow/ESP32-Cheap-Yellow-Display,
// DisplayConfig/User_Setup.h) verbatim, after our own trial-and-error
// landed on the right pins but the wrong driver macro (ILI9341_DRIVER
// instead of ILI9341_2_DRIVER), which caused inverted colours and partial
// screen corruption.
//
// TOUCH_CS is defined even though we never call tft.getTouch()/
// tft.calibrateTouch() (see src/display.cpp — touch is a separate SPI bus,
// handled directly via XPT2046_Touchscreen, not through TFT_eSPI): leaving
// it undefined crashes tft.init() itself (Guru Meditation LoadProhibited,
// confirmed by direct testing) — looks like a library bug in the
// "no touch configured" code path. Defining it is a harmless workaround.
//
// Display defaults to TFT_eSPI's default VSPI peripheral (no USE_HSPI_PORT
// here); touch is explicitly put on HSPI in src/display.cpp so the two
// never collide despite both being SPI.

#define ILI9341_2_DRIVER
// This board's panel shows colours inverted unless told otherwise (unlike
// the snekdisplay unit; CYD batches differ). If a board shows a white
// background where it should be dark, comment this out.
#define TFT_INVERSION_ON

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1   // tied to EN, no separate reset pin
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH
#define TOUCH_CS 33   // unused by us (see note above) but this is the touch chip's real CS pin

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY       55000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000

#pragma once
// =============================================================================
//  FlightRadar - user configuration
//
//  Target board: Waveshare ESP32-S3-Touch-LCD-7 (800x480 RGB panel + GT911 touch).
//
//  These are just the INITIAL DEFAULTS. Once you save settings from the on-
//  device web page (the "CFG" button, or the Wi-Fi setup portal), they are
//  stored in the ESP32's flash and override everything below. You can also
//  leave Wi-Fi/location as-is and change it all from the browser.
// =============================================================================

// ---- Wi-Fi (default; can be changed from the web portal) -------------------
#define WIFI_SSID        ""
#define WIFI_PASS        ""

// ---- Config portal access point --------------------------------------------
//  When Wi-Fi isn't set up or can't connect (and when you press CFG), the
//  device becomes this Wi-Fi hotspot. Join it and a setup page opens.
#define AP_SSID          "FlightRadar-Setup"
#define AP_PASS          "flightradar"   // min 8 chars, or "" for an open AP

// ---- Your location (centre of the radar) -----------------------------------
//  Decimal degrees. North/East positive, South/West negative.
//  Tip: right-click your house in Google Maps to copy lat, lon.
#define HOME_LAT          51.470000   // default placeholder (London Heathrow) -
#define HOME_LON          -0.454300   //   set your own here or via the web portal
#define HOME_LABEL        "HOME"

// ---- Flight data source ----------------------------------------------------
//  Free, keyless ADS-B aggregators that all speak the ADSBExchange v2 format.
//  Pick ONE host. If one is busy/blocked, try another:
//      "api.adsb.lol"        (recommended, generous limits)
//      "api.airplanes.live"  (max 1 request / second)
//      "api.adsb.one"
#define API_HOST          "api.adsb.lol"

// ---- Firmware version (shown on screen, web pages and serial) --------------
#define FW_VERSION        "2.4.15"

// ---- Debugging (Serial/UART monitor at 115200 baud) ------------------------
//  1 = print status, Wi-Fi, HTTP and JSON diagnostics to the Serial Monitor.
//      Every on-screen status message is mirrored to serial too.
//  0 = quiet build; all the logging below compiles away to nothing.
#define DEBUG             1
//  1 = additionally dump one line per received aircraft (very chatty).
#define DEBUG_AIRCRAFT    0

//  Debug logging macros - compile to nothing when DEBUG is 0. Defined here (a
//  header) rather than in the .ino so the PlatformIO .ino->.cpp converter pass,
//  which preprocesses the sketch under a stricter dialect, doesn't warn about the
//  variadic __VA_ARGS__. Headers are compiled under the normal gnu++ standard.
#if DEBUG
  #define DBG(...)   Serial.printf(__VA_ARGS__)
  #define DBGLN(x)   Serial.println(x)
#else
  #define DBG(...)   do {} while (0)
  #define DBGLN(x)   do {} while (0)
#endif

// ---- Behaviour -------------------------------------------------------------
#define DEFAULT_RANGE_NM  50      // initial radar range (nautical miles)
#define REFRESH_MS        12000   // time between data pulls (>= 1000 ms)
#define MAX_AC            48      // max aircraft tracked at once
#define USE_METRIC        0       // 0 = ft / kts / nm,  1 = m / km/h / km
#define HIDE_GROUND       1       // 1 = hide aircraft on the ground (GND) by default
#define GND_KEEP_TAXI     1       // when hiding GND: 1 = keep aircraft taxiing faster than...
#define GND_TAXI_KT       5       // ...this ground speed (knots); parked/slow still hidden
// These compile the feature in AND set its default; once built in, each can be
// toggled live from the Wi-Fi config page. Set to 0 to remove entirely.
#define SWEEP_ENABLE      1       // 1 = animated radar sweep line
#define SHOW_LANDMARKS    1       // 1 = draw map overlay (coastline etc.) - see landmarks.h
#define SHOW_AIRPORTS     1       // 1 = plot major airports (dot + IATA code) within range
#define AIRPORT_NAMES     0       // when showing airports: 0 = IATA code, 1 = full name
#define SHOW_ROUTES       1       // 1 = compile in the route feature (toggle on web config)
#define ROUTES_DEFAULT    0       // default state of the route lookup: 0 = off (route data
                                  //   isn't always accurate), can be enabled on the web config

// ---- Clock (NTP) -----------------------------------------------------------
#define NTP_SERVER        "pool.ntp.org"
#define GMT_OFFSET_SEC    0       // your timezone offset from UTC, in seconds
#define DST_OFFSET_SEC    3600    // daylight-saving offset, in seconds (0 if none)

// =============================================================================
//  Hardware pins / orientation - Waveshare ESP32-S3-Touch-LCD-7
// =============================================================================
// ---- GT911 capacitive touch (shares the I2C bus with the CH422G) -----------
//  The display's RGB data/sync pins are fixed in board_7in.h (not here).
#define TP_SDA_PIN      8
#define TP_SCL_PIN      9
#define TP_INT_PIN      4
//  GT911 I2C address. Waveshare panels usually enumerate at 0x5D; if touch
//  is dead, try 0x14 (the GT911's other factory address).
#define GT911_ADDR      0x5D

// ---- CH422G I/O expander (controls backlight + resets over I2C) ------------
//  Same I2C bus as the GT911 (pins 8/9). The expander outputs are written as
//  a single byte; EXIO bits used here:
//      EXIO1 = touch reset, EXIO2 = LCD backlight enable, EXIO3 = LCD reset.
#define CH422G_SET_ADDR  0x24   // mode/config register (I2C 7-bit addr)
#define CH422G_OUT_ADDR  0x38   // EXIO output register (I2C 7-bit addr)
#define CH422G_OUT_ON    0x0E   // EXIO1|EXIO2|EXIO3 high: resets released, BL on
#define CH422G_OUT_RESET 0x00   // all low: assert resets, backlight off

// ---- Panel resolution ------------------------------------------------------
//  Plain ESP32-S3-Touch-LCD-7 is 800x480. If you actually have the "7B"
//  (1024x600), set these to 1024 x 600 (and check the porch timings in
//  board_7in.h).
#define PANEL_W         800
#define PANEL_H         480

// ---- Panel pixel clock -----------------------------------------------------
//  RGB pixel clock in Hz. The LCD DMA fetches the frame buffer straight from
//  PSRAM, so it shares the PSRAM bus with the CPU (WiFi/flash). At a high pixel
//  clock the LCD can be starved when WiFi is busy, which permanently shifts the
//  image and shows as fast jitter - UNLESS PSRAM runs at 120 MHz, which gives the
//  bus enough bandwidth for both. With 120 MHz PSRAM (Arduino IDE: Flash Mode
//  "QIO 120MHz" + OPI PSRAM) use the panel's native 16 MHz (~37 fps).
//  NB: don't drop below ~14 MHz - this panel's timing won't lock at 12 MHz and the
//  screen free-runs through colours instead of showing the image.
#define RGB_PCLK_HZ      16000000

// Bounce buffer size in PIXELS (internal SRAM, x2 buffers). A few lines'
// worth is plenty; 10 lines = 8000 px = ~16 KB per buffer. (Enlarging this to 20
// lines was tried to absorb PSRAM bursts but didn't help and ate ~64 KB of internal
// SRAM needed by WiFi/TLS, so it's back at 10. The real fix for the periodic glitch
// was dropping the wasteful 1 Hz repaint - see the render loop in FlightRadar.ino.)
#define RGB_BOUNCE_PX    (PANEL_W * 10)

// ---- Radar sweep animation -------------------------------------------------
//  Each sweep step rebuilds and pushes the whole 768 KB canvas to PSRAM, which
//  competes with the LCD scanout for the PSRAM bus. Animating the arm at the full
//  30 fps poll rate was the last thing still disturbing the display, so we redraw it
//  at ~10 fps instead. The arm makes exactly ONE revolution per data-refresh interval
//  (the "Refresh (seconds)" web setting / REFRESH_MS): it's anchored once at the first
//  fetch and then free-runs continuously at that rate, so it never jumps back to 0 on a
//  data read. The per-step angle is computed from that, so there's nothing to set here.
//  (Data updates and taps still repaint immediately, between arm steps.)
#define SWEEP_REDRAW_MS  100      // ms between arm redraws (100 = ~10 fps)

// ---- Canvas push / draw pacing (anti-roll) ---------------------------------
//  Each redraw (a) draws the whole scene into a 768 KB PSRAM canvas and (b) copies
//  that canvas to the panel frame buffer. Both hammer the PSRAM bus; if either holds
//  it too long the LCD scanout underruns and the picture rolls (worse with many
//  planes, or with the sweep on). Rather than do each as one long blast, we push the
//  canvas in horizontal BANDS and release the bus briefly between bands (and between
//  groups of drawn targets), so the scanout's bounce-buffer refill always gets a turn.
#define PUSH_BAND_H      48       // rows per band when copying the canvas (480 = 10 bands)
#define PUSH_BAND_GAP_US 150      // bus-idle gap between bands (microseconds)
#define DRAW_GROUP        6       // release the bus every N targets while drawing
#define DRAW_GROUP_GAP_US 120     // bus-idle gap between target groups (microseconds)

// ---- Screen / touch orientation --------------------------------------------
//  The 7" panel is natively 800x480 landscape, so rotation 0 is upright.
#define LCD_ROTATION    0
#define TS_ROTATION     0

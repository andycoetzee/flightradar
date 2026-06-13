#pragma once
// =============================================================================
//  Board support: Waveshare ESP32-S3-Touch-LCD-7
//    7" 800x480 RGB parallel panel (Arduino_GFX + bounce buffer) + GT911 touch
//    + CH422G expander
//
//  Provides the same hardware abstraction the shared core expects:
//     gfx, spr                     - RGB panel + offscreen sprite (16-bit, PSRAM)
//     void displayBegin()          - power up panel, build back buffer, set
//                                    SCR_W/SCR_H and radar geometry
//     bool readTouch(int&,int&)    - current touch point in screen coords
//     the 7" UI (drawScreen etc.), a tappable NEARBY list, and handleTouch()
//
//  Included by FlightRadar.ino.
// =============================================================================

// The 7" RGB panel needs the ESP32-S3 (its LCD_CAM peripheral). If the Arduino
// "Board" isn't an S3, the RGB panel types are dropped and you get a confusing
// "does not name a type" cascade. Catch it here with a clear message.
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
  #error "FlightRadar requires an ESP32-S3. In Arduino IDE set Tools > Board > esp32 > 'ESP32S3 Dev Module' (and PSRAM = OPI PSRAM), then recompile."
#endif

// -----------------------------------------------------------------------------
//  Display stack for the Waveshare ESP32-S3-Touch-LCD-7
//
//  The 7" RGB parallel panel is driven by Arduino_GFX (the ESP-IDF RGB LCD
//  driver) with a hardware BOUNCE BUFFER, which is what makes the image
//  jitter-free: scanout is fed from internal SRAM instead of racing PSRAM.
//
//  LovyanGFX is kept ONLY as an off-screen canvas (LGFX_Sprite): all UI is
//  drawn into a full-screen 16-bit sprite in PSRAM exactly as before, then
//  copied into the Arduino_GFX framebuffer once per frame. This preserves the
//  whole existing UI (fonts, datums, layout) while getting the bounce buffer.
//
//  Touch (GT911) reuses LovyanGFX's proven standalone touch driver (port 1, the
//  same config that worked before); the CH422G backlight/reset expander is
//  driven directly over I2C below.
// -----------------------------------------------------------------------------
#define LGFX_USE_V1
#include <LovyanGFX.hpp>            // off-screen canvas (LGFX_Sprite) + GT911 touch driver
#include <Arduino_GFX_Library.h>    // RGB panel scanout with bounce buffer

// Off-screen back buffer (full-screen, 16-bit, PSRAM). Standalone - no panel.
LGFX_Sprite spr;

// GT911 touch via LovyanGFX's standalone driver. Not attached to any panel - we
// configure it on I2C port 1 (pins 8/9) and read raw points directly. This is
// the exact driver/port that worked when LovyanGFX drove the whole display.
lgfx::Touch_GT911 gt911;

// RGB parallel panel. Data/sync GPIOs are fixed by the board wiring (Waveshare
// ESP32-S3-Touch-LCD-7); order is R0..R4, G0..G5, B0..B4.
Arduino_ESP32RGBPanel rgbpanel(
  5 /*DE*/, 3 /*VSYNC*/, 46 /*HSYNC*/, 7 /*PCLK*/,
  1, 2, 42, 41, 40,            // R0..R4  (panel R3..R7)
  39, 0, 45, 48, 47, 21,       // G0..G5  (panel G2..G7)
  14, 38, 18, 17, 10,          // B0..B4  (panel B3..B7)
  0 /*hsync_polarity*/, 8 /*hsync_front_porch*/, 4 /*hsync_pulse_width*/, 43 /*hsync_back_porch*/,
  0 /*vsync_polarity*/, 8 /*vsync_front_porch*/, 4 /*vsync_pulse_width*/, 8 /*vsync_back_porch*/,
  1 /*pclk_active_neg*/, RGB_PCLK_HZ /*prefer_speed*/, false /*useBigEndian*/,
  0 /*de_idle_high*/, 0 /*pclk_idle_high*/, RGB_BOUNCE_PX /*bounce_buffer_size_px*/);

Arduino_RGB_Display gfx(PANEL_W, PANEL_H, &rgbpanel, 0 /*rotation*/, true /*auto_flush*/);

// Panel handle (captured after gfx.begin) so we can re-sync the LCD DMA. On this
// board the RGB DMA can fall behind when PSRAM is busy and the image shifts
// PERMANENTLY (a documented ESP32-S3 limitation; the auto-fix
// CONFIG_LCD_RGB_RESTART_IN_VSYNC is a compile-time flag not reachable from a stock
// Arduino build). esp_lcd_rgb_panel_restart() re-syncs the DMA at the next VSYNC, so
// calling it periodically self-heals the roll instead of it sticking.
static esp_lcd_panel_handle_t gPanelHandle = nullptr;
static inline void panelResync() {
  if (!gPanelHandle) return;
  esp_err_t e = esp_lcd_rgb_panel_restart(gPanelHandle);
#if DEBUG
  static esp_err_t lastE = ESP_FAIL + 12345;   // force first log
  static uint32_t  n = 0;
  if (e != lastE || (n++ % 30) == 0) {         // log changes + ~every 30s
    lastE = e;
    DBG("PANEL restart -> %d (%s)\n", (int)e, esp_err_to_name(e));
  }
#endif
}

// Copy the finished off-screen canvas to the panel framebuffer (one frame).
// LovyanGFX stores 16-bit sprite pixels byte-swapped (big-endian) in memory, so
// we use the big-endian blit variant, which swaps bytes into the little-endian
// RGB565 the panel framebuffer expects. (Using the plain LE variant turns green
// into red, etc.)
static inline void pushCanvas() {
  // Copy in horizontal bands with a short bus-idle gap between them instead of one
  // ~768 KB PSRAM->PSRAM blast. The blast monopolises the PSRAM bus long enough for
  // the LCD scanout to underrun (the picture rolls), especially on heavy frames or
  // during a fetch. Banding lets the scanout's bounce-buffer refill get the bus too.
  uint16_t* buf = (uint16_t*)spr.getBuffer();
  for (int y = 0; y < SCR_H; y += PUSH_BAND_H) {
    int h = SCR_H - y;
    if (h > PUSH_BAND_H) h = PUSH_BAND_H;
    gfx.draw16bitBeRGBBitmap(0, y, buf + (size_t)y * SCR_W, SCR_W, h);
    delayMicroseconds(PUSH_BAND_GAP_US);   // brief gap: CPU off the PSRAM bus
  }
}

#include "landmarks.h"   // map overlay (coastline etc.); uses spr + core helpers

// Tappable "nearby" list geometry (set in drawPanel): rows map to acList[]
static int  listX, listW, listY0, listRowH, listRows;
static int  listIndex[12];            // acList index shown on each visible row

// -----------------------------------------------------------------------------
//  CH422G I/O expander
//  The LCD backlight and the LCD/touch reset lines hang off a CH422G I2C
//  expander (same bus as the GT911 touch). We drive it directly over Wire at
//  boot to enable the backlight and pulse the resets. The GT911 itself is read
//  through the inline I2C driver below (gt911Read / readTouch).
// -----------------------------------------------------------------------------
static void ch422Write(uint8_t addr, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(value);
  Wire.endTransmission();
}

// Power up the panel: set the expander outputs to push-pull, hold the resets
// low briefly, then release them with the backlight on. (The GT911's INT wakeup
// and I2C address discovery are handled by LovyanGFX's Touch_GT911::init().)
static void expanderPowerUp() {
  ch422Write(CH422G_SET_ADDR, 0x01);             // EXIO pins = push-pull outputs
  ch422Write(CH422G_OUT_ADDR, CH422G_OUT_RESET); // assert resets, backlight off
  delay(20);
  ch422Write(CH422G_OUT_ADDR, CH422G_OUT_ON);    // release resets, backlight on
  delay(120);
}

// Fill the whole panel with solid black. Used during an OTA flash write: the
// panel framebuffer is in PSRAM, which shares the octal-SPI bus with flash, so
// the panel DMA underruns while flashing and shows shifted/garbled content. The
// backlight on this board can't be switched off (it's not on the CH422G), so
// instead we make the buffer solid black - an underrun that repeats/shifts black
// is still black, so there's nothing visibly garbled to see.
void otaBlankScreen() {
  spr.fillSprite(TFT_BLACK);
  pushCanvas();
}

// -----------------------------------------------------------------------------
//  GT911 capacitive touch - configured via LovyanGFX's standalone Touch_GT911.
//  Its reset is on the CH422G (pin_rst = -1); init() does the INT wakeup pulse
//  and tries both factory addresses (0x5D / 0x14), so it Just Works. We read
//  raw points; at TS_ROTATION 0 raw coordinates already match the screen.
// -----------------------------------------------------------------------------
static void gt911Begin() {
  auto cfg = gt911.config();
  cfg.x_min = 0;   cfg.x_max = PANEL_W - 1;
  cfg.y_min = 0;   cfg.y_max = PANEL_H - 1;
  cfg.pin_int = TP_INT_PIN;
  cfg.pin_rst = -1;                 // reset is driven via the CH422G expander
  cfg.bus_shared = false;
  cfg.offset_rotation = TS_ROTATION;
  cfg.i2c_port = 1;                 // I2C_NUM_1 (CH422G uses Wire on port 0)
  cfg.pin_sda  = TP_SDA_PIN;
  cfg.pin_scl  = TP_SCL_PIN;
  cfg.freq     = 400000;
  cfg.i2c_addr = GT911_ADDR;
  gt911.config(cfg);
  if (gt911.init()) DBGLN("GT911 touch initialised");
  else              DBGLN("WARN: GT911 touch init failed");
}

// -----------------------------------------------------------------------------
//  HAL: bring up display + touch, allocate the back buffer, compute geometry.
// -----------------------------------------------------------------------------
void displayBegin() {
  // Bring up the CH422G expander first: backlight on + LCD/touch resets out.
  Wire.begin(TP_SDA_PIN, TP_SCL_PIN);
  Wire.setClock(400000);
  expanderPowerUp();

  // Start the RGB panel (bounce-buffered scanout) and clear it.
  gfx.begin();
  gfx.fillScreen(0x0000);
  gPanelHandle = rgbpanel.getPanelHandle();   // for periodic DMA re-sync (anti-roll)
  DBG("PANEL handle=%p\n", (void*)gPanelHandle);
  SCR_W = gfx.width();
  SCR_H = gfx.height();

  // 16-bit full-screen back buffer in PSRAM. All UI is drawn here, then copied
  // to the panel framebuffer once per frame by pushCanvas(). RGB565 matches the
  // panel format, so the copy is straight 16-bit.
  spr.setColorDepth(16);
  spr.setPsram(true);
  if (!spr.createSprite(SCR_W, SCR_H)) Serial.println("ERROR: sprite alloc failed");
  spr.setTextWrap(false);
  DBG("Free heap after sprite: %u bytes\n", ESP.getFreeHeap());

  gt911Begin();   // inline GT911 touch over the shared I2C bus

  // Radar geometry: circular scope on the left, info panel on the right.
  // 30 px header on top; leave ~360 px on the right for the info panel.
  radarR = (SCR_H - 30 - 28) / 2;
  int radarWLimit = (SCR_W - 360 - 40) / 2;
  if (radarR > radarWLimit) radarR = radarWLimit;
  cx = 28 + radarR - 5;        // nudge the scope left 5 px
  cy = 30 + 14 + radarR + 10;  // nudge the scope down 10 px for better centring
}

// HAL: read the touch panel. Returns true if pressed; fills x/y (screen coords).
//
// The 7" RGB parallel bus radiates noise onto the GT911's lines, so the panel
// occasionally reports single-frame "ghost" touches at random coordinates.
// Those caused the unit to drop into (and bounce out of) the config portal on
// its own. We reject them two ways:
//   1. bounds-check the coordinate against the panel, and
//   2. require the press to be confirmed by two consecutive samples that land
//      within a few pixels of each other - a real finger holds steady across
//      polls, a jumping ghost does not.
bool readTouch(int& x, int& y) {
  // The RGB parallel bus injects noise onto the GT911 lines, producing ghost
  // touches that the panel reports as genuine presses - sometimes stable enough
  // to survive a short confirming burst, which let them auto-cycle the range and
  // open the setup portal. We debounce by *duration and stability*, without
  // blocking: a press must stay within a few pixels for ~70 ms before it counts.
  // A real finger holds still easily; an EMI burst jitters between polls, which
  // keeps resetting the timer so it never qualifies. State is held across calls,
  // so once a press qualifies it keeps reporting until the finger lifts.
  const int      TOL      = 12;     // px the press may wander while settling
  const uint32_t HOLD_MS  = 40;     // continuous, stable time before it counts
                                    // (~1 render frame: responsive but still
                                    //  rejects single-frame / jittery EMI ghosts)

  static bool     armed = false;    // a candidate press is being timed
  static bool     fired = false;    // candidate has qualified (reporting true)
  static uint32_t tStart = 0;
  static int      ax = 0, ay = 0;   // anchor position

  lgfx::touch_point_t tp;
  bool raw = (gt911.getTouchRaw(&tp, 1) >= 1);
  int rx = 0, ry = 0;
  if (raw) {
    rx = tp.x; ry = tp.y;
#if (TS_ROTATION == 2)
    rx = SCR_W - 1 - rx;
    ry = SCR_H - 1 - ry;
#endif
    if (rx < 0 || rx >= SCR_W || ry < 0 || ry >= SCR_H) raw = false;  // off-panel
  }

  if (!raw) { armed = false; fired = false; return false; }           // released

  uint32_t now = millis();
  if (!armed ||                                                       // new press, or
      rx - ax < -TOL || rx - ax > TOL ||
      ry - ay < -TOL || ry - ay > TOL) {                              // jumped: restart
    armed = true; fired = false; tStart = now; ax = rx; ay = ry;
    return false;
  }

  if (fired) { x = ax; y = ay; return true; }                         // already counting
  if (now - tStart >= HOLD_MS) { fired = true; x = ax; y = ay; return true; }
  return false;                                                       // still settling
}

// -----------------------------------------------------------------------------
//  Drawing - radar
// -----------------------------------------------------------------------------
void drawPlaneIcon(int x, int y, int track, uint16_t col, bool big) {
  float h = (track < 0) ? 0 : toRad(track);
  float s = big ? 15.0f : 10.0f;
  int nx = x + (int)(s * sinf(h));
  int ny = y - (int)(s * cosf(h));
  float hl = h + toRad(140);
  float hr = h - toRad(140);
  int lx = x + (int)(s * 0.7f * sinf(hl));
  int ly = y - (int)(s * 0.7f * cosf(hl));
  int rx = x + (int)(s * 0.7f * sinf(hr));
  int ry = y - (int)(s * 0.7f * cosf(hr));
  spr.fillTriangle(nx, ny, lx, ly, rx, ry, col);
}

#if SHOW_AIRPORTS
// Built-in table of major airports (lat, lon, IATA code). Static and offline -
// far more reliable than a live OSM/Overpass query, and airports don't move.
// Mostly UK + a few near-neighbour hubs reachable at the wider ranges. Add your
// own here if you relocate. Only those within the current range are drawn.
struct Airport { float lat, lon; const char* code; const char* name; };
static const Airport AIRPORTS[] = {
  {51.4700f, -0.4543f, "LHR", "Heathrow"},      {51.1537f, -0.1821f, "LGW", "Gatwick"},
  {51.8850f,  0.2350f, "STN", "Stansted"},      {51.8747f, -0.3683f, "LTN", "Luton"},
  {51.5053f,  0.0553f, "LCY", "London City"},   {53.3537f, -2.2750f, "MAN", "Manchester"},
  {52.4539f, -1.7480f, "BHX", "Birmingham"},    {53.8659f, -1.6606f, "LBA", "Leeds Bradford"},
  {53.3336f, -2.8497f, "LPL", "Liverpool"},     {52.8311f, -1.3281f, "EMA", "East Midlands"},
  {53.5744f, -0.3508f, "HUY", "Humberside"},    {54.5092f, -1.4294f, "MME", "Teesside"},
  {55.0375f, -1.6917f, "NCL", "Newcastle"},     {55.9500f, -3.3725f, "EDI", "Edinburgh"},
  {55.8719f, -4.4331f, "GLA", "Glasgow"},       {55.5094f, -4.5867f, "PIK", "Prestwick"},
  {57.2019f, -2.1978f, "ABZ", "Aberdeen"},      {57.5425f, -4.0475f, "INV", "Inverness"},
  {54.6575f, -6.2158f, "BFS", "Belfast"},       {51.3827f, -2.7191f, "BRS", "Bristol"},
  {50.9503f, -1.3568f, "SOU", "Southampton"},   {51.3967f, -3.3433f, "CWL", "Cardiff"},
  {52.6758f,  1.2828f, "NWI", "Norwich"},       {53.4213f, -6.2701f, "DUB", "Dublin"},
  {52.3105f,  4.7683f, "AMS", "Amsterdam"},
};

void drawAirports() {
  const uint16_t COL = 0xB596;          // muted grey - distinct from traffic/coast
  spr.setTextColor(COL, TFT_BLACK);
  spr.setTextDatum(TL_DATUM);
  for (const Airport& ap : AIRPORTS) {
    float d = haversineNm(g.homeLat, g.homeLon, ap.lat, ap.lon);
    if (d > rangeNm) continue;          // only what fits on the scope
    int sx, sy;
    geoToScreen(ap.lat, ap.lon, sx, sy);
    spr.fillCircle(sx, sy, 3, COL);     // dot
    spr.drawCircle(sx, sy, 6, COL);     // ring -> airport symbol
    spr.drawString(g.airportNames ? ap.name : ap.code, sx + 9, sy - 4, 2);   // label
  }
}
#endif

// Altitude colour legend, bottom-left corner (outside the scope circle). Mirrors
// altColour() in the .ino so the swatches match the plotted targets exactly.
void drawAltLegend() {
  struct LegEntry { uint16_t col; const char* txt; };
  static const LegEntry leg[] = {
    {0x07FF, "30k+"},      // cyan
    {0x07E0, "20-30k"},    // green
    {0xFFE0, "10-20k"},    // yellow
    {0xFDA0, "3-10k"},     // orange
    {0xF800, "<3k"},       // red
    {0x7BEF, "GND"},       // dark grey
  };
  const int n = (int)(sizeof(leg) / sizeof(leg[0]));
  const int rowH = 12, sw = 9, x = 6;
  int yTop = SCR_H - 6 - n * rowH;        // bottom-aligned stack
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(0x05E0, TFT_BLACK);    // dim green header (matches the rings)
  spr.drawString("ALT (ft)", x, yTop - 11, 1);
  for (int i = 0; i < n; i++) {
    int ry = yTop + i * rowH;
    spr.fillRect(x, ry, sw, sw, leg[i].col);
    spr.drawRect(x, ry, sw, sw, 0x4208);  // subtle border so grey/dark swatches read
    spr.setTextColor(leg[i].col, TFT_BLACK);
    spr.drawString(leg[i].txt, x + sw + 4, ry, 1);
  }
}

void drawRadarScope() {
  const uint16_t GREEN_BRT = TFT_GREEN;   // bright radar green (0x07E0)
  const uint16_t GREEN_MED = 0x05E0;      // medium green (rings / crosshair)
  const uint16_t GREEN_DIM = 0x03E0;      // dim green (minor ticks)

  // Range rings (3 px, medium green)
  for (int i = 1; i <= 3; i++) {
    int rr = radarR * i / 3;
    for (int o = 0; o < 3; o++) spr.drawCircle(cx, cy, rr - o, GREEN_MED);
  }
  // Bright outer ring (4 px)
  for (int o = 0; o < 4; o++) spr.drawCircle(cx, cy, radarR - o, GREEN_BRT);

  // Cross hairs (3 px) through the centre
  spr.fillRect(cx - radarR, cy - 1, 2 * radarR + 1, 3, GREEN_MED);
  spr.fillRect(cx - 1, cy - radarR, 3, 2 * radarR + 1, GREEN_MED);

  // Compass rose: ticks every 10 deg, labels (tens of degrees) every 30 deg.
  spr.setTextDatum(MC_DATUM);
  for (int d = 0; d < 360; d += 10) {
    float a  = toRad(d);
    float sa = sinf(a), ca = cosf(a);
    bool major = (d % 30 == 0);
    int rInner = radarR - (major ? 16 : 8);
    int x0 = cx + (int)(rInner * sa), y0 = cy - (int)(rInner * ca);
    int x1 = cx + (int)(radarR * sa), y1 = cy - (int)(radarR * ca);
    spr.drawLine(x0, y0, x1, y1, major ? GREEN_BRT : GREEN_DIM);
    if (major) {                          // 3 px tick
      spr.drawLine(x0 + 1, y0, x1 + 1, y1, GREEN_BRT);
      spr.drawLine(x0, y0 + 1, x1, y1 + 1, GREEN_BRT);
      char lbl[3];
      snprintf(lbl, sizeof(lbl), "%02d", d / 10);   // 00,03,06 ... 33
      int rl = radarR - 28;
      spr.setTextColor(GREEN_BRT, TFT_BLACK);
      spr.drawString(lbl, cx + (int)(rl * sa), cy - (int)(rl * ca), 2);
    }
  }

  // Range-ring distance labels
  spr.setTextColor(GREEN_MED, TFT_BLACK);
  spr.setTextDatum(BL_DATUM);
  for (int i = 1; i <= 3; i++) {
    int rr = radarR * i / 3;
    int val = (int)distOut(rangeNm * i / 3.0f);
    spr.drawNumber(val, cx + 4, cy - rr - 2, 2);
  }

#if SHOW_LANDMARKS
  if (g.landmarks) drawLandmarks();   // coastline / labelled points, clipped to the scope
#endif

#if SHOW_AIRPORTS
  if (g.airports) drawAirports();     // major airports within range (dot + IATA code)
#endif

#if SWEEP_ENABLE
  // Only show the rotating arm once we have a live ADS-B feed (dataOk). Before the
  // first successful fetch - or if the connection drops - the sweep is hidden, so
  // a still scope signals "not receiving". Also honours the runtime toggle.
  const uint16_t sweepTail[6] = {GREEN_BRT, 0x06E0, 0x0460, 0x0320, 0x0220, 0x0120};
  if (dataOk && g.sweep)
  for (int t = 0; t < 6; t++) {
    float ang = toRad(sweepAngle - t * 5);
    int ex = cx + (int)(radarR * sinf(ang)), ey = cy - (int)(radarR * cosf(ang));
    spr.drawLine(cx, cy, ex, ey, sweepTail[t]);
    if (t == 0) {                         // 3 px leading edge
      spr.drawLine(cx + 1, cy, ex + 1, ey, sweepTail[t]);
      spr.drawLine(cx, cy + 1, ex, ey + 1, sweepTail[t]);
    }
  }
#endif

  // Own position (bullseye)
  spr.fillRect(cx - 8, cy - 1, 17, 3, GREEN_BRT);
  spr.fillRect(cx - 1, cy - 8, 3, 17, GREEN_BRT);
  spr.fillCircle(cx, cy, 3, TFT_WHITE);
  spr.setTextColor(GREEN_BRT, TFT_BLACK);
  spr.setTextDatum(TC_DATUM);
  spr.drawString(g.label, cx, cy + 10, 2);

  // Targets: symbol + velocity leader + ATC-style data block.
  // acList is sorted nearest-first; iterate farthest-first so the nearest aircraft
  // (and their data blocks) are drawn last and sit on top rather than being painted
  // over by more-distant traffic.
  int drawn = 0;
  for (int i = acCount - 1; i >= 0; i--) {
    if (acList[i].distNm > rangeNm) continue;
    // Release the PSRAM bus briefly every few targets so a crowded scope (lots of
    // labels = lots of PSRAM writes) can't hold the bus long enough to roll the panel.
    if (drawn && (drawn % DRAW_GROUP) == 0) delayMicroseconds(DRAW_GROUP_GAP_US);
    drawn++;
    int sx, sy;
    planeToScreen(acList[i], sx, sy);
    bool isSel = (i == selected);
    uint16_t col = altColour(acList[i]);

    // History trail: past fixes as fading dots in the target's altitude colour.
    int ti = findTrail(acList[i].hex);
    if (ti >= 0 && trails[ti].count > 1) {
      const Trail& tr = trails[ti];
      for (int k = 0; k < tr.count - 1; k++) {   // skip newest (== current pos)
        int hx, hy;
        geoToScreen(tr.lat[k], tr.lon[k], hx, hy);
        spr.fillCircle(hx, hy, 2, dimColour(col, k + 1, tr.count));
      }
    }

    // Velocity leader line (predictor), length ~ groundspeed.
    if (acList[i].track >= 0 && acList[i].gs > 30) {
      float a = toRad(acList[i].track);
      int len = acList[i].gs / 9;
      if (len > 48) len = 48;
      spr.drawLine(sx, sy, sx + (int)(len * sinf(a)),
                   sy - (int)(len * cosf(a)), col);
    }

    drawPlaneIcon(sx, sy, acList[i].track, col, isSel);
    if (isSel) {
      spr.drawCircle(sx, sy, 20, TFT_WHITE);
      spr.drawCircle(sx, sy, 21, TFT_WHITE);
    }

    // ATC-style data block: callsign / flight-level + groundspeed / type, drawn in
    // the target's altitude colour with a short leader from the icon. Shown for
    // every aircraft in range (the pre-declutter behaviour). Because the loop runs
    // farthest-first, nearer blocks are painted last and sit on top.
    {
      int bx = sx + 14, by = sy - 18;
      char tag[16];
      if (acList[i].onGround) snprintf(tag, sizeof(tag), "GND  %d", acList[i].gs);
      else snprintf(tag, sizeof(tag), "%03d  %d", acList[i].alt / 100, acList[i].gs);
      spr.drawLine(sx, sy, bx - 2, by + 16, isSel ? TFT_WHITE : 0x0480);
      spr.setTextDatum(TL_DATUM);
      spr.setTextColor(col, TFT_BLACK);
      spr.drawString(acList[i].flight, bx, by, 2);
      spr.drawString(tag, bx, by + 16, 2);
      if (strlen(acList[i].type) > 0) spr.drawString(acList[i].type, bx, by + 32, 2);
    }
  }

  drawAltLegend();   // altitude colour key, bottom-left corner
}

void drawHeader() {
  spr.fillRect(0, 0, SCR_W, 30, 0x10A2);
  spr.setTextColor(0x1C9F, 0x10A2);           // blue title
  spr.setTextDatum(TL_DATUM);
  spr.drawString("FLIGHT RADAR", 8, 6, 4);
  spr.setTextColor(0x7BEF, 0x10A2);
  spr.drawString("v" FW_VERSION, 196, 11, 2);

  struct tm ti;
  char dStr[8] = "--/--", tStr[8] = "--:--";
  if (getLocalTime(&ti, 5)) {
    snprintf(dStr, sizeof(dStr), "%02d/%02d", ti.tm_mday, ti.tm_mon + 1);
    snprintf(tStr, sizeof(tStr), "%02d:%02d", ti.tm_hour, ti.tm_min);
  }
  spr.setTextColor(TFT_WHITE, 0x10A2);
  const int gap = 12;                         // space between the date and time
  spr.setTextDatum(TR_DATUM);
  spr.drawString(dStr, SCR_W / 2 - gap, 6, 4);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(tStr, SCR_W / 2 + gap, 6, 4);

  spr.setTextDatum(TR_DATUM);
  bool wifi = (WiFi.status() == WL_CONNECTED);
  spr.setTextColor(wifi ? TFT_GREEN : TFT_RED, 0x10A2);
  spr.drawString(wifi ? "WiFi" : "No WiFi", SCR_W - 8, 6, 4);
}

void drawPanel() {
  int px = cx + radarR + 24;
  int pw = SCR_W - px - 10;
  int colW = pw / 2 + 22;       // right column (Reg/Spd/Dst) nudged right so the
                                // left column has more room for the type name

  spr.drawFastVLine(px - 12, 34, SCR_H - 42, 0x18E3);

  // ---- Top stats: RANGE | TRACKED ----
  spr.setTextColor(TFT_CYAN, TFT_BLACK);
  spr.setTextDatum(TL_DATUM);
  spr.drawString("RANGE", px, 40, 2);
  spr.drawString("TRACKED", px + colW, 40, 2);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  char rbuf[16];
  snprintf(rbuf, sizeof(rbuf), "%d %s", (int)distOut(rangeNm), distUnit());
  spr.drawString(rbuf, px, 58, 4);
  // Count only what's actually on the scope (within range). acList is sorted
  // nearest-first, so the in-range planes are a prefix - stop at the first one
  // past the edge. This keeps TRACKED in step with the dots on the radar.
  int shown = 0;
  while (shown < acCount && acList[shown].distNm <= rangeNm) shown++;
  spr.drawNumber(shown, px + colW, 58, 4);

  // ---- Selected / nearest aircraft detail ----
  int y = 100;
  const Aircraft* a = nullptr;
  // Default to NEAREST - the panel is in "nearest" mode unless the user has actually
  // tapped a plane. (Previously this defaulted to SELECTED, so with nothing selected
  // and no aircraft in range the header was stuck on SELECTED.)
  const char* title = "NEAREST";
  // acList is sorted nearest-first, so acList[0] is the nearest; only treat it as
  // NEAREST when it's actually inside the current range. This keeps the panel in
  // step with the scope when the range is reduced below a plane's distance.
  if (selected >= 0 && selected < acCount)             { a = &acList[selected]; title = "SELECTED"; }
  else if (acCount > 0 && acList[0].distNm <= rangeNm)   a = &acList[0];

  spr.setTextColor(TFT_ORANGE, TFT_BLACK);
  spr.drawString(title, px, y, 2); y += 20;

  if (a) {
    // Full type ("Manufacturer model") and airline/operator come from the same
    // cached adsbdb lookup (by Mode-S hex). Read both here, once, under the lock.
    // Works for the SELECTED flight and for NEAREST (netTask keeps the lookup
    // pointed at whichever is shown), and costs no network call - it's RAM cache.
    char fullBuf[40]  = "";
    char ownerBuf[36] = "";
    LOCK();
    int tci = typeCacheFind(a->hex);
    if (tci >= 0) {                          // show whatever the lookup resolved,
      if (strlen(typeCache[tci].full))  strlcpy(fullBuf,  typeCache[tci].full,  sizeof(fullBuf));
      if (strlen(typeCache[tci].owner)) strlcpy(ownerBuf, typeCache[tci].owner, sizeof(ownerBuf));
    }                                        // independent of the type resolving
    UNLOCK();

    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.drawString(a->flight, px, y, 4);

    // Airline beside the callsign on the same row. The ICAO callsign prefix
    // (e.g. RYR -> Ryanair) is instant and needs no API call, so it's the primary
    // source; fall back to the adsbdb registered owner for prefixes not in the
    // table (or GA traffic). Clipped to the panel width remaining after the ID.
    const char* airline = airlineFromCallsign(a->flight);
    bool isMil = (a->dbFlags & 0x01);          // feed-tagged military
    if (airline || strlen(ownerBuf) || isMil) {
      // Prefer the airline (callsign prefix), then the adsbdb owner, and only fall
      // back to "Military" for flagged military traffic with no other operator.
      String op = airline ? String(airline)
                          : (strlen(ownerBuf) ? String(ownerBuf) : String("Military"));
      spr.setTextFont(4);
      int ax = px + spr.textWidth(a->flight) + 10;   // just right of the callsign
      spr.setTextFont(2);
      int omax = px + pw - ax;
      if (omax > 24) {                                // only if there's room
        if (spr.textWidth(op) > omax) {
          while (op.length() > 1 && spr.textWidth(op + "...") > omax) op.remove(op.length() - 1);
          op += "...";
        }
        spr.setTextColor(0xFE60, TFT_BLACK);          // light amber
        spr.drawString(op, ax, y + 8, 2);             // vertically align to font 4
      }
    }
    y += 30;

    char line[40];
    spr.setTextColor(0xBDF7, TFT_BLACK);
    // Type label: full name once resolved, otherwise the short ICAO code. Stays
    // on one row sharing with the registration, clipped to the left column.
    // Type name: prefer the offline ICAO table (instant, reliable), then the
    // adsbdb-resolved name, then the short feed code.
    const char* sFull = typeNameFromIcao(a->type);
    const char* showT = sFull ? sFull : (fullBuf[0] ? fullBuf : (strlen(a->type) ? a->type : nullptr));
    if (showT) {
      String t = String(showT);
      spr.setTextFont(2);
      int tmax = colW - 6;                  // keep clear of the Reg column
      if (spr.textWidth(t) > tmax) {
        while (t.length() > 1 && spr.textWidth(t + "...") > tmax) t.remove(t.length() - 1);
        t += "...";
      }
      spr.drawString(t, px, y, 2);
    }
    if (strlen(a->reg)) spr.drawString((String("Reg ") + a->reg).c_str(), px + colW, y, 2);
    y += 18;

    // Vertical state shown in brackets after the altitude (e.g. "Alt 35000ft
    // (Climbing)"); not meaningful while on the ground.
    const char* vs = (a->vrate > 50) ? "Climbing" : (a->vrate < -50) ? "Descending" : "Level";
    if (a->onGround) snprintf(line, sizeof(line), "Alt GND");
    else             snprintf(line, sizeof(line), "Alt %d%s (%s)", altOut(a->alt), altUnit(), vs);
    spr.drawString(line, px, y, 2);
    snprintf(line, sizeof(line), "Spd %d%s", spdOut(a->gs), spdUnit());
    spr.drawString(line, px + colW, y, 2); y += 18;

    if (a->track >= 0) { snprintf(line, sizeof(line), "Hdg %d", a->track); spr.drawString(line, px, y, 2); }
    snprintf(line, sizeof(line), "Dst %.1f%s", distOut(a->distNm), distUnit());
    spr.drawString(line, px + colW, y, 2); y += 18;

#if SHOW_ROUTES
    // Route (origin > destination), looked up by callsign from adsb.lol's routeset
    // and cached in RAM. Only shown once a *plausible* route resolved (state 2), so
    // a wrong/uncertain route is left blank rather than displayed misleadingly.
    if (g.routes) {
      char rf[ROUTE_PLACE_LEN] = "";
      char rt[ROUTE_PLACE_LEN] = "";
      LOCK();
      int rci = routeCacheFind(a->flight);
      if (rci >= 0 && routeCache[rci].state == 2) {
        strlcpy(rf, routeCache[rci].from, sizeof(rf));
        strlcpy(rt, routeCache[rci].to,   sizeof(rt));
      }
      UNLOCK();
      if (strlen(rf) && strlen(rt)) {
        String rte = String(rf) + " > " + rt;
        spr.setTextColor(0x5D9F, TFT_BLACK);     // soft blue, distinct from the rest
        spr.setTextFont(2);
        if (spr.textWidth(rte) > pw) {           // clip long city pairs to the panel
          while (rte.length() > 1 && spr.textWidth(rte + "...") > pw) rte.remove(rte.length() - 1);
          rte += "...";
        }
        spr.drawString(rte, px, y, 2); y += 20;
      }
    }
#endif
  } else {
    spr.setTextColor(0xBDF7, TFT_BLACK);
    spr.drawString("No aircraft in range.", px, y, 2);
  }

  // ---- Buttons along the bottom: RNG- / RNG+ (range down/up) and a small CFG ----
  // CFG is deliberately narrow so it isn't pressed by accident; the two range
  // buttons share the rest of the width so range can be stepped both ways.
  btnH = 44; btnY = SCR_H - btnH - 8;
  const int bgap = 8;
  int cfgW = 56;                                   // small CFG target
  int rngW = (pw - cfgW - 2 * bgap) / 2;
  btnRngDnX = px;                       btnRngDnW = rngW;            // RNG-
  btnRngX   = px + rngW + bgap;         btnRngW   = rngW;            // RNG+
  btnCfgX   = px + 2 * (rngW + bgap);   btnCfgW   = px + pw - btnCfgX;  // CFG (fills remainder)

  // ---- Tappable "NEARBY" list (fills the space above the status line) ----
  int listTop = 248;
  spr.setTextColor(TFT_CYAN, TFT_BLACK);
  spr.setTextDatum(TL_DATUM);
  spr.drawString("NEARBY  (tap to select)", px, listTop, 2);

  listX = px - 8; listW = pw + 8;
  listRowH = 24;
  listY0 = listTop + 20;
  int maxRows = (btnY - 24 - listY0) / listRowH;
  if (maxRows > 12) maxRows = 12;
  listRows = 0;
  for (int i = 0; i < acCount && listRows < maxRows; i++) {
    // Sorted nearest-first, so once we pass the range edge everything after is
    // further still - stop listing. Mirrors the scope's distNm > rangeNm cull so
    // shrinking the range immediately drops now-out-of-range planes here too.
    if (acList[i].distNm > rangeNm) break;
    int ry = listY0 + listRows * listRowH;
    bool isSel = (i == selected);
    const uint16_t SEL_BG = 0x0208;                     // highlight bar colour
    const uint16_t rowBg  = isSel ? SEL_BG : TFT_BLACK; // text bg matches the bar
    if (isSel) spr.fillRect(listX, ry - 2, listW, listRowH, SEL_BG);
    spr.setTextColor(isSel ? TFT_WHITE : 0xBDF7, rowBg);
    spr.setTextDatum(TL_DATUM);
    spr.drawString(acList[i].flight, px, ry, 2);
    if (strlen(acList[i].type)) {                       // aircraft type, middle column
      spr.setTextColor(isSel ? TFT_WHITE : 0x7BEF, rowBg);
      spr.drawString(acList[i].type, px + 92, ry, 2);
      spr.setTextColor(isSel ? TFT_WHITE : 0xBDF7, rowBg);
    }
    char info[24];
    if (acList[i].onGround)
      snprintf(info, sizeof(info), "GND  %.0f%s", distOut(acList[i].distNm), distUnit());
    else
      snprintf(info, sizeof(info), "%d%s  %.0f%s",
               altOut(acList[i].alt), altUnit(), distOut(acList[i].distNm), distUnit());
    spr.setTextDatum(TR_DATUM);
    spr.drawString(info, px + pw, ry, 2);
    listIndex[listRows] = i;
    listRows++;
  }

  // Status / last-fetch result: blue when OK, red on a failure.
  spr.setTextColor(dataOk ? 0x1C9F : TFT_RED, TFT_BLACK);   // 0x1C9F = light blue
  spr.setTextDatum(TL_DATUM);
  spr.drawString(statusMsg.substring(0, 24).c_str(), px, btnY - 20, 2);

  spr.setTextColor(TFT_WHITE);
  spr.setTextDatum(MC_DATUM);

  spr.fillRoundRect(btnRngDnX, btnY, btnRngDnW, btnH, 6, 0x034A);
  spr.drawRoundRect(btnRngDnX, btnY, btnRngDnW, btnH, 6, TFT_CYAN);
  spr.drawString("RNG-", btnRngDnX + btnRngDnW / 2, btnY + btnH / 2, 4);

  spr.fillRoundRect(btnRngX, btnY, btnRngW, btnH, 6, 0x034A);
  spr.drawRoundRect(btnRngX, btnY, btnRngW, btnH, 6, TFT_CYAN);
  spr.drawString("RNG+", btnRngX + btnRngW / 2, btnY + btnH / 2, 4);

  spr.fillRoundRect(btnCfgX, btnY, btnCfgW, btnH, 6, 0x3A06);
  spr.drawRoundRect(btnCfgX, btnY, btnCfgW, btnH, 6, TFT_CYAN);
  spr.drawString("CFG", btnCfgX + btnCfgW / 2, btnY + btnH / 2, 2);  // smaller label for the small button
}

// Set true to force a full-screen repaint on the next drawScreen() (e.g. after a
// tap changes the selection/range). Otherwise most frames only repaint the
// animating radar scope, leaving the static header + info panel untouched.
volatile bool gForceFull = true;

void drawScreen() {
  // With the bounce-buffered RGB driver, scanout is fed from internal SRAM, so
  // a full-frame redraw + copy no longer races the panel timing - we just draw
  // the whole UI into the back buffer and push it. (gForceFull is retained for
  // source compatibility with handleTouch but is no longer needed.)
  gForceFull = false;
  spr.fillSprite(TFT_BLACK);
  // Clip the radar render to its own area so aircraft tags near the right edge can't
  // bleed into the right-hand panel. The panel divider sits at (cx+radarR+12); keep
  // all scope drawing left of it. Cleared before the header/panel draw full width.
  spr.setClipRect(0, 0, cx + radarR + 12, SCR_H);
  drawRadarScope();
  spr.clearClipRect();
  drawHeader();
  drawPanel();
  pushCanvas();
}

void drawConfigPortalScreen() {
  spr.fillSprite(TFT_BLACK);
  spr.fillRect(0, 0, SCR_W, 30, 0x10A2);
  spr.setTextColor(TFT_YELLOW, 0x10A2);
  spr.setTextDatum(TL_DATUM);
  spr.drawString("WI-FI SETUP MODE", 10, 6, 4);
  spr.setTextColor(0x7BEF, 0x10A2);
  spr.drawString("v" FW_VERSION, 260, 11, 2);
  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(TFT_WHITE, 0x10A2);
  spr.drawString((String(WiFi.softAPgetStationNum()) + " joined").c_str(), SCR_W - 10, 8, 4);

  // Back-to-radar button (top-right, below the header). Tapping it leaves the
  // portal and reconnects to the saved network without a reboot.
  cfgBackW = 130; cfgBackH = 46;
  cfgBackX = SCR_W - cfgBackW - 12; cfgBackY = 40;
  spr.fillRoundRect(cfgBackX, cfgBackY, cfgBackW, cfgBackH, 8, 0x1082);
  spr.drawRoundRect(cfgBackX, cfgBackY, cfgBackW, cfgBackH, 8, TFT_WHITE);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_WHITE, 0x1082);
  spr.drawString("< BACK", cfgBackX + cfgBackW / 2, cfgBackY + cfgBackH / 2, 4);

  spr.setTextDatum(TL_DATUM);
  int y = 70;
  spr.setTextColor(TFT_CYAN, TFT_BLACK);
  spr.drawString("1) Join this Wi-Fi network:", 40, y, 4); y += 44;
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.drawString(AP_SSID, 80, y, 4); y += 50;
  if (strlen(AP_PASS)) {
    spr.setTextColor(0x9CD3, TFT_BLACK);
    spr.drawString((String("password: ") + AP_PASS).c_str(), 80, y, 4); y += 50;
  }
  spr.setTextColor(TFT_CYAN, TFT_BLACK);
  spr.drawString("2) Open in your browser:", 40, y, 4); y += 44;
  spr.setTextColor(TFT_GREEN, TFT_BLACK);
  spr.drawString((String("http://") + WiFi.softAPIP().toString()).c_str(), 80, y, 4); y += 50;
  spr.setTextColor(0x7BEF, TFT_BLACK);
  spr.drawString("Set Wi-Fi, location & options, then Save.", 40, SCR_H - 36, 4);
  pushCanvas();
}

void drawOtaScreen(const char* msg, uint16_t col) {
  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_YELLOW, TFT_BLACK);
  spr.drawString("FIRMWARE UPDATE", SCR_W / 2, SCR_H / 2 - 60, 4);
  spr.setTextColor(col, TFT_BLACK);
  spr.drawString(msg, SCR_W / 2, SCR_H / 2, 4);
  spr.setTextColor(0x7BEF, TFT_BLACK);
  spr.drawString("Do not power off", SCR_W / 2, SCR_H / 2 + 56, 4);
  pushCanvas();
}

// -----------------------------------------------------------------------------
//  Boot splash - an ATC-flavoured "title card" shown while we bring up Wi-Fi.
//  A green PPI scope (rings + crosshair + a frozen sweep and a few colour-coded
//  blips) sits behind the blue "FLIGHT RADAR" wordmark, with the firmware
//  version and a live boot-status line. `status` is the current setup step
//  (e.g. "Connecting Wi-Fi..."), so the same card doubles as the boot progress.
// -----------------------------------------------------------------------------
void drawSplashScreen(const char* status) {
  const uint16_t GREEN_BRT = TFT_GREEN;     // bright sweep / center
  const uint16_t GREEN_DIM = 0x03E0;        // dim rings / crosshair
  const uint16_t BLUE      = 0x1C9F;        // wordmark blue (matches header)

  spr.fillSprite(TFT_BLACK);

  const int cx = SCR_W / 2;
  const int cy = SCR_H / 2 + 36;            // scope sits a little below centre
  const int R  = 190;

  // Range rings + crosshair
  for (int r = R / 3; r <= R; r += R / 3) spr.drawCircle(cx, cy, r, GREEN_DIM);
  spr.drawFastHLine(cx - R, cy, 2 * R, GREEN_DIM);
  spr.drawFastVLine(cx, cy - R, 2 * R, GREEN_DIM);

  // A frozen sweep with a short fading tail, like the live scope
  const uint16_t tail[5] = {GREEN_BRT, 0x05E0, 0x0460, 0x0320, 0x0220};
  for (int t = 0; t < 5; t++) {
    float a = toRad(40 - t * 6);
    spr.drawLine(cx, cy, cx + (int)(R * sinf(a)), cy - (int)(R * cosf(a)), tail[t]);
  }
  spr.fillCircle(cx, cy, 4, GREEN_BRT);

  // A few decorative, altitude-coloured blips around the scope
  drawPlaneIcon(cx - 70, cy - 90,  60,  0x07FF, false);   // cyan  (low)
  drawPlaneIcon(cx + 95, cy - 30, 210,  0xFFE0, false);   // amber (mid)
  drawPlaneIcon(cx + 30, cy + 95, 320,  0xFD20, false);   // orange(high)

  // Wordmark - blue, double-size font 4, with a black backdrop so it reads
  // cleanly over the rings.
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(BLUE, TFT_BLACK);
  spr.drawString("FLIGHT RADAR", cx, 96, 4);
  spr.setTextSize(1);

  spr.setTextColor(0x7BEF, TFT_BLACK);
  spr.drawString("Live ADS-B Air Traffic Display", cx, 150, 4);

  // Version + boot status
  spr.setTextColor(GREEN_DIM, TFT_BLACK);
  spr.drawString("v" FW_VERSION, cx, SCR_H - 64, 2);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.drawString(status ? status : "", cx, SCR_H - 34, 4);

  pushCanvas();
}

// -----------------------------------------------------------------------------
//  Touch handling
// -----------------------------------------------------------------------------
// Mark an aircraft as the selected one and queue its type lookup (or clear).
static void selectAircraft(int idx) {
  selected = idx;
  if (idx >= 0 && idx < acCount) {
    strlcpy(selectedHex, acList[idx].hex, sizeof(selectedHex));  // sticky across refreshes
    strlcpy(typeHex, acList[idx].hex, sizeof(typeHex));
    typeFull[0] = '\0';
    typeOwner[0] = '\0';
    LOCK();
    int ci = typeCacheFind(typeHex);          // already looked this airframe up?
    if (ci >= 0) {                  // cache hit: use it, no network call
      strlcpy(typeFull,  typeCache[ci].full,  sizeof(typeFull));
      strlcpy(typeOwner, typeCache[ci].owner, sizeof(typeOwner));
      typeState = typeCache[ci].state;
    } else {
      // Resolve from the offline ICAO type table first (instant, no API). Only
      // fall back to an adsbdb lookup for codes the table doesn't know.
      const char* sf = typeNameFromIcao(acList[idx].type);
      if (sf) {
        strlcpy(typeFull, sf, sizeof(typeFull));
        typeCachePut(typeHex, sf, "", 2);
        typeState = 2;
      } else {
        typeState = 1;              // miss: queue an adsbdb lookup
      }
    }
    UNLOCK();
  } else {
    selectedHex[0] = '\0';
    typeState  = 0;
  }
}

void handleTouch() {
  int tx = 0, ty = 0;
  bool touched = readTouch(tx, ty);

  if (touched && !wasTouched) {     // rising edge = a fresh tap
    gForceFull = true;              // a tap changes selection/range -> full repaint
    gNeedRedraw = true;             // and forces a repaint in sweep-off mode
    bool inBtnRow = (ty >= btnY && ty <= btnY + btnH);
    if (inBtnRow && tx >= btnRngDnX && tx <= btnRngDnX + btnRngDnW) {  // RNG-
      rangeIdx = (rangeIdx - 1 + numRanges) % numRanges;
      rangeNm  = rangeSteps[rangeIdx];
      selected = -1;
      selectedHex[0] = '\0';
      typeState  = 0;
      firstFetch = false;           // force an immediate refresh at new range
    } else if (inBtnRow && tx >= btnRngX && tx <= btnRngX + btnRngW) {  // RNG+
      rangeIdx = (rangeIdx + 1) % numRanges;
      rangeNm  = rangeSteps[rangeIdx];
      selected = -1;
      selectedHex[0] = '\0';
      typeState  = 0;
      firstFetch = false;           // force an immediate refresh at new range
    } else if (inBtnRow && tx >= btnCfgX && tx <= btnCfgX + btnCfgW) {  // CFG
      wasTouched = touched;
      requestConfigPortal();   // netTask starts the portal safely (owns Wi-Fi)
      return;
    } else if (tx >= listX && tx <= listX + listW &&
               ty >= listY0 && ty < listY0 + listRows * listRowH) {  // list row
      int row = (ty - listY0) / listRowH;
      if (row >= 0 && row < listRows) selectAircraft(listIndex[row]);
    } else {                                                    // tap a plane?
      int best = -1;
      int bestD2 = 36 * 36;         // generous finger-sized hit radius (picks the
                                    // nearest plane within it, so a near-miss still
                                    // selects the intended target)
      for (int i = 0; i < acCount; i++) {
        if (acList[i].distNm > rangeNm) continue;
        int sx, sy;
        planeToScreen(acList[i], sx, sy);
        int dx = sx - tx, dy = sy - ty;
        int d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
      }
      selectAircraft(best);
    }
  }
  wasTouched = touched;
}

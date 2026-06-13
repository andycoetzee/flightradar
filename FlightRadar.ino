// =============================================================================
//  FlightRadar - a desk "flight radar" for the Waveshare ESP32-S3-Touch-LCD-7
//
//  Hardware: Waveshare ESP32-S3-Touch-LCD-7 - a 7" 800x480 16-bit RGB parallel
//  panel + GT911 capacitive touch + CH422G I/O expander, all on one PCB.
//
//  What it does:
//    * Connects to Wi-Fi and pulls live ADS-B aircraft positions around you
//      from a free, keyless aggregator (adsb.lol / airplanes.live / adsb.one).
//    * Draws a PPI radar scope: you are in the centre, range rings show
//      distance, each aircraft is a triangle pointing its direction of travel,
//      coloured by altitude band, with an animated sweep.
//    * Tap an aircraft for details (incl. full aircraft-type lookup).
//      Tap RNG+ to change range, CFG to open the web setup portal.
//    * WEB CONFIG + OTA: Wi-Fi, location, range, units, timezone and data
//      source are all set from a browser, and firmware can be updated over the
//      air. Settings persist in flash.
//
//  This .ino holds the display-agnostic core. Everything specific to the panel
//  and touch controller lives in board_7in.h, which provides:
//      spr, displayBegin(), readTouch(), and the UI + handleTouch.
//
//  Required libraries (Arduino IDE -> Library Manager):
//    * ArduinoJson             (by Benoit Blanchon, v7+)
//    * GFX Library for Arduino (by moononournation)
//        drives the RGB panel with a bounce buffer (jitter-free scanout)
//    * LovyanGFX               (by lovyan03)
//        used as the off-screen canvas (LGFX_Sprite) + GT911 touch driver
//  (WiFi, WebServer, DNSServer, ESPmDNS, Update, Preferences ship with the core.)
//
//  See README.md for board settings and step-by-step setup.
// =============================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>
#include <LittleFS.h>            // coastline cache lives in a file, not NVS
#include <ArduinoJson.h>
#include <Wire.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "config.h"
#include "airlines.h"           // ICAO callsign-prefix -> airline name
#include "aircraft_types.h"     // ICAO type designator -> full aircraft name

// Debug logging macros (DBG / DBGLN) are defined in config.h, which is included
// above - kept there so the .ino->.cpp converter doesn't warn on __VA_ARGS__.

// -----------------------------------------------------------------------------
//  Persistent settings (defaults come from config.h, then overridden by flash)
// -----------------------------------------------------------------------------
struct Settings {
  char     ssid[33];
  char     pass[65];
  char     apiHost[40];
  char     label[16];
  float    homeLat;
  float    homeLon;
  int      defaultRange;
  uint32_t refreshMs;
  long     gmtOffset;
  int      dstOffset;
  uint8_t  useMetric;
  uint8_t  hideGround;   // 1 = drop aircraft on the ground from the feed
  uint8_t  gndKeepTaxi;  // when hideGround: 1 = keep ground traffic above gndTaxiKt
  uint8_t  gndTaxiKt;    // taxi speed threshold (knots)
  uint8_t  sweep;        // 1 = animated radar sweep arm
  uint8_t  landmarks;    // 1 = draw coastline / map overlay
  uint8_t  airports;     // 1 = plot major airports within range
  uint8_t  airportNames; // 1 = label airports with full name, 0 = IATA code
  uint8_t  routes;       // 1 = look up flight route (FROM > TO) for the focused plane
};
Settings g;

Preferences prefs;

// -----------------------------------------------------------------------------
//  Aircraft model
//  (Defined up here, before any function, so the Arduino IDE's auto-generated
//   function prototypes can see the type.)
// -----------------------------------------------------------------------------
struct Aircraft {
  char  hex[8];
  char  flight[10];
  char  type[6];
  char  reg[10];
  float lat, lon;
  int   alt;        // ft
  int   gs;         // knots
  int   track;      // deg (0-359), -1 if unknown
  int   vrate;      // ft/min
  float distNm;     // computed from HOME
  float bearing;    // computed deg from HOME (0 = north)
  bool  onGround;
  uint8_t dbFlags;  // feed flags: bit0=military, bit1=interesting, bit2=PIA, bit3=LADD
};

Aircraft acList[MAX_AC];
int      acCount = 0;

// -----------------------------------------------------------------------------
//  Position history ("trails") - a few past fixes per aircraft, keyed by ICAO
//  hex so a target keeps its trail across refreshes even as the list re-sorts.
//  Stored as lat/lon and re-projected each frame so trails honour the current
//  range. Newest fix is the last valid entry.
// -----------------------------------------------------------------------------
#define TRAIL_LEN 6
struct Trail {
  char    hex[8];
  float   lat[TRAIL_LEN];
  float   lon[TRAIL_LEN];
  uint8_t count;
};
Trail trails[MAX_AC];
int   trailCount = 0;

void loadSettings() {
  prefs.begin("flightradar", true);            // read-only
  String s;
  s = prefs.getString("ssid",  WIFI_SSID);  strlcpy(g.ssid,    s.c_str(), sizeof(g.ssid));
  s = prefs.getString("pass",  WIFI_PASS);  strlcpy(g.pass,    s.c_str(), sizeof(g.pass));
  s = prefs.getString("host",  API_HOST);   strlcpy(g.apiHost, s.c_str(), sizeof(g.apiHost));
  s = prefs.getString("label", HOME_LABEL); strlcpy(g.label,   s.c_str(), sizeof(g.label));
  g.homeLat      = prefs.getFloat("lat",     HOME_LAT);
  g.homeLon      = prefs.getFloat("lon",     HOME_LON);
  g.defaultRange = prefs.getInt  ("range",   DEFAULT_RANGE_NM);
  g.refreshMs    = prefs.getUInt ("refresh", REFRESH_MS);
  g.gmtOffset    = prefs.getLong ("gmt",     GMT_OFFSET_SEC);
  g.dstOffset    = prefs.getInt  ("dst",     DST_OFFSET_SEC);
  g.useMetric    = prefs.getUChar("metric",  USE_METRIC);
  g.hideGround   = prefs.getUChar("hidegnd", HIDE_GROUND);
  g.gndKeepTaxi  = prefs.getUChar("keeptaxi",GND_KEEP_TAXI);
  g.gndTaxiKt    = prefs.getUChar("taxikt",  GND_TAXI_KT);
  g.sweep        = prefs.getUChar("sweep",   SWEEP_ENABLE);
  g.landmarks    = prefs.getUChar("landmk",  SHOW_LANDMARKS);
  g.airports     = prefs.getUChar("airpt",   SHOW_AIRPORTS);
  g.airportNames = prefs.getUChar("apname",  AIRPORT_NAMES);
  g.routes       = prefs.getUChar("routes",  ROUTES_DEFAULT);
  prefs.end();
}

void saveSettings() {
  prefs.begin("flightradar", false);           // read-write
  prefs.putString("ssid",  g.ssid);
  prefs.putString("pass",  g.pass);
  prefs.putString("host",  g.apiHost);
  prefs.putString("label", g.label);
  prefs.putFloat ("lat",   g.homeLat);
  prefs.putFloat ("lon",   g.homeLon);
  prefs.putInt   ("range", g.defaultRange);
  prefs.putUInt  ("refresh", g.refreshMs);
  prefs.putLong  ("gmt",   g.gmtOffset);
  prefs.putInt   ("dst",   g.dstOffset);
  prefs.putUChar ("metric", g.useMetric);
  prefs.putUChar ("hidegnd", g.hideGround);
  prefs.putUChar ("keeptaxi",g.gndKeepTaxi);
  prefs.putUChar ("taxikt",  g.gndTaxiKt);
  prefs.putUChar ("sweep",   g.sweep);
  prefs.putUChar ("landmk",  g.landmarks);
  prefs.putUChar ("airpt",   g.airports);
  prefs.putUChar ("apname",  g.airportNames);
  prefs.putUChar ("routes",  g.routes);
  prefs.end();
}

// -----------------------------------------------------------------------------
//  Screen dimensions (set by the board's displayBegin) + web server
// -----------------------------------------------------------------------------
int SCR_W = 320;   // overwritten in displayBegin()
int SCR_H = 240;

WebServer server(80);
DNSServer dns;
bool apMode = false;             // true while the Wi-Fi setup portal is active
volatile bool otaActive = false; // true while a web firmware upload is in progress
                                 // (freezes the radar render so it can't repaint
                                 //  over the OTA status screen)
volatile bool portalRequested = false;     // CFG tapped: netTask starts the portal
                                            // safely (Wi-Fi is owned by core 0)
volatile bool portalExitRequested = false; // BACK tapped: netTask leaves the portal

// -----------------------------------------------------------------------------
//  State
// -----------------------------------------------------------------------------
// 250 nm is intentionally omitted: at that range the aircraft JSON can exceed
// 250 KB, which buffers into a String and drops free RAM low enough to starve
// the next HTTPS (landmark) fetch of TLS memory.
const int   rangeSteps[]  = {25, 50, 100, 150};
const int   numRanges     = sizeof(rangeSteps) / sizeof(rangeSteps[0]);
int         rangeIdx      = 1;            // index into rangeSteps
int         rangeNm       = DEFAULT_RANGE_NM;

unsigned long lastFetch   = 0;
bool          firstFetch  = false;
String        statusMsg   = "Starting...";
bool          dataOk      = false;
int           sweepAngle  = 0;
// Phase reference for the sweep arm: one full revolution takes exactly one data
// refresh interval (g.refreshMs), and the arm is reset to the top (0 deg) the moment
// fresh aircraft data lands - so it scans round and "completes" just as the next pull
// is due, like a real PPI scope refreshing its blips. Set by netTask after a fetch.
volatile unsigned long gSweepEpochMs = 0;
// Repaint-on-change: when the sweep is off the render loop only redraws+pushes the
// canvas when something actually changed (new data, a tap, etc.) instead of 30x/s,
// which removes the per-frame PSRAM copy that contends with the panel scanout. Set
// true by netTask (new data) and handleTouch (selection/range), plus a 1 Hz
// heartbeat keeps the clock/status fresh. (With the sweep on we still draw every
// frame so the arm animates.)
volatile bool gNeedRedraw = true;
int           selected    = -1;           // index of selected aircraft, -1 = none
char          selectedHex[8] = "";         // hex of the selection, so it survives
                                           // list re-sorts across refreshes

// Full aircraft type for the selected flight, looked up on demand from adsbdb.com
// by Mode-S hex (e.g. "Boeing 777-236ER"). The ADS-B feed only carries the short
// ICAO type code (e.g. "B772"), which is what the on-scope tags keep showing.
char typeHex[8]      = "";                  // hex the full type below belongs to
char typeFull[40]    = "";                  // "Manufacturer model", else ""
char typeOwner[36]   = "";                  // registered owner / airline, else ""
int  typeState       = 0;                  // 0=none 1=pending 2=ok 3=unknown
                                            // 4=transient fail, retry pending
unsigned long typeRetryAt = 0;             // earliest millis() to retry a fail
#define TYPE_RETRY_MS  30000UL             // back-off after a transient failure

// Small RAM cache of resolved aircraft types, keyed by Mode-S hex, so a given
// airframe is only looked up from adsbdb once per power cycle (the feed reuses
// the same hexes every refresh). Ring-buffer eviction; resolved entries only
// (state 2 = known, 3 = unknown/404 - both worth remembering to avoid refetch).
struct TypeCacheEntry { char hex[8]; char full[40]; char owner[36]; uint8_t state; };
#define TYPE_CACHE_MAX 40
TypeCacheEntry typeCache[TYPE_CACHE_MAX];
int typeCacheCount = 0;                     // entries in use (grows to MAX)
int typeCacheNext  = 0;                     // next slot to overwrite once full

// Route (origin > destination) for the focused flight, looked up by callsign from
// adsb.lol's routeset endpoint. We send the plane's live position with the query;
// the API returns a "plausible" flag that cross-checks the route against that
// position, and we only accept plausible routes - which is what makes this usable
// where the old adsbdb route data (no position check) was too often plain wrong.
#define ROUTE_PLACE_LEN 28                  // holds a city name ("New York", "Munich")
char routeFlight[10] = "";                  // callsign the route below belongs to
char routeFrom[ROUTE_PLACE_LEN] = "";       // origin town (falls back to IATA/ICAO)
char routeTo[ROUTE_PLACE_LEN]   = "";       // destination town (falls back to IATA/ICAO)
float routeLat = 0, routeLon = 0;           // focused plane position for the query
int  routeState  = 0;                       // 0=none 1=pending 2=ok 3=none/implausible
                                            // 4=transient fail, retry pending
unsigned long routeRetryAt = 0;             // earliest millis() to retry a fail
#define ROUTE_RETRY_MS 30000UL              // back-off after a transient failure

// RAM cache of resolved routes, keyed by callsign (the feed reuses callsigns each
// refresh, so a flight is only looked up once). Same ring-buffer scheme as types;
// state 2 (known) and 3 (no plausible route) are both cached to avoid refetching.
struct RouteCacheEntry { char flight[10]; char from[ROUTE_PLACE_LEN]; char to[ROUTE_PLACE_LEN]; uint8_t state; };
#define ROUTE_CACHE_MAX 40
RouteCacheEntry routeCache[ROUTE_CACHE_MAX];
int routeCacheCount = 0;
int routeCacheNext  = 0;

// Radar geometry (computed by the board's displayBegin)
int cx, cy, radarR;

// -----------------------------------------------------------------------------
//  Dynamic map overlay (coastline) - fetched from OpenStreetMap for the
//  configured home location, so the radar map is correct wherever you are.
//  Points are stored as one flat polyline list; penUp marks the first vertex
//  of each separate coastline way (so we don't draw a line between ways).
// -----------------------------------------------------------------------------
#if SHOW_LANDMARKS
#ifndef LM_MAX_PTS
#define LM_MAX_PTS 1200           // ~14 KB RAM; sized so a full 90 nm coastline
#endif                            //   fits without hitting the cap (which truncates)
struct LmPoint { float lat, lon; bool penUp; };
LmPoint       lmPts[LM_MAX_PTS];
int           lmCount    = 0;
// Cached screen projection of lmPts[] (filled by drawLandmarks only when the view
// changes). Projecting 1200 coastline points with haversine/bearing trig EVERY
// frame was saturating the CPU/PSRAM and starving the RGB scanout (frame roll).
// The coastline only moves when the range or home changes, so we project once and
// reuse. lmGen is bumped whenever lmPts[] is replaced so the cache reprojects.
int16_t       lmScrX[LM_MAX_PTS];
int16_t       lmScrY[LM_MAX_PTS];
volatile uint32_t lmGen = 0;      // incremented on every lmPts[] update
volatile bool lmLoaded   = false; // false while (re)building the cache
float         lmCacheLat = 1e9f, lmCacheLon = 1e9f; // centre the cache was built for
int           lmStoredR  = 0;     // fetch radius (nm) the cache was built for
bool          lmFsOk     = false; // LittleFS mounted? (else cache is RAM-only)
volatile bool lmForceRefresh = false; // web "Refresh map" tapped: redownload now
#define       LM_STORE_VER  4     // bump if the on-flash format changes
                                  // (v4: discard caches built before the
                                  //  truncation fix - older fetches stopped at
                                  //  the byte/point cap and missed the local
                                  //  coast, then were wrongly kept as complete)
#define       LM_MAGIC      0x314D4C43UL  // "CLM1" - coastline cache file magic
#define       LM_CACHE_FILE "/coast.bin"
#endif

// Touch edge detection + button rectangles (set in the board's drawPanel)
bool wasTouched = false;
int  btnRngX, btnRngW, btnRngDnX, btnRngDnW, btnCfgX, btnCfgW, btnY, btnH;
int  cfgBackX, cfgBackY, cfgBackW, cfgBackH;   // "BACK" button on the portal screen

// Forward declarations of the board UI (defined in board_*.h, included below).
void drawScreen();
void drawConfigPortalScreen();
void drawSplashScreen(const char* status);
void otaBlankScreen();
void drawOtaScreen(const char* msg, uint16_t col);
void startConfigPortal();
void requestConfigPortal();
void requestConfigExit();

// -----------------------------------------------------------------------------
//  Dual-core data guard
//  Networking runs in a background task on core 0; the display + touch render
//  loop runs on core 1. This mutex protects the shared aircraft/trail/status/
//  type data so the two cores never read & write it at the same time. The
//  network task only holds it for the brief moment it swaps in fresh data, so
//  the render loop stays smooth even while a fetch is in flight.
// -----------------------------------------------------------------------------
SemaphoreHandle_t gDataMux = nullptr;
TaskHandle_t      netTaskHandle = nullptr;
// Recursive so a locked section can safely call a helper that also locks
// (e.g. handleTouch -> startConfigPortal -> setStatus).
#define LOCK()    do { if (gDataMux) xSemaphoreTakeRecursive(gDataMux, portMAX_DELAY); } while (0)
#define UNLOCK()  do { if (gDataMux) xSemaphoreGiveRecursive(gDataMux); } while (0)

// Set the on-screen status message and mirror it to the serial monitor, so the
// LCD and UART always tell the same story. Safe to call from either core.
void setStatus(const String& s) {
  LOCK();
  statusMsg = s;
  UNLOCK();
  DBGLN("[status] " + s);
}

// -----------------------------------------------------------------------------
//  Geo maths
// -----------------------------------------------------------------------------
static inline float toRad(float d) { return d * 0.01745329252f; }

float haversineNm(float lat1, float lon1, float lat2, float lon2) {
  const float R = 3440.065f;  // earth radius in nautical miles
  float dLat = toRad(lat2 - lat1);
  float dLon = toRad(lon2 - lon1);
  float a = sinf(dLat / 2) * sinf(dLat / 2) +
            cosf(toRad(lat1)) * cosf(toRad(lat2)) *
            sinf(dLon / 2) * sinf(dLon / 2);
  return R * 2 * atan2f(sqrtf(a), sqrtf(1 - a));
}

float bearingDeg(float lat1, float lon1, float lat2, float lon2) {
  float y = sinf(toRad(lon2 - lon1)) * cosf(toRad(lat2));
  float x = cosf(toRad(lat1)) * sinf(toRad(lat2)) -
            sinf(toRad(lat1)) * cosf(toRad(lat2)) * cosf(toRad(lon2 - lon1));
  float b = atan2f(y, x) * 57.2957795f;
  if (b < 0) b += 360.0f;
  return b;
}

// -----------------------------------------------------------------------------
//  Unit helpers (driven by the saved setting)
// -----------------------------------------------------------------------------
int   altOut(int ft)   { return g.useMetric ? (int)(ft * 0.3048f) : ft; }
int   spdOut(int kts)  { return g.useMetric ? (int)(kts * 1.852f)  : kts; }
float distOut(float nm){ return g.useMetric ? nm * 1.852f          : nm; }
const char* altUnit()  { return g.useMetric ? "m"   : "ft"; }
const char* spdUnit()  { return g.useMetric ? "kmh" : "kt"; }
const char* distUnit() { return g.useMetric ? "km"  : "nm"; }

// Colour by altitude band (RGB565; literal values so this stays independent of
// whichever graphics library the selected board pulls in).
uint16_t altColour(const Aircraft& a) {
  if (a.onGround)     return 0x7BEF;   // dark grey
  if (a.alt < 3000)   return 0xF800;   // red
  if (a.alt < 10000)  return 0xFDA0;   // orange
  if (a.alt < 20000)  return 0xFFE0;   // yellow
  if (a.alt < 30000)  return 0x07E0;   // green
  return 0x07FF;                       // cyan
}

// Map an aircraft's distance/bearing to a pixel on the radar scope.
void planeToScreen(const Aircraft& a, int& sx, int& sy) {
  float r = (a.distNm / (float)rangeNm) * radarR;
  if (r > radarR) r = radarR;
  float ang = toRad(a.bearing);
  sx = cx + (int)(r * sinf(ang));
  sy = cy - (int)(r * cosf(ang));
}

// Map an arbitrary lat/lon (e.g. a stored trail point) to a radar pixel.
void geoToScreen(float lat, float lon, int& sx, int& sy) {
  float dist = haversineNm(g.homeLat, g.homeLon, lat, lon);
  float brg  = bearingDeg(g.homeLat, g.homeLon, lat, lon);
  float r = (dist / (float)rangeNm) * radarR;
  if (r > radarR) r = radarR;
  float ang = toRad(brg);
  sx = cx + (int)(r * sinf(ang));
  sy = cy - (int)(r * cosf(ang));
}

// Like geoToScreen but does NOT clamp to the range ring, so map lines that run
// off the edge of the scope can be clipped geometrically (see clipSegToRadar).
void geoToScreenRaw(float lat, float lon, float& sx, float& sy) {
  float dist = haversineNm(g.homeLat, g.homeLon, lat, lon);
  float brg  = bearingDeg(g.homeLat, g.homeLon, lat, lon);
  float r = (dist / (float)rangeNm) * radarR;
  float ang = toRad(brg);
  sx = (float)cx + r * sinf(ang);
  sy = (float)cy - r * cosf(ang);
}

// Clip the segment (x0,y0)-(x1,y1) to the radar disc (centre cx,cy radius radarR).
// Returns true and the clipped endpoints if any part lies inside the scope.
bool clipSegToRadar(float x0, float y0, float x1, float y1,
                    int& ox0, int& oy0, int& ox1, int& oy1) {
  float dx = x1 - x0, dy = y1 - y0;
  float fx = x0 - (float)cx, fy = y0 - (float)cy;
  float A  = dx * dx + dy * dy;
  float C  = fx * fx + fy * fy - (float)radarR * (float)radarR;
  float t0 = 0.0f, t1 = 1.0f;
  if (A < 1e-6f) {                 // zero-length segment
    if (C > 0.0f) return false;    // a single point outside the disc
  } else {
    float B    = 2.0f * (fx * dx + fy * dy);
    float disc = B * B - 4.0f * A * C;
    if (disc < 0.0f) {             // line misses the circle entirely
      if (C > 0.0f) return false;  // ...and start is outside => fully outside
    } else {                       // clamp the inside interval to [0,1]
      float sq = sqrtf(disc);
      float ta = (-B - sq) / (2.0f * A);
      float tb = (-B + sq) / (2.0f * A);
      if (ta > t0) t0 = ta;
      if (tb < t1) t1 = tb;
      if (t0 > t1) return false;   // segment grazes past the disc
    }
  }
  ox0 = (int)(x0 + t0 * dx); oy0 = (int)(y0 + t0 * dy);
  ox1 = (int)(x0 + t1 * dx); oy1 = (int)(y0 + t1 * dy);
  return true;
}

// Scale an RGB565 colour's brightness by num/den (for fading trail dots).
uint16_t dimColour(uint16_t c, int num, int den) {
  if (den <= 0) return c;
  int r = (c >> 11) & 0x1F, gg = (c >> 5) & 0x3F, b = c & 0x1F;
  r = r * num / den; gg = gg * num / den; b = b * num / den;
  return (uint16_t)((r << 11) | (gg << 5) | b);
}

// Find the stored trail for an ICAO hex, or -1.
int findTrail(const char* hex) {
  for (int i = 0; i < trailCount; i++)
    if (strcmp(trails[i].hex, hex) == 0) return i;
  return -1;
}

// Rebuild the trail set from the latest acList: carry over history for aircraft
// still present (append the new fix), start a fresh trail for new ones, and
// drop trails for aircraft that have left the list.
void updateTrails() {
  static Trail next[MAX_AC];
  int n = 0;
  for (int i = 0; i < acCount; i++) {
    Trail& nt = next[n++];
    int t = findTrail(acList[i].hex);
    if (t >= 0) {
      nt = trails[t];                         // keep existing history
      if (nt.count < TRAIL_LEN) {
        nt.lat[nt.count] = acList[i].lat;
        nt.lon[nt.count] = acList[i].lon;
        nt.count++;
      } else {                                // full: slide window, append newest
        for (int k = 1; k < TRAIL_LEN; k++) {
          nt.lat[k - 1] = nt.lat[k];
          nt.lon[k - 1] = nt.lon[k];
        }
        nt.lat[TRAIL_LEN - 1] = acList[i].lat;
        nt.lon[TRAIL_LEN - 1] = acList[i].lon;
      }
    } else {
      strlcpy(nt.hex, acList[i].hex, sizeof(nt.hex));
      nt.count = 1;
      nt.lat[0] = acList[i].lat;
      nt.lon[0] = acList[i].lon;
    }
  }
  for (int i = 0; i < n; i++) trails[i] = next[i];
  trailCount = n;
}

// -----------------------------------------------------------------------------
//  Wi-Fi (station mode)
// -----------------------------------------------------------------------------
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  setStatus("Connecting WiFi...");
  DBG("Connecting to SSID '%s'...\n", g.ssid);
  if (!apMode) drawSplashScreen("Connecting Wi-Fi...");  // keep the boot splash up
  WiFi.mode(WIFI_STA);
  WiFi.begin(g.ssid, g.pass);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 18000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    setStatus("WiFi OK");
    DBG("WiFi connected: IP %s, RSSI %d dBm\n",
        WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTime(g.gmtOffset, g.dstOffset, NTP_SERVER);
    return true;
  }
  setStatus("WiFi FAILED");
  DBG("WiFi connect failed (status %d)\n", WiFi.status());
  return false;
}

// -----------------------------------------------------------------------------
//  Fetch + parse aircraft
// -----------------------------------------------------------------------------
void fetchAircraft() {
  if (WiFi.status() != WL_CONNECTED) { dataOk = false; return; }

  char url[160];
  snprintf(url, sizeof(url), "https://%s/v2/point/%.4f/%.4f/%d",
           g.apiHost, (double)g.homeLat, (double)g.homeLon, rangeNm);
  DBG("GET %s\n", url);

  WiFiClientSecure client;
  client.setInsecure();              // these public APIs don't need cert pinning
  client.setTimeout(8);

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.useHTTP10(true);          // simpler stream for ArduinoJson
  if (!http.begin(client, url)) {
    setStatus("HTTP begin fail");
    dataOk = false;
    return;
  }
  http.addHeader("Accept", "application/json");
  http.setUserAgent("ESP32-FlightRadar/1.0");

  int code = http.GET();
  DBG("HTTP status %d\n", code);

  // A negative code is a connection/TLS-level failure (e.g. -1 = refused,
  // -11 = read timeout), usually a transient hiccup or a momentary low-heap
  // moment for the TLS buffers. Free the socket, let the heap settle, and try
  // once more before giving up for this cycle.
  if (code < 0) {
    DBG("HTTP %d (free heap %u) - retrying once\n", code, ESP.getFreeHeap());
    http.end();
    delay(1500);
    if (http.begin(client, url)) {
      http.addHeader("Accept", "application/json");
      http.setUserAgent("ESP32-FlightRadar/1.0");
      code = http.GET();
      DBG("HTTP status %d (retry)\n", code);
    }
  }

  if (code != HTTP_CODE_OK) {
    setStatus(String("HTTP ") + code);
    dataOk = false;
    http.end();
    return;
  }

  // Only pull the fields we actually use -> tiny memory footprint.
  // For an array, element [0] of the filter describes every element.
  JsonDocument filter;
  JsonObject fac = filter["ac"][0].to<JsonObject>();
  fac["hex"]       = true;
  fac["flight"]    = true;
  fac["r"]         = true;
  fac["t"]         = true;
  fac["lat"]       = true;
  fac["lon"]       = true;
  fac["alt_baro"]  = true;
  fac["gs"]        = true;
  fac["track"]     = true;
  fac["baro_rate"] = true;
  fac["dbFlags"]   = true;

  // Pull the whole body first, THEN parse. Parsing straight off the TLS stream
  // can fail with "IncompleteInput": on a slow HTTPS connection ArduinoJson can
  // momentarily see zero bytes available and treat that as end-of-input before
  // the full response has arrived. getString() blocks until the transfer is
  // really finished (and de-chunks if needed). The field filter still keeps the
  // parsed document tiny.
  String payload = http.getString();
  http.end();
  DBG("Payload %u bytes\n", (unsigned)payload.length());

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));

  if (err) {
    setStatus(String("JSON ") + err.c_str());
    dataOk = false;
    return;
  }

  // Parse into a private buffer first; we only grab the lock to swap it in, so
  // the slow network + JSON work above never blocks the render loop on core 1.
  static Aircraft parseBuf[MAX_AC];
  int pCount = 0;
  JsonArray ac = doc["ac"].as<JsonArray>();
  for (JsonObject o : ac) {
    if (pCount >= MAX_AC) break;
    if (o["lat"].isNull() || o["lon"].isNull()) continue;

    Aircraft& a = parseBuf[pCount];
    strlcpy(a.hex, o["hex"] | "", sizeof(a.hex));

    String fl = String((const char*)(o["flight"] | ""));
    fl.trim();
    // Skip non-aircraft feed artifacts (e.g. "TXLU00") - these are TIS-B/ground
    // entries, not real aircraft, and shouldn't appear on the scope.
    if (fl.startsWith("TXLU")) continue;
    strlcpy(a.flight, fl.length() ? fl.c_str() : a.hex, sizeof(a.flight));

    strlcpy(a.type, o["t"] | "", sizeof(a.type));
    strlcpy(a.reg,  o["r"] | "", sizeof(a.reg));

    a.lat = o["lat"].as<float>();
    a.lon = o["lon"].as<float>();

    // alt_baro can be the string "ground"
    a.onGround = false;
    if (o["alt_baro"].is<const char*>()) {
      a.onGround = (strcmp(o["alt_baro"] | "", "ground") == 0);
      a.alt = 0;
    } else {
      a.alt = o["alt_baro"] | 0;
    }
    a.gs    = (int)(o["gs"] | 0.0f);

    // Optional filter: drop ground traffic so the scope shows airborne aircraft
    // only. With "keep taxiing" on, ground aircraft moving faster than the taxi
    // threshold are kept (so a plane rolling for takeoff stays visible) while
    // parked/slow ones are still dropped.
    if (g.hideGround && a.onGround &&
        !(g.gndKeepTaxi && a.gs >= (int)g.gndTaxiKt)) continue;

    a.track = o["track"].isNull() ? -1 : (int)(o["track"].as<float>());
    a.vrate = o["baro_rate"] | 0;
    a.dbFlags = (uint8_t)(o["dbFlags"] | 0);

    a.distNm  = haversineNm(g.homeLat, g.homeLon, a.lat, a.lon);
    a.bearing = bearingDeg(g.homeLat, g.homeLon, a.lat, a.lon);
#if DEBUG_AIRCRAFT
    DBG("  %-8s %-8s alt=%-6d gs=%-4d trk=%-4d dist=%.1f brg=%.0f\n",
        a.hex, a.flight, a.alt, a.gs, a.track, (double)a.distNm, (double)a.bearing);
#endif
    pCount++;
  }

  // Sort by distance (simple insertion sort, list is small).
  for (int i = 1; i < pCount; i++) {
    Aircraft key = parseBuf[i];
    int j = i - 1;
    while (j >= 0 && parseBuf[j].distNm > key.distNm) {
      parseBuf[j + 1] = parseBuf[j];
      j--;
    }
    parseBuf[j + 1] = key;
  }

  // Publish the new snapshot. Brief lock, no slow work inside.
  LOCK();
  memcpy(acList, parseBuf, sizeof(Aircraft) * pCount);
  acCount  = pCount;
  updateTrails();   // position history (keyed by hex), rebuilt from acList
  // Re-resolve the selection by hex. The list re-sorts every refresh, so the
  // old index is stale - we find the same airframe again and keep it selected,
  // dropping it only when it leaves the feed or moves beyond range (off scope).
  // This keeps a tapped plane pinned in the detail panel instead of clearing
  // every refresh; the user clears it by tapping empty space or another plane.
  selected = -1;
  if (selectedHex[0]) {
    for (int i = 0; i < acCount; i++) {
      if (strcmp(acList[i].hex, selectedHex) == 0) {
        if (acList[i].distNm <= rangeNm) selected = i;   // still on the scope
        break;
      }
    }
    if (selected < 0) selectedHex[0] = '\0';   // gone / off-radar -> deselect
  }
  dataOk   = true;
  UNLOCK();

  setStatus(String(pCount) + " aircraft");
  DBG("Fetched %d aircraft, free heap %u bytes\n", pCount, ESP.getFreeHeap());
}

// -----------------------------------------------------------------------------
//  UTF-8 -> ASCII transliteration. The built-in fonts only cover ASCII, so
//  accented place names (e.g. "Montréal") would otherwise show a stray glyph.
//  We fold the common Latin-1 Supplement and Latin Extended-A letters down to
//  their closest ASCII form ("Montreal"); anything unknown becomes '?'.
// -----------------------------------------------------------------------------
static const char* asciiForCp(uint32_t cp) {
  switch (cp) {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return "A";
    case 0xC6: return "AE";
    case 0xC7: return "C";
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: return "E";
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: return "I";
    case 0xD0: return "D";
    case 0xD1: return "N";
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return "O";
    case 0xD9: case 0xDA: case 0xDB: case 0xDC: return "U";
    case 0xDD: return "Y";
    case 0xDE: return "Th";
    case 0xDF: return "ss";
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return "a";
    case 0xE6: return "ae";
    case 0xE7: return "c";
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: return "e";
    case 0xEC: case 0xED: case 0xEE: case 0xEF: return "i";
    case 0xF0: return "d";
    case 0xF1: return "n";
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: return "o";
    case 0xF9: case 0xFA: case 0xFB: case 0xFC: return "u";
    case 0xFD: case 0xFF: return "y";
    case 0xFE: return "th";
    // Latin Extended-A (common Central/Eastern European letters)
    case 0x100: case 0x102: case 0x104: return "A";
    case 0x101: case 0x103: case 0x105: return "a";
    case 0x106: case 0x108: case 0x10A: case 0x10C: return "C";
    case 0x107: case 0x109: case 0x10B: case 0x10D: return "c";
    case 0x10E: case 0x110: return "D";
    case 0x10F: case 0x111: return "d";
    case 0x112: case 0x114: case 0x116: case 0x118: case 0x11A: return "E";
    case 0x113: case 0x115: case 0x117: case 0x119: case 0x11B: return "e";
    case 0x11C: case 0x11E: case 0x120: case 0x122: return "G";
    case 0x11D: case 0x11F: case 0x121: case 0x123: return "g";
    case 0x124: case 0x126: return "H";
    case 0x125: case 0x127: return "h";
    case 0x128: case 0x12A: case 0x12C: case 0x12E: case 0x130: return "I";
    case 0x129: case 0x12B: case 0x12D: case 0x12F: case 0x131: return "i";
    case 0x139: case 0x13B: case 0x13D: case 0x13F: case 0x141: return "L";
    case 0x13A: case 0x13C: case 0x13E: case 0x140: case 0x142: return "l";
    case 0x143: case 0x145: case 0x147: return "N";
    case 0x144: case 0x146: case 0x148: return "n";
    case 0x14C: case 0x14E: case 0x150: return "O";
    case 0x14D: case 0x14F: case 0x151: return "o";
    case 0x152: return "OE";
    case 0x153: return "oe";
    case 0x154: case 0x156: case 0x158: return "R";
    case 0x155: case 0x157: case 0x159: return "r";
    case 0x15A: case 0x15C: case 0x15E: case 0x160: return "S";
    case 0x15B: case 0x15D: case 0x15F: case 0x161: return "s";
    case 0x162: case 0x164: case 0x166: return "T";
    case 0x163: case 0x165: case 0x167: return "t";
    case 0x168: case 0x16A: case 0x16C: case 0x16E: case 0x170: case 0x172: return "U";
    case 0x169: case 0x16B: case 0x16D: case 0x16F: case 0x171: case 0x173: return "u";
    case 0x174: return "W"; case 0x175: return "w";
    case 0x176: case 0x178: return "Y"; case 0x177: return "y";
    case 0x179: case 0x17B: case 0x17D: return "Z";
    case 0x17A: case 0x17C: case 0x17E: return "z";
    default: return "?";
  }
}

// Fold a UTF-8 string into ASCII in `out` (NUL-terminated, bounded by outSize).
void asciiFold(const char* in, char* out, size_t outSize) {
  size_t o = 0;
  const unsigned char* p = (const unsigned char*)in;
  while (*p && o + 1 < outSize) {
    unsigned char c = *p;
    if (c < 0x80) { out[o++] = (char)c; p++; continue; }   // plain ASCII
    uint32_t cp = 0; int extra = 0;
    if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { p++; continue; }                                 // stray byte: drop
    p++;
    while (extra-- > 0 && (*p & 0xC0) == 0x80) { cp = (cp << 6) | (*p & 0x3F); p++; }
    for (const char* r = asciiForCp(cp); *r && o + 1 < outSize; r++) out[o++] = *r;
  }
  out[o] = '\0';
}

// Find a cached type by hex. Returns its index or -1. Caller must hold the lock.
int typeCacheFind(const char* hex) {
  for (int i = 0; i < typeCacheCount; i++)
    if (strcmp(typeCache[i].hex, hex) == 0) return i;
  return -1;
}

// Insert/update a resolved type in the cache. Caller must hold the lock.
void typeCachePut(const char* hex, const char* full, const char* owner, uint8_t state) {
  if (!hex || !hex[0]) return;
  int i = typeCacheFind(hex);
  if (i < 0) {                              // allocate a slot (grow, then ring)
    if (typeCacheCount < TYPE_CACHE_MAX) i = typeCacheCount++;
    else { i = typeCacheNext; typeCacheNext = (typeCacheNext + 1) % TYPE_CACHE_MAX; }
    strlcpy(typeCache[i].hex, hex, sizeof(typeCache[i].hex));
  }
  strlcpy(typeCache[i].full,  full  ? full  : "", sizeof(typeCache[i].full));
  strlcpy(typeCache[i].owner, owner ? owner : "", sizeof(typeCache[i].owner));
  typeCache[i].state = state;
}

// Look up the full aircraft type ("Boeing 777-236ER") for the selected flight
// from adsbdb.com by Mode-S hex. Resolves typeHex -> typeFull. Runs on the net
// task (core 0). Results are cached in RAM so each airframe is only fetched once.
void fetchAircraftType() {
  char hx[8];
  LOCK();
  strlcpy(hx, typeHex, sizeof(hx));
  UNLOCK();

  char nFull[40]  = "";
  char nOwner[36] = "";
  int  nState     = 3;                  // n/a (used only for a definitive answer)
  bool transient  = true;               // assume "try again later" unless we get
                                        // a definitive 200-with-data or a 404

  if (WiFi.status() == WL_CONNECTED && strlen(hx) > 0) {
    char url[96];
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/aircraft/%s", hx);
    DBG("TYPE GET %s\n", url);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(6);

    HTTPClient http;
    http.setConnectTimeout(6000);
    http.setTimeout(6000);
    http.useHTTP10(true);
    if (http.begin(client, url)) {
      http.setUserAgent("ESP32-FlightRadar/1.0");
      int code = http.GET();
      DBG("TYPE HTTP %d\n", code);
      if (code == HTTP_CODE_OK) {
        JsonDocument filter;
        JsonObject fa = filter["response"]["aircraft"].to<JsonObject>();
        fa["manufacturer"]     = true;
        fa["type"]             = true;
        fa["registered_owner"] = true;

        String payload = http.getString();
        http.end();

        JsonDocument doc;
        DeserializationError err =
            deserializeJson(doc, payload, DeserializationOption::Filter(filter));
        if (!err) {
          transient = false;            // got a parseable answer - definitive
          JsonObject ac = doc["response"]["aircraft"];
          if (!ac.isNull()) {
            const char* mfr = ac["manufacturer"] | "";
            const char* mdl = ac["type"]         | "";
            char raw[64] = "";
            if (strlen(mfr) && strlen(mdl))
              snprintf(raw, sizeof(raw), "%s %s", mfr, mdl);
            else if (strlen(mdl))
              strlcpy(raw, mdl, sizeof(raw));
            else if (strlen(mfr))
              strlcpy(raw, mfr, sizeof(raw));
            asciiFold(raw, nFull, sizeof(nFull));   // ASCII-only for the built-in font
            const char* owner = ac["registered_owner"] | "";
            if (strlen(owner)) asciiFold(owner, nOwner, sizeof(nOwner));
            if (strlen(nFull) || strlen(nOwner)) nState = 2;   // got something useful
          }
        }
        // err (200 but unparseable / partial body) -> leave transient = true
      } else if (code == HTTP_CODE_NOT_FOUND) {   // 404: genuinely not in the DB
        http.end();
        transient = false;              // permanent "unknown" - don't keep asking
      } else {
        http.end();                     // 202 (queued), 429 (rate-limit), 5xx,
                                        // negative (TLS/timeout): transient, retry
      }
    }
  }

  // Publish the outcome. A transient failure is deliberately NOT cached, so the
  // airframe stays unresolved and gets another go after a short back-off rather
  // than being remembered forever as "unknown".
  LOCK();
  if (transient) {
    typeRetryAt = millis() + TYPE_RETRY_MS;
    if (strcmp(typeHex, hx) == 0) typeState = 4;   // 4 = transient, retry pending
    DBG("TYPE %s transient fail - retry in %lus\n", hx, (unsigned long)(TYPE_RETRY_MS / 1000));
  } else {
    typeCachePut(hx, nFull, nOwner, (uint8_t)nState);
    if (strcmp(typeHex, hx) == 0) {
      strlcpy(typeFull,  nFull,  sizeof(typeFull));
      strlcpy(typeOwner, nOwner, sizeof(typeOwner));
      typeState = nState;
      DBG("TYPE %s = '%s' (%s)\n", hx, typeFull, nOwner);
    }
  }
  UNLOCK();
}

#if SHOW_ROUTES
// Find a cached route by callsign. Returns its index or -1. Caller holds the lock.
int routeCacheFind(const char* cs) {
  for (int i = 0; i < routeCacheCount; i++)
    if (strcmp(routeCache[i].flight, cs) == 0) return i;
  return -1;
}

// Insert/update a resolved route in the cache. Caller holds the lock.
void routeCachePut(const char* cs, const char* from, const char* to, uint8_t state) {
  if (!cs || !cs[0]) return;
  int i = routeCacheFind(cs);
  if (i < 0) {
    if (routeCacheCount < ROUTE_CACHE_MAX) i = routeCacheCount++;
    else { i = routeCacheNext; routeCacheNext = (routeCacheNext + 1) % ROUTE_CACHE_MAX; }
    strlcpy(routeCache[i].flight, cs, sizeof(routeCache[i].flight));
  }
  strlcpy(routeCache[i].from, from ? from : "", sizeof(routeCache[i].from));
  strlcpy(routeCache[i].to,   to   ? to   : "", sizeof(routeCache[i].to));
  routeCache[i].state = state;
}

// Heuristic: is this aircraft likely to fly a scheduled route worth querying? Skips
// military and privacy-blocked airframes (via the feed's dbFlags) and GA/private
// traffic flying under its own registration (callsign == registration). Avoids
// pointless lookups - and the server-side 500s - on aircraft that never have a route.
bool routeWorthLooking(const Aircraft* a) {
  if (a->dbFlags & 0x01) return false;    // military
  if (a->dbFlags & 0x04) return false;    // PIA  (privacy ICAO address)
  if (a->dbFlags & 0x08) return false;    // LADD (limited aircraft data display)
  if (strlen(a->reg) && strcasecmp(a->flight, a->reg) == 0) return false;  // GA: callsign == reg
  return true;
}

// Shorten an airport name to something recognisable by dropping generic words
// ("International", "Airport", ...) and ASCII-folding. "Teesside International
// Airport" -> "Teesside", "Amsterdam Airport Schiphol" -> "Amsterdam Schiphol".
void airportShortName(const char* raw, char* out, size_t outSize) {
  char folded[56];
  asciiFold(raw, folded, sizeof(folded));      // strip accents first
  static const char* kDrop[] = {
    "Airport", "International", "Intl", "Regional", "Municipal",
    "Airfield", "Aerodrome", "Apt", "Intern"
  };
  size_t ol = 0;
  char* save = nullptr;
  for (char* tok = strtok_r(folded, " ", &save); tok; tok = strtok_r(nullptr, " ", &save)) {
    bool drop = false;
    for (const char* d : kDrop) if (strcasecmp(tok, d) == 0) { drop = true; break; }
    if (drop || !*tok) continue;
    if (ol && ol + 1 < outSize) out[ol++] = ' ';
    for (char* p = tok; *p && ol + 1 < outSize; p++) out[ol++] = *p;
  }
  out[ol] = '\0';
}

// Best display name for a route airport: prefer the (shortened) airport name, then
// the town, then the IATA/ICAO code. Result is ASCII-folded for the built-in font.
void airportPlace(JsonObject ap, char* out, size_t outSize) {
  out[0] = '\0';
  const char* nm = ap["name"] | "";
  if (strlen(nm)) { airportShortName(nm, out, outSize); if (strlen(out)) return; }
  const char* loc = ap["location"] | "";
  if (strlen(loc)) { asciiFold(loc, out, outSize); return; }
  const char* code = ap["iata"] | "";
  if (!strlen(code)) code = ap["icao"] | "";
  strlcpy(out, code, outSize);
}

// Look up the route (origin > destination) for the focused flight from adsb.lol's
// routeset endpoint. Resolves routeFlight -> routeFrom/routeTo. Runs on the net
// task (core 0). POSTs the callsign plus the plane's live position; only routes
// the API marks "plausible" (i.e. consistent with that position) are accepted.
void fetchRoute() {
  char  cs[10];
  float la, lo;
  LOCK();
  strlcpy(cs, routeFlight, sizeof(cs));
  la = routeLat; lo = routeLon;
  UNLOCK();

  char nFrom[ROUTE_PLACE_LEN] = "";
  char nTo[ROUTE_PLACE_LEN]   = "";
  int  nState   = 3;                    // definitive "no plausible route" by default
  bool transient = true;                // unless we get a parseable answer or 404

  if (WiFi.status() == WL_CONNECTED && strlen(cs) > 0) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(6);

    HTTPClient http;
    http.setConnectTimeout(6000);
    http.setTimeout(6000);
    http.useHTTP10(true);               // same as the (working) aircraft GET
    // Use the GET route endpoint, not the POST /routeset: an edge/proxy in front of
    // adsb.lol intercepts the bare POST and returns a 201 with an empty text/html
    // body. Plain GETs to this host work fine, and this endpoint returns the same
    // single route + "plausible" flag. Path params: callsign / lat / lng.
    char url[128];
    snprintf(url, sizeof(url), "https://api.adsb.lol/api/0/route/%s/%.4f/%.4f",
             cs, (double)la, (double)lo);
    DBG("ROUTE GET %s\n", url);
    if (http.begin(client, url)) {
      http.setUserAgent("ESP32-FlightRadar/1.0");
      int code = http.GET();
      DBG("ROUTE HTTP %d\n", code);
      // A negative code is a connection/TLS hiccup (-1 refused, -11 read timeout),
      // common on a weak link. Retry once immediately, like the aircraft fetch, so a
      // momentary drop doesn't park the route in a 30 s back-off.
      if (code < 0) {
        http.end();
        delay(800);
        if (http.begin(client, url)) {
          http.setUserAgent("ESP32-FlightRadar/1.0");
          code = http.GET();
          DBG("ROUTE HTTP %d (retry)\n", code);
        }
      }
      if (code >= 200 && code < 300) {
        String payload = http.getString();
        http.end();
        DBG("ROUTE payload %d bytes\n", (int)payload.length());

        // This endpoint returns a single route OBJECT (not an array).
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (!err) {
          transient = false;            // parseable answer - definitive
          JsonObject r0 = doc.as<JsonObject>();
          bool plausible = r0["plausible"] | false;
          JsonArray aps  = r0["_airports"];
          if (plausible && !aps.isNull() && aps.size() >= 2) {
            airportPlace(aps[0], nFrom, sizeof(nFrom));            // origin
            airportPlace(aps[aps.size() - 1], nTo, sizeof(nTo));  // destination (last leg)
            if (strlen(nFrom) && strlen(nTo)) nState = 2;         // usable, plausible route
          }
          // implausible / no airports -> nState stays 3 (cached as "no route")
        } else {
          DBG("ROUTE parse err: %s (payload %d bytes)\n", err.c_str(), (int)payload.length());
        }
      } else if (code == HTTP_CODE_NOT_FOUND) {
        http.end();
        transient = false;              // permanent miss - don't keep asking
      } else {
        http.end();                     // 429/5xx/timeout: transient, retry
      }
    }
  }

  LOCK();
  if (transient) {
    routeRetryAt = millis() + ROUTE_RETRY_MS;
    if (strcmp(routeFlight, cs) == 0) routeState = 4;
    DBG("ROUTE %s transient fail - retry in %lus\n", cs, (unsigned long)(ROUTE_RETRY_MS / 1000));
  } else {
    routeCachePut(cs, nFrom, nTo, (uint8_t)nState);
    if (strcmp(routeFlight, cs) == 0) {
      strlcpy(routeFrom, nFrom, sizeof(routeFrom));
      strlcpy(routeTo,   nTo,   sizeof(routeTo));
      routeState = nState;
      DBG("ROUTE %s = '%s' > '%s'\n", cs, routeFrom, routeTo);
    }
  }
  UNLOCK();
}
#endif  // SHOW_ROUTES

// -----------------------------------------------------------------------------
//  Dynamic coastline (OpenStreetMap / Overpass API)
// -----------------------------------------------------------------------------
#if SHOW_LANDMARKS

// Coverage radius (nm) to fetch the coastline for: the currently selected range
// plus ~30% headroom, clamped. Basing it on the live range (not the default)
// means the map reaches the edge of whatever you've zoomed to; the headroom
// stops tiny zoom changes from forcing an immediate refetch.
//
// The upper clamp (90 nm) is deliberately well below the max aircraft range
// (150 nm): an Overpass coastline query for a big bbox is slow, returns a large
// payload, and frequently times out (-11) - and a payload too big to read in
// full gets truncated, dropping whole coastlines. Capping coverage keeps the
// response small enough to download completely. At the widest zoom the
// coastline simply stops a little short of the scope edge.
int lmFetchRadiusNm() { return constrain((int)(rangeNm * 1.3f), 25, 90); }

// On-flash header for the coastline cache file (followed by lmCount LmPoints).
struct LmFileHeader {
  uint32_t magic;
  uint8_t  ver;
  uint16_t count;
  float    lat, lon;
  int32_t  r;
};

// Mount LittleFS once. The coastline cache is stored as a FILE here rather than
// in NVS: an ~8 KB blob in the small (~20 KB) NVS partition starves the WiFi
// stack's RF-calibration storage ("store calibration data failed 0x1105") and
// can wedge the boot. LittleFS uses the data ("spiffs") partition, which the
// bundled partitions.csv provides (~9.8 MB). If that partition is missing (e.g.
// a stale/FATFS-only scheme) the cache stays in RAM and is refetched each boot -
// no harm. To make it persist, flash once with this sketch's partitions.csv in
// place (the Arduino IDE picks it up automatically). See README section 2.
void landmarksFsBegin() {
  lmFsOk = LittleFS.begin(true);          // format on first use if needed
  if (!lmFsOk)
    DBGLN("LANDMARKS: no LittleFS partition - coastline cached in RAM only");
}

// Persist the current coastline cache to a LittleFS file so it survives reboots.
// A 0-point result (inland) is stored too, so we don't re-query Overpass every boot.
void saveLandmarks() {
  if (!lmFsOk) return;
  File f = LittleFS.open(LM_CACHE_FILE, "w");
  if (!f) { DBGLN("LANDMARKS FS open (w) fail"); return; }
  LmFileHeader h = { LM_MAGIC, LM_STORE_VER, (uint16_t)lmCount,
                     lmCacheLat, lmCacheLon, (int32_t)lmStoredR };
  f.write((const uint8_t*)&h, sizeof(h));
  if (lmCount > 0) f.write((const uint8_t*)lmPts, (size_t)lmCount * sizeof(LmPoint));
  f.close();
  DBG("LANDMARKS saved %d points to %s\n", lmCount, LM_CACHE_FILE);
}

// Restore a previously saved coastline cache. Returns true if a valid cache was
// loaded (even an empty 'inland' one). Sets lmCacheLat/Lon and lmStoredR so the
// caller can decide whether it still matches the location/range.
bool loadLandmarks() {
  if (!lmFsOk) return false;
  File f = LittleFS.open(LM_CACHE_FILE, "r");
  if (!f) return false;

  LmFileHeader h;
  if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) ||
      h.magic != LM_MAGIC || h.ver != LM_STORE_VER || h.count > LM_MAX_PTS) {
    f.close();
    return false;
  }
  bool ok = true;
  if (h.count > 0) {
    size_t want = (size_t)h.count * sizeof(LmPoint);
    if (f.read((uint8_t*)lmPts, want) != want) ok = false;
  }
  f.close();
  if (!ok) return false;

  LOCK();
  lmCount    = h.count;
  lmGen      = lmGen + 1;  // invalidate the cached screen projection
  lmLoaded   = true;
  lmCacheLat = h.lat;
  lmCacheLon = h.lon;
  lmStoredR  = h.r;
  UNLOCK();
  DBG("LANDMARKS restored %u points from %s (centre %.3f,%.3f r=%dnm)\n",
      h.count, LM_CACHE_FILE, (double)h.lat, (double)h.lon, (int)h.r);
  return true;
}

// Percent-encode a string for an x-www-form-urlencoded POST body.
String urlEncode(const char* s) {
  String o; char b[4];
  for (const char* p = s; *p; p++) {
    char c = *p;
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      o += c;
    } else {
      snprintf(b, sizeof(b), "%%%02X", (unsigned char)c);
      o += b;
    }
  }
  return o;
}

// Set true when lmReadByte() gives up on a stalled stream (vs. a clean EOF), so
// the caller can tell a truncated download apart from a finished one and avoid
// caching a half/empty result as if it were valid.
static bool lmReadTimedOut = false;

// Block-read one byte from the TLS stream, with a timeout. Returns -1 on EOF or
// timeout; lmReadTimedOut distinguishes the two.
static int lmReadByte(WiFiClient* st) {
  unsigned long t0 = millis();
  for (;;) {
    if (st->available()) return st->read();
    if (!st->connected() && !st->available()) return -1;   // clean end of stream
    if (millis() - t0 > 8000) { lmReadTimedOut = true; return -1; }  // stalled
    delay(1);
  }
}

// Read a JSON number token from the stream (skips leading spaces). The first
// non-numeric character is consumed and discarded (it's only ever a structural
// ',' or '}', which the coastline scanner doesn't rely on).
static float lmReadNumber(WiFiClient* st) {
  char buf[24]; int n = 0; int c;
  do { c = lmReadByte(st); } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
  while (c >= 0 && (isdigit(c) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')) {
    if (n < (int)sizeof(buf) - 1) buf[n++] = (char)c;
    c = lmReadByte(st);
  }
  buf[n] = '\0';
  return atof(buf);
}

// Fetch the coastline around the home location and build the polyline cache.
// Uses a tiny streaming scanner (no full-payload buffer) so a large response
// can't exhaust the heap: it watches for the "geometry"/"lat"/"lon" keys in
// the Overpass JSON and pulls vertices straight off the socket, decimating to
// roughly one point per minSep nautical miles and capping at LM_MAX_PTS.
bool fetchLandmarks() {
  if (WiFi.status() != WL_CONNECTED) return false;

  float homeLat = g.homeLat, homeLon = g.homeLon;
  int   fetchR  = lmFetchRadiusNm();        // nm radius, with headroom for zoom
  float latM    = fetchR / 60.0f;
  float cosL    = cosf(toRad(homeLat)); if (fabsf(cosL) < 0.01f) cosL = 0.01f;
  float lonM    = fetchR / 60.0f / cosL;
  float south = homeLat - latM, north = homeLat + latM;
  float west  = homeLon - lonM, east  = homeLon + lonM;
  // Coarser decimation than before (was /350) so the full coastline within the
  // bbox fits comfortably under LM_MAX_PTS - hitting that cap mid-response
  // truncates coverage and can drop the local coast.
  float minSep = fetchR / 120.0f; if (minSep < 0.2f) minSep = 0.2f;

  char query[224];
  snprintf(query, sizeof(query),
           "[out:json][timeout:50];way[\"natural\"=\"coastline\"]"
           "(%.4f,%.4f,%.4f,%.4f);out geom;",
           (double)south, (double)west, (double)north, (double)east);

  // Public Overpass mirrors. The main server (overpass-api.de) is the busiest
  // and most likely to answer a coastline query with a server-side timeout
  // ("remark"); rotate through alternatives so a retry lands on a different,
  // hopefully less-loaded backend. The index advances every call.
  static const char* OVERPASS[] = {
    "https://overpass.kumi.systems/api/interpreter",
    "https://overpass-api.de/api/interpreter",
    "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
  };
  static uint8_t opIdx = 0;
  const char* endpoint = OVERPASS[opIdx];
  opIdx = (opIdx + 1) % (sizeof(OVERPASS) / sizeof(OVERPASS[0]));

  DBG("LANDMARKS query bbox %.3f,%.3f,%.3f,%.3f r=%dnm via %s\n",
      (double)south, (double)west, (double)north, (double)east, fetchR, endpoint);
  setStatus("Loading map...");

  String body = "data=";
  body += urlEncode(query);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(55);

  HTTPClient http;
  http.setConnectTimeout(12000);
  // Must cover the server's own [timeout:50] compute window plus transfer: the
  // POST blocks here until Overpass finishes computing and starts replying.
  http.setTimeout(55000);
  http.useHTTP10(true);
  if (!http.begin(client, endpoint)) {
    DBGLN("LANDMARKS http begin fail");
    return false;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setUserAgent("ESP32-FlightRadar/1.0");

  int code = http.POST(body);
  DBG("LANDMARKS HTTP %d\n", code);
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  // Stream into a scratch buffer and only swap it into the live cache once we
  // have a COMPLETE response. This way the existing coastline stays on screen
  // throughout the fetch and is preserved untouched if the download stalls or
  // errors out. (static => lives in .bss, not on the net task's stack.)
  static LmPoint lmTmp[LM_MAX_PTS];

  WiFiClient* st = http.getStreamPtr();
  const char* KGEO = "\"geometry\":";
  const char* KLAT = "\"lat\":";
  const char* KLON = "\"lon\":";
  const char* KREM = "\"remark\"";            // Overpass error/timeout marker
  char winBuf[12]; int winLen = 0;            // rolling tail of recent chars
  bool  newWay = false, haveLat = false, haveLon = false;
  bool  sawRemark = false;
  float curLat = 0, curLon = 0, lastLat = 0, lastLon = 0;
  int   n = 0;
  long  scanned = 0;
  bool  truncated = false;                    // hit the raw-byte safety stop
  lmReadTimedOut = false;                     // reset before streaming

  auto endsWith = [&](const char* key) -> bool {
    int kl = strlen(key);
    if (winLen < kl) return false;
    return memcmp(winBuf + winLen - kl, key, kl) == 0;
  };

  int c;
  while (n < LM_MAX_PTS && (c = lmReadByte(st)) >= 0) {
    if (++scanned > 2500000L) { truncated = true; break; }  // hard safety stop
    if (winLen == (int)sizeof(winBuf)) {       // slide window left by 1
      memmove(winBuf, winBuf + 1, winLen - 1);
      winLen--;
    }
    winBuf[winLen++] = (char)c;

    // Overpass signals server-side timeout / rate-limit with a "remark" field
    // (and no usable geometry). Bail so we treat it as a failure, not "inland".
    if (endsWith(KREM)) { sawRemark = true; break; }
    if (endsWith(KGEO)) { newWay = true; winLen = 0; continue; }
    if (endsWith(KLAT)) { curLat = lmReadNumber(st); haveLat = true; winLen = 0; }
    else if (endsWith(KLON)) { curLon = lmReadNumber(st); haveLon = true; winLen = 0; }

    if (haveLat && haveLon) {
      haveLat = haveLon = false;
      bool keep = newWay;
      if (!keep) {
        float d = haversineNm(lastLat, lastLon, curLat, curLon);
        keep = (d >= minSep);
      }
      if (keep) {
        lmTmp[n].lat = curLat; lmTmp[n].lon = curLon; lmTmp[n].penUp = newWay;
        n++;
        lastLat = curLat; lastLon = curLon;
        newWay = false;
      }
    }
  }
  http.end();

  // A stalled stream or an Overpass "remark" means we did NOT get a complete
  // answer. The scratch buffer is discarded and the live cache is left exactly
  // as it was (old map still on screen, old coverage radius unchanged), so the
  // caller backs off and tries again rather than caching a bad/empty result.
  if (lmReadTimedOut || sawRemark || truncated) {
    DBG("LANDMARKS incomplete (%s) after %d pts - keeping old map, will retry\n",
        sawRemark ? "remark" : (truncated ? "too big" : "timeout"), n);
    setStatus("Map load failed");
    return false;
  }

  // Complete response: swap the freshly-built coastline in atomically.
  LOCK();
  if (n > 0) memcpy(lmPts, lmTmp, (size_t)n * sizeof(LmPoint));
  lmCount    = n;
  lmGen      = lmGen + 1;  // invalidate the cached screen projection
  lmLoaded   = true;
  lmCacheLat = homeLat;
  lmCacheLon = homeLon;
  lmStoredR  = fetchR;
  UNLOCK();
  DBG("LANDMARKS loaded %d coastline points (%ld bytes scanned)\n", n, scanned);
  setStatus(n > 0 ? "Map loaded" : "No coastline here");

  saveLandmarks();      // persist so we don't refetch on the next boot
  return true;
}
#endif  // SHOW_LANDMARKS

// -----------------------------------------------------------------------------
//  Web config server
// -----------------------------------------------------------------------------
String htmlEscapeAttr(const char* s) {
  String o;
  for (const char* p = s; *p; p++) {
    if (*p == '\'') o += "&#39;";
    else if (*p == '"') o += "&quot;";
    else if (*p == '&') o += "&amp;";
    else if (*p == '<') o += "&lt;";
    else if (*p == '>') o += "&gt;";
    else o += *p;
  }
  return o;
}

void handleRoot() {
  String h;
  h.reserve(3600);
  h += F("<!DOCTYPE html><html><head><meta charset=utf-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>FlightRadar Setup</title><style>"
         "body{font-family:system-ui,Segoe UI,Roboto,sans-serif;background:#0b1020;color:#e6edf3;margin:0;padding:16px}"
         "h1{font-size:20px;color:#39d353;margin:0 0 4px}"
         ".c{max-width:440px;margin:auto}"
         "label{display:block;margin:12px 0 4px;font-size:13px;color:#9fb3c8}"
         "input,select{width:100%;box-sizing:border-box;padding:9px;border-radius:8px;border:1px solid #30363d;background:#161b22;color:#e6edf3;font-size:15px}"
         ".cb{display:flex;align-items:center;gap:8px;margin-top:14px}.cb input{width:auto}.cb label{margin:0}"
         ".sub{margin:8px 0 0 16px;padding-left:12px;border-left:2px solid #30363d}.sub .cb{margin-top:10px}"
         ".row{display:flex;gap:10px}.row>div{flex:1}"
         "button{margin-top:20px;width:100%;padding:13px;border:0;border-radius:8px;background:#238636;color:#fff;font-size:16px;font-weight:600}"
         ".n{font-size:12px;color:#6e7681;margin:6px 0 0}"
         "</style></head><body><div class=c>"
         "<h1>&#9992; FlightRadar Setup</h1>"
         "<p class=n>Settings are saved to flash. The device reboots after saving.</p>"
         "<form method=POST action='/save'>");

  h += F("<label>Wi-Fi network (2.4 GHz only)</label><input name=ssid value='");
  h += htmlEscapeAttr(g.ssid); h += F("'>");

  h += F("<label>Wi-Fi password</label><input name=pass type=password value='");
  h += htmlEscapeAttr(g.pass); h += F("'>");

  h += F("<div class=row><div><label>Latitude</label><input name=lat value='");
  h += String(g.homeLat, 5);
  h += F("'></div><div><label>Longitude</label><input name=lon value='");
  h += String(g.homeLon, 5);
  h += F("'></div></div>");

  h += F("<label>Home label (shown at radar centre)</label><input name=label value='");
  h += htmlEscapeAttr(g.label); h += F("'>");

  h += F("<label>Data source</label><select name=host>");
  const char* hosts[] = {"api.adsb.lol", "api.airplanes.live", "api.adsb.one"};
  for (auto hh : hosts) {
    h += F("<option");
    if (!strcmp(hh, g.apiHost)) h += F(" selected");
    h += F(">"); h += hh; h += F("</option>");
  }
  h += F("</select>");

  h += F("<div class=row><div><label>Range (nm, 1-250)</label><input name=range type=number min=1 max=250 value='");
  h += String(g.defaultRange);
  h += F("'></div><div><label>Refresh (seconds)</label><input name=refresh type=number min=1 value='");
  h += String(g.refreshMs / 1000);
  h += F("'></div></div>");

  h += F("<label>Timezone offset from UTC (hours)</label><input name=tz value='");
  h += String(g.gmtOffset / 3600.0, 1);
  h += F("'>");

  h += F("<div class=cb><input type=checkbox name=dst id=dst");
  if (g.dstOffset) h += F(" checked");
  h += F("><label for=dst>Daylight saving (+1h)</label></div>");

  h += F("<div class=cb><input type=checkbox name=metric id=metric");
  if (g.useMetric) h += F(" checked");
  h += F("><label for=metric>Metric units (m, km/h, km)</label></div>");

  h += F("<div class=cb><input type=checkbox name=hidegnd id=hidegnd");
  if (g.hideGround) h += F(" checked");
  h += F("><label for=hidegnd>Hide aircraft on the ground (GND)</label></div>");

  h += F("<div class=sub><div class=cb><input type=checkbox name=keeptaxi id=keeptaxi");
  if (g.gndKeepTaxi) h += F(" checked");
  h += F("><label for=keeptaxi>Keep taxiing aircraft</label></div>");
  h += F("<label>Keep if ground speed above (kt)</label>"
         "<input name=taxikt type=number min=1 max=200 value='");
  h += String(g.gndTaxiKt);
  h += F("'></div>");

#if SWEEP_ENABLE
  h += F("<div class=cb><input type=checkbox name=sweep id=sweep");
  if (g.sweep) h += F(" checked");
  h += F("><label for=sweep>Radar sweep arm</label></div>");
#endif

#if SHOW_LANDMARKS
  h += F("<div class=cb><input type=checkbox name=landmk id=landmk");
  if (g.landmarks) h += F(" checked");
  h += F("><label for=landmk>Coastline map overlay</label></div>");
#endif

#if SHOW_AIRPORTS
  h += F("<div class=cb><input type=checkbox name=airpt id=airpt");
  if (g.airports) h += F(" checked");
  h += F("><label for=airpt>Show major airports</label></div>");
  h += F("<div class=sub><div class=cb><input type=checkbox name=apname id=apname");
  if (g.airportNames) h += F(" checked");
  h += F("><label for=apname>Label with full name (else IATA code)</label></div></div>");
#endif

#if SHOW_ROUTES
  h += F("<div class=cb><input type=checkbox name=routes id=routes");
  if (g.routes) h += F(" checked");
  h += F("><label for=routes>Look up flight routes (FROM &gt; TO &ndash; may not always accurate)</label></div>");
#endif

  h += F("<button type=submit>Save &amp; Reboot</button></form>");

#if SHOW_LANDMARKS
  h += F("<p class=n><a style='color:#58a6ff' href='/refreshmap'>"
         "&#10227; Refresh coastline map now</a> "
         "(no reboot; redownloads for the current location)</p>");
#endif

  h += F("<p class=n>Firmware v" FW_VERSION
         " &middot; <a style='color:#58a6ff' href='/update'>Update firmware (OTA)</a></p>"
         "</div></body></html>");
  server.send(200, "text/html", h);
}

void handleSave() {
  if (server.hasArg("ssid"))  strlcpy(g.ssid,    server.arg("ssid").c_str(),  sizeof(g.ssid));
  if (server.hasArg("pass"))  strlcpy(g.pass,    server.arg("pass").c_str(),  sizeof(g.pass));
  if (server.hasArg("host"))  strlcpy(g.apiHost, server.arg("host").c_str(),  sizeof(g.apiHost));
  if (server.hasArg("label")) strlcpy(g.label,   server.arg("label").c_str(), sizeof(g.label));
  if (server.hasArg("lat"))   g.homeLat = server.arg("lat").toFloat();
  if (server.hasArg("lon"))   g.homeLon = server.arg("lon").toFloat();
  if (server.hasArg("range")) g.defaultRange = constrain((int)server.arg("range").toInt(), 1, 250);
  if (server.hasArg("refresh")) {
    long s = server.arg("refresh").toInt();
    if (s < 1) s = 1;
    g.refreshMs = (uint32_t)s * 1000UL;
  }
  g.gmtOffset = (long)(server.arg("tz").toFloat() * 3600.0);
  g.dstOffset = server.hasArg("dst")    ? 3600 : 0;
  g.useMetric  = server.hasArg("metric")  ? 1 : 0;
  g.hideGround  = server.hasArg("hidegnd")  ? 1 : 0;
  g.gndKeepTaxi = server.hasArg("keeptaxi") ? 1 : 0;
  if (server.hasArg("taxikt"))
    g.gndTaxiKt = (uint8_t)constrain((int)server.arg("taxikt").toInt(), 1, 200);
#if SWEEP_ENABLE
  g.sweep      = server.hasArg("sweep")   ? 1 : 0;
#endif
#if SHOW_LANDMARKS
  g.landmarks  = server.hasArg("landmk")  ? 1 : 0;
#endif
#if SHOW_AIRPORTS
  g.airports     = server.hasArg("airpt")  ? 1 : 0;
  g.airportNames = server.hasArg("apname") ? 1 : 0;
#endif
#if SHOW_ROUTES
  g.routes     = server.hasArg("routes")  ? 1 : 0;
#endif

  saveSettings();

  String h = F("<!DOCTYPE html><html><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<style>body{font-family:system-ui,sans-serif;background:#0b1020;color:#e6edf3;text-align:center;padding-top:60px}"
               "h2{color:#39d353}</style></head><body>"
               "<h2>&#10003; Saved</h2><p>Rebooting and connecting to Wi-Fi&hellip;</p>"
               "</body></html>");
  server.send(200, "text/html", h);
  delay(1200);
  ESP.restart();
}

#if SHOW_LANDMARKS
// Force a fresh coastline download without rebooting. The flag is picked up by
// netTask (core 0, which owns the network) on its next cycle; we just ack here.
void handleRefreshMap() {
  lmForceRefresh = true;
  setStatus("Refreshing map...");
  String h = F("<!DOCTYPE html><html><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<meta http-equiv=refresh content='2;url=/'>"
               "<style>body{font-family:system-ui,sans-serif;background:#0b1020;color:#e6edf3;text-align:center;padding-top:60px}"
               "h2{color:#39d353}a{color:#58a6ff}</style></head><body>"
               "<h2>&#10227; Map refresh queued</h2>"
               "<p>The coastline is being re-downloaded&hellip;</p>"
               "<p><a href='/'>&larr; back to settings</a></p>"
               "</body></html>");
  server.send(200, "text/html", h);
}
#endif

// -----------------------------------------------------------------------------
//  OTA firmware update (web)
//  Upload a compiled .bin (Arduino IDE: Sketch -> Export Compiled Binary).
//  Requires an OTA-capable partition scheme (see README): the new image is
//  written to the spare app slot, then the device reboots into it.
// -----------------------------------------------------------------------------
void handleUpdatePage() {
  String h = F(
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>FlightRadar OTA</title><style>"
    "body{font-family:system-ui,Segoe UI,Roboto,sans-serif;background:#0b1020;color:#e6edf3;margin:0;padding:16px}"
    "h1{font-size:20px;color:#39d353}.c{max-width:440px;margin:auto}"
    "input[type=file]{width:100%;box-sizing:border-box;margin:14px 0;color:#e6edf3}"
    "button{width:100%;padding:13px;border:0;border-radius:8px;background:#238636;color:#fff;font-size:16px;font-weight:600}"
    ".n{font-size:12px;color:#6e7681}a{color:#58a6ff}"
    "#b{height:12px;background:#161b22;border-radius:6px;overflow:hidden;margin-top:16px;display:none}"
    "#p{height:100%;width:0;background:#39d353;transition:width .2s}"
    "</style></head><body><div class=c>"
    "<h1>Firmware update</h1>"
    "<p class=n>Current version: v" FW_VERSION
    ". Pick a compiled <b>.bin</b> (Arduino IDE: Sketch &rarr; Export Compiled Binary).</p>"
    "<form id=f method=POST action='/update' enctype='multipart/form-data'>"
    "<input type=file name=update accept='.bin' required>"
    "<button type=submit>Upload &amp; flash</button></form>"
    "<div id=b><div id=p></div></div>"
    "<p class=n><a href='/'>&larr; back to settings</a></p>"
    "<script>"
    "var f=document.getElementById('f');"
    "f.onsubmit=function(e){e.preventDefault();"
    "var x=new XMLHttpRequest();x.open('POST','/update');"
    "document.getElementById('b').style.display='block';"
    "x.upload.onprogress=function(ev){if(ev.lengthComputable)"
    "document.getElementById('p').style.width=(ev.loaded/ev.total*100)+'%';};"
    "x.onload=function(){document.body.innerHTML="
    "'<div class=c><h1>'+x.responseText+'</h1></div>';};"
    "x.send(new FormData(f));};"
    "</script></div></body></html>");
  server.send(200, "text/html", h);
}

void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaActive = true;                    // freeze the radar render (see loop())
    Serial.printf("OTA start: %s\n", up.filename.c_str());
    // On the S3-N16R8 the panel framebuffer is in PSRAM, which shares the octal-
    // SPI bus with flash. While Update.write() programs flash, the panel DMA is
    // starved and the display shows shifted/garbled content. The backlight isn't
    // switchable on this board, so instead we show the message briefly, then make
    // the framebuffer solid BLACK for the whole write phase - an underrun that
    // repeats black is still black, so nothing garbled is visible.
    drawOtaScreen("Updating - do not power off", 0xFFFF);
    delay(800);                          // let the message be read first
    otaBlankScreen();                    // solid black hides the flash-write garbage
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    // No display activity here (buffer stays solid black).
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("OTA success: %u bytes\n", up.totalSize);
      drawOtaScreen("Success - rebooting", 0x07E0);   // reboot follows (POST handler)
    } else {
      Update.printError(Serial);
      drawOtaScreen("FAILED", 0xF800);
      otaActive = false;                 // failed: let the radar resume
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    drawOtaScreen("Aborted", 0xF800);
    otaActive = false;                   // connection dropped: resume the radar
  }
}

void registerRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
#if SHOW_LANDMARKS
  server.on("/refreshmap", HTTP_GET, handleRefreshMap);
#endif
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, []() {
    bool ok = !Update.hasError();
    server.send(200, "text/html", ok ? "Update OK - rebooting..."
                                      : "Update FAILED - check serial");
    delay(800);
    if (ok) ESP.restart();
  }, handleUpdateUpload);
  server.onNotFound(handleRoot);   // captive portal: any URL -> setup page
}

// Ask for the Wi-Fi setup portal. Safe to call from the render core / under the
// data lock: the actual teardown happens in netTask (see startConfigPortal).
void requestConfigPortal() {
  setStatus("Setup mode");
  portalRequested = true;
}

// MUST run on the network core (core 0) - it reconfigures Wi-Fi. Do not call
// from the render core while netTask may be mid-fetch (use requestConfigPortal).
// The portal screen is painted by loop()'s apMode branch, not here, so only one
// core ever draws to the sprite.
void startConfigPortal() {
  apMode = true;
  portalExitRequested = false;     // clear any stale exit request
  selected = -1;
  setStatus("Setup mode");
  server.close();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  delay(100);
  WiFi.softAP(AP_SSID, strlen(AP_PASS) ? AP_PASS : NULL);
  delay(300);
  dns.start(53, "*", WiFi.softAPIP());
  server.begin();
  DBG("Config portal AP '%s', open http://%s\n",
      AP_SSID, WiFi.softAPIP().toString().c_str());
}

// Ask to leave the setup portal (BACK button). Safe to call from the render
// core: netTask performs the Wi-Fi switch-back (see exitConfigPortal).
void requestConfigExit() {
  portalExitRequested = true;
}

// MUST run on the network core (core 0) - it reconfigures Wi-Fi. Tears the AP
// down and reconnects to the saved network without a reboot. apMode stays true
// (so the portal screen keeps drawing and connectWiFi() doesn't draw from this
// core) until the switch-back is done.
void exitConfigPortal() {
  setStatus("Leaving setup...");
  dns.stop();
  server.close();
  WiFi.softAPdisconnect(true);
  delay(100);
  bool ok = (strlen(g.ssid) > 0) && connectWiFi();   // apMode still true here
  if (ok) {
    server.begin();
    if (MDNS.begin("flightradar")) MDNS.addService("http", "tcp", 80);
    DBG("Config UI: http://%s\n", WiFi.localIP().toString().c_str());
  } else {
    DBGLN("Exit portal: no Wi-Fi (no SSID or connect failed)");
  }
  firstFetch = false;      // pull fresh aircraft immediately
  portalRequested = false; // clear any stale portal request
  apMode     = false;      // hand the screen back to the radar
}

// -----------------------------------------------------------------------------
//  Board support (display + touch + UI).
//  Provides: spr, displayBegin(), readTouch(), the UI (drawScreen,
//  drawConfigPortalScreen, drawOtaScreen) and handleTouch().
// -----------------------------------------------------------------------------
#include "board_7in.h"

// -----------------------------------------------------------------------------
//  Network task (core 0)
//  All the slow/blocking networking lives here: the periodic aircraft fetch and
//  the on-demand aircraft-type lookup. It writes results through the mutex; the render
//  loop on core 1 just reads them. This keeps the radar sweep + touch smooth
//  even while a fetch is in flight (the old single-core glitch).
// -----------------------------------------------------------------------------
void netTask(void* pv) {
  for (;;) {
    // CFG was tapped: start the setup portal HERE, on core 0, which owns Wi-Fi.
    // Doing the Wi-Fi teardown from the render core while a fetch is in flight
    // on this core crashes the chip, so the UI only *requests* the portal and
    // we act on it between fetches (never mid-TLS).
    if (portalRequested && !apMode) {
      portalRequested = false;
      startConfigPortal();
    }
    // BACK was tapped on the portal: switch back to the saved network here, on
    // core 0, for the same cross-core-safety reason as starting the portal.
    if (portalExitRequested && apMode) {
      portalExitRequested = false;
      exitConfigPortal();
    }

    // Pause fetching during an OTA flash (avoids a TLS + flash-write heap spike).
    if (!apMode && !Update.isRunning() && WiFi.status() == WL_CONNECTED) {
      // NTP self-heal. configTime() (called on connect) starts the background
      // SNTP daemon which retries on its own, but if it never synced - e.g. NTP
      // was unreachable on the first connect - getLocalTime() keeps failing and
      // the header shows --:--. Re-issue configTime() every 60 s until the clock
      // actually reads valid, then stop (getLocalTime() only succeeds once set).
      {
        static unsigned long ntpNextTry = 0;
        static bool          ntpSynced  = false;
        if (!ntpSynced) {
          struct tm ti;
          if (getLocalTime(&ti, 5)) {
            ntpSynced = true;
            DBG("NTP synced: %04d-%02d-%02d %02d:%02d\n",
                ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min);
          } else if (millis() >= ntpNextTry) {
            configTime(g.gmtOffset, g.dstOffset, NTP_SERVER);
            ntpNextTry = millis() + 60000;
            DBG("NTP not set yet - re-issued configTime()\n");
          }
        }
      }

      // Aircraft first (time-critical, frequent).
      unsigned long now = millis();
      if (!firstFetch || now - lastFetch >= g.refreshMs) {
        fetchAircraft();
        lastFetch  = millis();
        firstFetch = true;
        gNeedRedraw   = true;        // new positions -> repaint (sweep-off mode)
        gSweepEpochMs = lastFetch;   // restart the sweep arm in step with the new data
      }
      // With nothing selected, keep the full-type lookup aimed at the nearest
      // in-range plane so the NEAREST panel shows the extended type as well. The
      // RAM cache means each airframe hits adsbdb only once; we only re-arm when
      // the nearest changes, so this doesn't spam the API every refresh.
      if (selected < 0) {
        LOCK();
        if (acCount > 0 && acList[0].distNm <= rangeNm &&
            strcmp(typeHex, acList[0].hex) != 0) {
          strlcpy(typeHex, acList[0].hex, sizeof(typeHex));
          int ci = typeCacheFind(typeHex);
          if (ci >= 0) {
            strlcpy(typeFull,  typeCache[ci].full,  sizeof(typeFull));
            strlcpy(typeOwner, typeCache[ci].owner, sizeof(typeOwner));
            typeState = typeCache[ci].state;
          } else {
            typeFull[0] = '\0'; typeOwner[0] = '\0';
            // Resolve from the offline ICAO type table first (instant, no API).
            const char* sf = typeNameFromIcao(acList[0].type);
            if (sf) {
              strlcpy(typeFull, sf, sizeof(typeFull));
              typeCachePut(typeHex, sf, "", 2);   // remember it; no network call
              typeState = 2;
            } else {
              typeState = 1;                       // unknown code -> queue adsbdb
            }
          }
        }
        UNLOCK();
      }
      // A transient lookup failure (adsbdb 202/429, a 5xx, a dropped connection)
      // parks the request in state 4; re-arm it once the back-off has elapsed.
      if (typeState == 4 && millis() >= typeRetryAt) typeState = 1;
      if (typeState  == 1) fetchAircraftType();  // a tap (or nearest) queued a lookup

#if SHOW_ROUTES
      // Route lookup, aimed at the same focused plane as the type lookup (selected,
      // else the nearest in-range). Keyed by callsign and cached, so a flight only
      // hits the API once; we only re-arm when the focused callsign changes. Planes
      // with no callsign (flight defaults to the hex) are skipped - no route to find.
      if (g.routes) {
        LOCK();
        const Aircraft* fa = nullptr;
        if (selected >= 0 && selected < acCount)              fa = &acList[selected];
        else if (acCount > 0 && acList[0].distNm <= rangeNm)  fa = &acList[0];
        bool hasCs = fa && strlen(fa->flight) && strcmp(fa->flight, fa->hex) != 0
                     && routeWorthLooking(fa);
        if (hasCs) {
          if (strcmp(routeFlight, fa->flight) != 0) {         // focus changed
            strlcpy(routeFlight, fa->flight, sizeof(routeFlight));
            routeLat = fa->lat; routeLon = fa->lon;
            int rci = routeCacheFind(routeFlight);
            if (rci >= 0) {
              strlcpy(routeFrom, routeCache[rci].from, sizeof(routeFrom));
              strlcpy(routeTo,   routeCache[rci].to,   sizeof(routeTo));
              routeState = routeCache[rci].state;
            } else {
              routeFrom[0] = '\0'; routeTo[0] = '\0';
              routeState = 1;                                 // queue a lookup
            }
          } else {
            routeLat = fa->lat; routeLon = fa->lon;            // keep position fresh
          }
        } else {
          routeFlight[0] = '\0'; routeFrom[0] = '\0'; routeTo[0] = '\0'; routeState = 0;
        }
        UNLOCK();
        if (routeState == 4 && millis() >= routeRetryAt) routeState = 1;
        if (routeState == 1) fetchRoute();
      }
#endif

#if SHOW_LANDMARKS
      // Coastline (re)fetch, done AFTER the first aircraft fetch so traffic
      // shows quickly and the heavy Overpass TLS session doesn't sit back-to-
      // back with the first feed connection. Grow-on-demand: (re)download only
      // when there's no cache yet, the selected range now reaches past what the
      // cache covers (zoomed out), or the home has moved. Zooming back in just
      // reuses the larger cache. A failed download backs off for 60 s.
      static unsigned long lmNextTry = 0;
      static int           lmFailStreak = 0;   // consecutive failed fetches
      // A forced refresh (web "Refresh map" button) clears any back-off window
      // once, on the rising edge, so it downloads straight away rather than
      // waiting out a previous failure's 60 s timer.
      static bool lmForcePrev = false;
      if (lmForceRefresh && !lmForcePrev) lmNextTry = 0;
      lmForcePrev = lmForceRefresh;
      // Compare the *target* coverage (capped by lmFetchRadiusNm) against what's
      // stored - not the raw range - so the capped widest zoom doesn't think it
      // perpetually needs a bigger map and refetch every cycle.
      bool lmNeed = g.landmarks
                 && (lmForceRefresh
                 || !lmLoaded
                 || lmFetchRadiusNm() > lmStoredR
                 || haversineNm(lmCacheLat, lmCacheLon, g.homeLat, g.homeLon) > 2.0f);
      if (firstFetch && lmNeed && millis() >= lmNextTry) {
        // The Overpass fetch needs ~40 KB+ of internal RAM for its TLS session.
        // If a big aircraft payload has temporarily drained the heap, defer
        // rather than attempt a doomed connection (it would fail with -11).
        if (ESP.getFreeHeap() < 100000) {
          DBGLN("LANDMARKS deferred - low heap");
          lmNextTry = millis() + 15000UL;     // keep lmForceRefresh set; retry soon
        } else {
          bool ok = fetchLandmarks();        // builds for the current range
          if (ok) {
            lmForceRefresh = false;          // clear the request only on success
            lmFailStreak   = 0;
            lmNextTry      = 0;
          } else {
            // Escalating back-off: a coastline query that keeps timing out
            // server-side means Overpass is busy / throttling us. Don't hammer
            // it every 60 s - widen the gap (60s,120s,...,up to 5min) so we stay
            // within fair-use limits and give the mirrors room to recover.
            if (lmFailStreak < 5) lmFailStreak++;
            lmNextTry = millis() + 60000UL * lmFailStreak;
          }
        }
      }
#endif
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// -----------------------------------------------------------------------------
//  Setup / loop
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32 FlightRadar v" FW_VERSION " ===");

  gDataMux = xSemaphoreCreateRecursiveMutex();   // guard shared data (2 cores)

  loadSettings();
  DBG("Settings: SSID='%s' host=%s home=%.4f,%.4f range=%dnm refresh=%lus metric=%d "
      "hideGnd=%d keepTaxi=%d@%dkt sweep=%d landmarks=%d airports=%d routes=%d\n",
      g.ssid, g.apiHost, (double)g.homeLat, (double)g.homeLon, g.defaultRange,
      (unsigned long)(g.refreshMs / 1000), g.useMetric, g.hideGround,
      g.gndKeepTaxi, g.gndTaxiKt, g.sweep, g.landmarks, g.airports, g.routes);

#if SHOW_LANDMARKS
  // Restore the saved coastline so it's on screen immediately. The net task
  // then (re)downloads only if needed (no cache / zoomed past coverage / moved).
  landmarksFsBegin();
  bool lmHaveCache = loadLandmarks();
  DBG("LANDMARKS cache %s%s\n", lmHaveCache ? "found" : "empty",
      lmHaveCache ? "" : " - will download once online");
#endif

  displayBegin();                  // board: HW + back buffer + SCR_W/H + geometry

  // Use the saved range as-is, then pick the closest preset so RNG+ has a
  // sensible starting point (the web range field allows any value 1-250).
  rangeNm  = g.defaultRange;
  rangeIdx = 0;
  int rDiff = abs(rangeSteps[0] - rangeNm);
  for (int i = 1; i < numRanges; i++) {
    int d = abs(rangeSteps[i] - rangeNm);
    if (d < rDiff) { rDiff = d; rangeIdx = i; }
  }

  uint32_t splashT0 = millis();      // keep the splash up for a beat (below)
  drawSplashScreen("Starting...");   // ATC title card while we bring up Wi-Fi
  registerRoutes();

  // First-run / no Wi-Fi configured -> straight to the setup portal.
  if (strlen(g.ssid)) drawSplashScreen("Connecting Wi-Fi...");
  if (strlen(g.ssid) == 0 || connectWiFi()) {
    if (strlen(g.ssid) == 0) {
      startConfigPortal();
    } else {
      server.begin();              // LAN config UI while connected
      if (MDNS.begin("flightradar")) {     // ~a few KB RAM; nice convenience
        MDNS.addService("http", "tcp", 80);
        DBGLN("mDNS ready: http://flightradar.local");
      }
      DBG("Config UI: http://%s\n", WiFi.localIP().toString().c_str());
    }
  } else {
    startConfigPortal();           // Wi-Fi failed -> portal
  }

  // Make sure the splash is on screen long enough to read even when Wi-Fi
  // connects almost instantly (otherwise it would just flash by).
  if (!apMode) {
    drawSplashScreen("Ready");
    while (millis() - splashT0 < 5000) delay(20);
  }

  // Start the networking task on core 0 (the Arduino loop runs on core 1), so
  // fetches never stall the display. Stack is generous for TLS + ArduinoJson.
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, &netTaskHandle, 0);
}

void loop() {
  if (apMode) {                    // Wi-Fi setup portal active
    dns.processNextRequest();
    server.handleClient();
    drawConfigPortalScreen();
    // Tapping BACK leaves the portal and reconnects (handled on core 0).
    static bool wasTp = false;
    int tx, ty;
    bool tp = readTouch(tx, ty);
    if (tp && !wasTp &&
        tx >= cfgBackX && tx <= cfgBackX + cfgBackW &&
        ty >= cfgBackY && ty <= cfgBackY + cfgBackH) {
      requestConfigExit();
    }
    wasTp = tp;
    delay(20);
    return;
  }

  server.handleClient();           // LAN config page stays available

  // During a web firmware upload, leave the OTA status screen on screen and skip
  // the radar render - otherwise this loop repaints the radar between upload
  // chunks and the "FIRMWARE UPDATE" screen flickers in and out.
  if (otaActive) { delay(5); return; }

  static unsigned long lastReconnect = 0;
  if (!apMode && WiFi.status() != WL_CONNECTED) {           // never touch Wi-Fi in
    bool sayLost;                                           // AP/portal mode (core 0
    LOCK();                                                 // owns Wi-Fi there)
    sayLost = (statusMsg != "WiFi lost");
    UNLOCK();
    if (sayLost) setStatus("WiFi lost");                    // log once per drop
    if (millis() - lastReconnect > 5000) {
      WiFi.reconnect();
      lastReconnect = millis();
    }
  }

  LOCK();
  handleTouch();                   // selection / RNG+ / CFG (queues type lookup)
  UNLOCK();
  if (apMode) return;              // CFG was pressed this frame

  // Render. The network task on core 0 fills the data; we just draw it. The
  // lock is held only for the duration of one frame's draw.
  //
  // With the sweep ON we redraw every frame so the arm animates (~30 fps). With
  // it OFF the scene is static between data pulls, so we only redraw when something
  // changed (new data / a tap), when the displayed clock minute rolls over, or as a
  // slow safety heartbeat. The clock is HH:MM, so a per-SECOND repaint was pointless
  // - and that periodic full-frame push to PSRAM was itself starving the scanout and
  // glitching the panel roughly once a second. Minute-resolution kills that.
  static unsigned long lastDraw  = 0;
  static unsigned long lastSweep = 0;
  static int           lastMin   = -1;
  bool doDraw;
  if (dataOk && g.sweep) {
    // Redraw the arm at ~10 fps (SWEEP_REDRAW_MS), not the 30 fps poll rate: each
    // step pushes the whole canvas to PSRAM and that bus is shared with the scanout.
    // A data update or tap (gNeedRedraw) still repaints immediately between steps.
    bool sweepStep = (millis() - lastSweep >= SWEEP_REDRAW_MS);
    if (sweepStep) {
      lastSweep = millis();
      // One revolution == one data-refresh interval, phased to the last fetch, so the
      // arm makes exactly one scan per update and finishes as the next pull is due.
      // Derived from elapsed time (not accumulated steps) so it can't drift; %360
      // keeps it spinning at the right rate even if a fetch is late (back-off).
      unsigned long period = g.refreshMs ? g.refreshMs : REFRESH_MS;
      sweepAngle = (int)(((millis() - gSweepEpochMs) * 360UL / period) % 360UL);
    }
    doDraw = sweepStep || gNeedRedraw;
  } else {
    struct tm ti;
    int curMin = lastMin;
    if (getLocalTime(&ti, 0)) curMin = ti.tm_min;   // non-blocking once NTP is set
    doDraw = gNeedRedraw || (curMin != lastMin) || (millis() - lastDraw >= 30000);
    if (doDraw) lastMin = curMin;
  }
  if (doDraw) {
    gNeedRedraw = false;
    lastDraw    = millis();
    LOCK();
    drawScreen();
    UNLOCK();
  }

  // Self-heal the RGB DMA. If a PSRAM-bandwidth spike ever de-syncs the panel the
  // image shifts permanently; esp_lcd_rgb_panel_restart() re-aligns it at the next
  // VSYNC. Doing it ~1 Hz is invisible when already in sync and recovers a roll
  // within a second instead of it sticking until reboot.
  static unsigned long lastResync = 0;
  if (millis() - lastResync >= 1000) {
    lastResync = millis();
    panelResync();
  }

  delay(33);                       // ~30 fps poll (touch stays responsive)
}

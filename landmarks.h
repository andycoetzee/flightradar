// =====================================================================
//  landmarks.h  -  Map overlay for the radar scope
// ---------------------------------------------------------------------
//  Draws a map overlay under the live traffic:
//
//    * Coastline  - fetched DYNAMICALLY from OpenStreetMap (Overpass API)
//                   for whatever home location is configured, so it is
//                   correct wherever you are. No editing required; see
//                   fetchLandmarks() in FlightRadar.ino. The points live
//                   in the shared lmPts[] cache (built by the net task).
//
//    * Static extras (OPTIONAL) - your own fixed polylines / labelled
//                   points (airports, towns, rivers...). Edit the arrays
//                   below. Leave them empty to draw only the coastline.
//
//  Coordinates are decimal degrees (N/E positive, S/W negative). Anything
//  outside the current range ring is clipped automatically.
//
//  Included by the active board header AFTER the display sprite (spr) and
//  the core projection helpers (geoToScreenRaw / clipSegToRadar) exist.
// =====================================================================
#pragma once

#if SHOW_LANDMARKS

// ---- Colours (RGB565). Kept subdued so traffic stands out.
#define COAST_COLOUR   0x4ABF   // muted steel-blue coastline
#define MARK_COLOUR    0xFD20   // amber point markers / labels

// ---------------------------------------------------------------------
//  OPTIONAL static extras. Default: none (coastline is dynamic). Add your
//  own here if you want fixed landmarks on top of the live coastline.
// ---------------------------------------------------------------------
struct GeoPoint { float lat, lon; };
struct GeoLine  { const GeoPoint* pts; int n; uint16_t col; };
struct GeoMark  { float lat, lon; const char* label; uint16_t col; };

// To ADD extras: list your points/lines/marks below and set the matching
// *Count* to  sizeof(arr)/sizeof(arr[0]).  The first entry of each array is a
// disabled placeholder (count = 0) so the overlay draws only the coastline by
// default. Example of an enabled river + airport:
//   static const GeoPoint myRiver[] = { {54.50f,-1.20f}, {54.45f,-1.05f} };
//   static const GeoLine  geoLines[] = { { myRiver, 2, 0x4416 } };
//   static const int      geoLineCount = sizeof(geoLines)/sizeof(geoLines[0]);
//   static const GeoMark  geoMarks[]   = { {54.5092f,-1.4294f,"MME",MARK_COLOUR} };
//   static const int      geoMarkCount = sizeof(geoMarks)/sizeof(geoMarks[0]);

static const GeoPoint lmDummy[]  = { { 0, 0 } };
static const GeoLine  geoLines[] = { { lmDummy, 0, 0 } };  // placeholder
static const int      geoLineCount = 0;                    // <- set to sizeof(...) to enable
static const GeoMark  geoMarks[] = { { 0, 0, "", 0 } };    // placeholder
static const int      geoMarkCount = 0;                    // <- set to sizeof(...) to enable

// ---------------------------------------------------------------------
//  Render the overlay onto the scope sprite. Call from drawRadarScope()
//  after the rings/compass and before the aircraft targets. Runs under
//  the data mutex (drawScreen is locked), so reading lmPts[] is safe.
// ---------------------------------------------------------------------
inline void drawLandmarks() {
  // 1) Dynamic coastline (built by the net task from OpenStreetMap)
  if (lmLoaded && lmCount > 0) {
    // Project the coastline to screen coords ONLY when the view changed (range or
    // home moved) or the data was replaced - not every frame. The per-point
    // haversine/bearing trig done every frame was the main CPU/PSRAM hog that
    // starved the RGB scanout and made the picture roll. Cached coords mean each
    // frame just does cheap line-clipping + drawing.
    static int      lmProjRange = -1;
    static float    lmProjLat   = 1e9f, lmProjLon = 1e9f;
    static uint32_t lmProjGen   = 0xFFFFFFFFu;
    if (lmProjRange != rangeNm || lmProjLat != g.homeLat ||
        lmProjLon != g.homeLon || lmProjGen != lmGen) {
      for (int i = 0; i < lmCount; i++) {
        float sx, sy;
        geoToScreenRaw(lmPts[i].lat, lmPts[i].lon, sx, sy);
        // Clamp far-off points so they fit int16 (they get clipped out anyway).
        if (sx >  15000.f) sx =  15000.f; else if (sx < -15000.f) sx = -15000.f;
        if (sy >  15000.f) sy =  15000.f; else if (sy < -15000.f) sy = -15000.f;
        lmScrX[i] = (int16_t)sx;
        lmScrY[i] = (int16_t)sy;
      }
      lmProjRange = rangeNm; lmProjLat = g.homeLat;
      lmProjLon   = g.homeLon; lmProjGen = lmGen;
    }

    int dbgDrawn = 0, dbgClipped = 0;
    float ppx = 0, ppy = 0; bool prev = false;
    for (int i = 0; i < lmCount; i++) {
      float sx = (float)lmScrX[i], sy = (float)lmScrY[i];
      if (!lmPts[i].penUp && prev) {           // penUp = first vertex of a way
        int a, b, c, d;
        if (clipSegToRadar(ppx, ppy, sx, sy, a, b, c, d)) {
          spr.drawLine(a, b, c, d, COAST_COLOUR);
          dbgDrawn++;
        } else dbgClipped++;
      }
      ppx = sx; ppy = sy; prev = true;
    }
#if DEBUG
    static uint32_t lmDbgLast = 0;
    if (millis() - lmDbgLast > 5000) {
      lmDbgLast = millis();
      DBG("LM draw: count=%d drawn=%d clipped=%d range=%dnm cx=%d cy=%d r=%d p0=(%.3f,%.3f)\n",
          lmCount, dbgDrawn, dbgClipped, rangeNm, cx, cy, radarR,
          (double)lmPts[0].lat, (double)lmPts[0].lon);
    }
#endif
  }
#if DEBUG
  else {
    static uint32_t lmDbgLast2 = 0;
    if (millis() - lmDbgLast2 > 5000) {
      lmDbgLast2 = millis();
      DBG("LM draw: nothing - lmLoaded=%d lmCount=%d\n", (int)lmLoaded, lmCount);
    }
  }
#endif

  // 2) Optional user-defined static polylines
  for (int L = 0; L < geoLineCount; L++) {
    const GeoLine& gl = geoLines[L];
    float ppx = 0, ppy = 0; bool prev = false;
    for (int i = 0; i < gl.n; i++) {
      float sx, sy;
      geoToScreenRaw(gl.pts[i].lat, gl.pts[i].lon, sx, sy);
      if (prev) {
        int a, b, c, d;
        if (clipSegToRadar(ppx, ppy, sx, sy, a, b, c, d))
          spr.drawLine(a, b, c, d, gl.col);
      }
      ppx = sx; ppy = sy; prev = true;
    }
  }

  // 3) Optional user-defined labelled points
  for (int M = 0; M < geoMarkCount; M++) {
    const GeoMark& gm = geoMarks[M];
    float sx, sy;
    geoToScreenRaw(gm.lat, gm.lon, sx, sy);
    float dx = sx - (float)cx, dy = sy - (float)cy;
    if (dx * dx + dy * dy <= (float)radarR * (float)radarR) {
      int x = (int)sx, y = (int)sy;
      spr.drawLine(x - 3, y, x + 3, y, gm.col);
      spr.drawLine(x, y - 3, x, y + 3, gm.col);
      spr.setTextDatum(TL_DATUM);
      spr.setTextColor(gm.col, TFT_BLACK);
      spr.drawString(gm.label, x + 4, y - 4, 1);
    }
  }
}

#endif // SHOW_LANDMARKS

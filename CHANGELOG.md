# Changelog

All notable changes to **FlightRadar** (Waveshare ESP32-S3-Touch-LCD-7).
The on-device version is `FW_VERSION` in `config.h` and is shown in the header,
the web pages and the serial log.

Format: newest first. Dates are when the change was made.

## Versioning

This project follows [Semantic Versioning](https://semver.org/) (`MAJOR.MINOR.PATCH`),
interpreted for firmware as:

- **MAJOR** - breaking changes that need user action on upgrade: a change to the
  saved-settings (NVS) layout or the coastline cache format, or anything that forces
  a reconfigure / cache wipe.
- **MINOR** - new, backwards-compatible features (a new config option, overlay,
  data source, etc.). Resets PATCH to 0.
- **PATCH** - backwards-compatible bug fixes, performance work, and cosmetic tweaks.

Adopted at `2.3.80`. Earlier releases used the same `x.y.z` format but bumped PATCH
for every change regardless of type, so pre-2.3.80 numbers don't carry SemVer meaning.

## 2.4.15
- **Detail panel no longer stuck on "SELECTED" with an empty scope.** The panel title
  defaulted to SELECTED and only became NEAREST when an in-range aircraft existed, so
  with nothing tapped and no aircraft in range it wrongly read SELECTED above "No
  aircraft in range." It now defaults to NEAREST and shows SELECTED only when a plane
  is actually selected. (PATCH: bug fix.)

## 2.4.14
- **Sweep arm no longer jumps back to 0 on every API read.** 2.4.13 re-anchored the
  sweep phase to each fetch, which made the arm visibly snap back to the top every
  refresh. It's now anchored just ONCE (at the first fetch) and free-runs continuously
  at the refresh rate, so it turns smoothly without resyncing on a data read. (PATCH:
  fixes the jump introduced in 2.4.13.)

## 2.4.13
- **Sweep arm now turns at the data-refresh rate.** The radar arm makes exactly one
  revolution per refresh interval (the "Refresh (seconds)" web setting / `REFRESH_MS`)
  instead of a fixed ~2.4 s spin, and it's phased to each fetch - it restarts at the
  top (12 o'clock) the moment new aircraft data lands and completes the scan just as
  the next pull is due, like a real PPI scope refreshing its blips. The per-step angle
  is derived from elapsed time (not accumulated steps) so it can't drift, and `%360`
  keeps it rotating at the right rate even if a fetch is delayed by API back-off. At the
  default 12 s refresh the arm rotates much more slowly than before, which also eases
  the per-frame PSRAM load. Removed the now-unused `SWEEP_STEP_DEG`; `SWEEP_REDRAW_MS`
  still caps the redraw cadence at ~10 fps. (PATCH: tweak to an existing feature.)

## 2.4.12
- **Added a PlatformIO build (`platformio.ini`) to enable the real RGB-panel fix.**
  The frame roll is PSRAM-bus starvation whose documented cures are ESP-IDF sdkconfig
  options the stock Arduino IDE can't set. The pioarduino platform's "hybrid compile"
  applies them via `custom_sdkconfig` while keeping the ordinary Arduino API, so the
  sketch code is unchanged. Enabled: `CONFIG_LCD_RGB_RESTART_IN_VSYNC` (auto DMA
  re-sync every frame - no more permanent roll), `CONFIG_SPIRAM_XIP_FROM_PSRAM` +
  `FETCH_INSTRUCTIONS` + `RODATA` (run code/const from PSRAM so the cache stays on and
  the LCD isn't starved), IRAM-safe GDMA/LCD ISRs, and a 64 KB/64 B data cache. Uses
  the 120 MHz `esp32s3_120_16_8-qio_opi` board profile and the existing `partitions.csv`.
  - **Libraries are vendored into `./lib`** (Arduino_GFX [patched with `getPanelHandle`],
    LovyanGFX, ArduinoJson) rather than scanning the global `C:/Arduino/libraries` -
    scanning that folder pulled unrelated libraries (one embeds `https_server.crt`) and
    broke the build.
  - **`custom_component_remove`** drops the IDF managed components arduino-esp32 pulls
    in by default but this project never uses (esp_insights/esp_rainmaker - which embed
    server certs and were the actual build-breaker - plus sr, zigbee, dsp, camera,
    modbus, modem, mp3, ...). Keeps mdns (ESPmDNS) and littlefs (coastline cache).
  - The Arduino IDE files are untouched (PlatformIO builds the .ino in place via
    `src_dir = .`), so the IDE remains a fallback.
  - First build is slow (~5 min; rebuilds the framework from source once, then caches);
    incremental builds are fast. Verified: RAM 31.7%, Flash 43.7% of the 3 MB app slot.
  - The 2.4.10 manual `esp_lcd_rgb_panel_restart()` self-heal is now redundant (the
    sdkconfig flag does it automatically) but left in as a harmless backstop.
  - **Build-log cleanup (no functional change).** Fixed the two `lmGen++`
    `-Wvolatile` warnings (now `lmGen = lmGen + 1`, identical effect, C++20-clean),
    added `-Wno-deprecated-declarations` to silence LovyanGFX's numeric-font
    `drawString`/`drawNumber` deprecation notices (the `IFont` migration is cosmetic;
    the overloads still work), and moved the `DBG`/`DBGLN` macros from the `.ino` into
    `config.h` so the `.ino`->`.cpp` converter stops warning on `__VA_ARGS__`. Build is
    now fully warning-clean.
  - **Repo housekeeping for publishing.** Expanded `.gitignore` to exclude all
    generated PlatformIO/IDF artifacts (`.pio/`, `build/`, `managed_components/`,
    `dependencies.lock`, `sdkconfig*`, `.dummy/`, logs); added a PlatformIO build
    section to the README; and changed the default `HOME_LAT`/`HOME_LON` to a neutral
    placeholder (London Heathrow) so a published copy doesn't ship a real address.
  - **Field note:** initial on-device testing shows the roll is noticeably improved.
    Further soak testing under heavy traffic + network activity still pending.

## 2.4.11
- **Diagnostics for the DMA re-sync.** Logs the captured panel handle at boot
  (`PANEL handle=...`) and the return code of `esp_lcd_rgb_panel_restart()`
  (`PANEL restart -> ...`), to confirm whether the 2.4.10 self-heal is actually taking
  effect or failing (e.g. invalid handle / invalid state). No behaviour change.

## 2.4.10
- **Self-heal the panel DMA so a roll no longer sticks (the "once it fails it never
  recovers" symptom).** Per the ESP-IDF docs, when the RGB DMA falls behind on PSRAM
  bandwidth the ESP32-S3 shifts the image PERMANENTLY until the DMA is restarted. The
  automatic fix (`CONFIG_LCD_RGB_RESTART_IN_VSYNC`) is a compile-time IDF flag that a
  stock Arduino IDE build can't set, but the runtime equivalent `esp_lcd_rgb_panel_restart()`
  is callable - it re-syncs the DMA cleanly at the next VSYNC. We now call it ~1 Hz from
  the render loop, which is invisible when the panel is already in sync and recovers a roll
  within a second instead of it sticking until reboot.
  - Requires a one-line getter added to `GFX_Library_for_Arduino`
    (`Arduino_ESP32RGBPanel::getPanelHandle()`) to reach the panel handle - re-apply that
    if the library is ever updated.
  - This is the recovery backstop; the pacing in 2.4.9 still reduces how often a roll
    happens in the first place.

## 2.4.9
- **Paced the frame build so it never monopolises the PSRAM bus (fixes the roll that
  appears as plane count grows / with the sweep on).** A redraw (a) draws the whole
  scene into the 768 KB PSRAM canvas - work that scales with the number of on-scope
  labels - and (b) copies that canvas to the panel frame buffer in one big
  PSRAM->PSRAM blast. Either, held too long, starves the LCD scanout and rolls the
  picture; that's why it got worse with more planes and failed fast with the sweep on.
  Now the canvas is copied in horizontal bands (`PUSH_BAND_H`) with a short bus-idle
  gap between them (`PUSH_BAND_GAP_US`), and the target-drawing loop releases the bus
  every few aircraft (`DRAW_GROUP` / `DRAW_GROUP_GAP_US`). The scanout's bounce-buffer
  refill now always gets bus time, so heavy frames no longer underrun it. All four
  values are tunable in config.h.

## 2.4.8
- **Sweep arm now animates at ~10 fps instead of 30 fps.** With the jitter otherwise
  fixed (120 MHz PSRAM + 16 MHz clock), the rotating arm was the last thing disturbing
  the display: it redraws and pushes the whole 768 KB canvas every step, and doing that
  30x/second still loaded the PSRAM bus enough to compete with the scanout. The arm now
  steps every 100 ms (15 deg/step), keeping the same ~2.4 s rotation with a third of the
  full-frame pushes. Data updates and taps still repaint immediately between steps. Tune
  via `SWEEP_REDRAW_MS` / `SWEEP_STEP_DEG` in config.h.

## 2.4.7
- **Pixel clock back to 16 MHz, now that PSRAM runs at 120 MHz.** With 120 MHz PSRAM the
  bus has enough bandwidth that the LCD DMA isn't starved by WiFi, so the 12 MHz jitter
  workaround is no longer needed - and 12 MHz was actually too low for this panel's timing
  to lock (it free-ran "slowly through colours" instead of showing the image). 16 MHz is
  the panel's native rate (~37 fps). Requires the Arduino IDE 120 MHz PSRAM setting
  (Flash Mode "QIO 120MHz" + OPI PSRAM); on 80 MHz PSRAM 16 MHz will jitter again.

## 2.4.6
- **Lowered the RGB pixel clock 16 -> 12 MHz to stop the WiFi-induced jitter.** The
  jitter is a documented ESP32-S3 RGB-panel limitation: the LCD DMA fetches the frame
  buffer from PSRAM and shares that bus with the CPU, so when WiFi is active the LCD can
  be starved and the picture shifts/jitters. The full cures (`CONFIG_LCD_RGB_RESTART_IN_VSYNC`,
  `CONFIG_SPIRAM_FETCH_INSTRUCTIONS`, `CONFIG_SPIRAM_RODATA`) are compile-time IDF
  sdkconfig flags not reachable from a stock Arduino IDE build. The accessible in-sketch
  mitigation is a lower pixel clock (Espressif/Waveshare recommend 10-12 MHz); 12 MHz is
  ~28 fps. NB: the earlier 14 MHz attempt looked "worse" only because it was bundled with
  the 20-line bounce buffer (since reverted) - this is a clean clock-only change.
- For a rock-steady display at the full 16 MHz, the hardware fix is running **PSRAM at
  120 MHz** (this N16R8 board supports it): in Arduino IDE pick a 120 MHz-capable board
  profile / flash mode (QIO 120 MHz) with OPI PSRAM. That roughly doubles PSRAM bandwidth
  and removes the starvation. (Optional, requires IDE Tools changes - not a sketch change.)

## 2.4.5
- **Reverted the 2.4.4 `WiFi.setSleep(false)` change** - it made the jitter worse, not
  better. That's a useful result: forcing the radio fully on increased WiFi's PSRAM-bus
  usage and the jitter got worse, which confirms the jitter is the panel scanout being
  starved of PSRAM bandwidth by WiFi activity (a hardware bus-contention limit on this
  board), rather than anything in our drawing/network code.

## 2.4.4
- **Disabled WiFi modem power-save (`WiFi.setSleep(false)`).** Intended to stop the
  beacon-wake bursts, but it made the jitter worse - reverted in 2.4.5.

## 2.4.3
- **Render loop heartbeat is now minute-resolution, not 1 Hz (fixes the ~1 s glitch /
  rolled picture with the sweep off).** With the sweep off the loop only needs to
  repaint on real changes; the old 1 Hz "keep the clock fresh" heartbeat was pushing
  the whole 768 KB canvas to PSRAM every second, and that periodic burst was underrunning
  the panel scanout - which showed as a glitch ~once a second and the image slipping up
  ("low / wrapped top-to-bottom"). The clock is only HH:MM, so the heartbeat now fires
  when the minute changes (plus a 30 s safety net) instead of every second.
- **Reverted the bounce buffer to 10 lines** (2.4.2's 20-line buffer didn't help and
  cost ~64 KB of internal SRAM that WiFi/TLS need; it likely made the per-second push
  underrun where 10 lines didn't).

## 2.4.2
- **Doubled the RGB bounce buffer (10 -> 20 lines)** to try to absorb PSRAM bursts.
  Didn't help and ate internal SRAM - reverted in 2.4.3.

## 2.4.1
- **Restored the on-scope data blocks** (callsign / flight-level + groundspeed / type
  next to each icon). 2.4.0 removed them entirely, but the intent was only to drop the
  *declutter cap / collision logic and the configurable count* - not the labels
  themselves. They're now drawn for every aircraft in range, as they were before the
  declutter feature, in the target's altitude colour with a short leader line.

## 2.4.0
- **Removed the on-scope label declutter feature.** Reverted the nearest-first cap,
  the per-tag collision test and the "On-scope labels" web-config option (and the
  `tagCount` / `TAG_COUNT_*` settings). The old saved `tagcnt` NVS key is simply
  ignored - no reconfigure needed. (2.4.1 restores the data blocks themselves.)

## 2.3.80
- **Cache the coastline projection (fixes the frame roll when the map overlay is on).**
  The shudder is actually the RGB panel losing vertical sync (a bounce-buffer underrun:
  the image rolls so the top lines wrap to the bottom). Testing pinned it to the
  coastline overlay, which was re-projecting all ~1200 points with haversine/bearing
  trig EVERY frame - a heavy CPU/PSRAM load that starved the scanout. The coastline
  only moves when the range or home changes, so it's now projected to screen
  coordinates once and cached (`lmScrX/lmScrY`, reprojected only on range/home/data
  change). Per frame it's just cheap line-clipping + drawing, which should stop the
  roll with the map overlay enabled.

## 2.3.79
- **Repaint-on-change render (fixes shudder with the sweep off).** The render loop
  used to redraw and push the entire 768 KB canvas to PSRAM ~30x/second regardless of
  whether anything changed, which kept the PSRAM bus near saturation and starved the
  panel scanout (the shudder). Now, when the radar sweep is OFF, the loop only repaints
  when something actually changed (new ADS-B data, a tap) plus a 1 Hz heartbeat for the
  clock/status line - so the panel sits static between the ~12 s data pulls and there's
  almost no per-frame PSRAM contention. With the sweep ON, behaviour is unchanged
  (still redraws every frame so the arm animates). Touch polling stays at ~30 Hz either
  way. To use it, turn off "Radar sweep arm" in the web config.

## 2.3.78
- **Revert the 2.3.77 panel-timing change.** Lowering the pixel clock to 14 MHz and
  doubling the bounce buffer made the display worse, not better, so panel timing is
  back to the known-good 16 MHz / 10-line bounce buffer. The shudder will be tackled
  a different way (see notes); the panel config is no longer the lever.

## 2.3.77
- **Widen RGB timing margin to stop the intermittent shudder.** The shudder is a
  bounce-buffer underrun: the full-screen canvas lives in PSRAM and is copied to the
  PSRAM framebuffer every frame while the panel's scanout DMA also reads PSRAM, so the
  bus runs near capacity. A periodic extra load (the 12s Wi-Fi data fetch, route
  lookups, denser traffic) tips it over and the scanout starves for a moment. Doubled
  the bounce buffer (10 -> 20 lines, ~64 KB internal RAM) so scanout can ride out
  longer PSRAM stalls, and lowered the pixel clock 16 -> 14 MHz (~32 fps, no visible
  flicker) for more refill headroom. Drop `RGB_PCLK_HZ` to 12 MHz if any shudder
  remains; lower `RGB_BOUNCE_PX` if the panel ever fails to start.

## 2.3.76
- **Fix display shudder from the label declutter pass.** The decluttering added in
  2.3.75 measured every candidate tag with `textWidth()` and kept scanning past the
  visible cap each frame. Since the whole UI is redrawn into PSRAM and pushed to the
  panel ~30x/second, that extra per-frame work contended with the panel's scanout DMA
  and made the image shudder. The pass now estimates the tag box from the character
  count (no font measurement) and stops as soon as the cap is met, restoring the
  per-frame cost to roughly what it was before.

## 2.3.75
- **On-scope labels: configurable count + declutter.** Previously only the nearest 8
  aircraft (plus the selected one) showed a data block, hard-coded. Now:
  - The number of labelled planes is configurable on the web config ("On-scope labels",
    0-24, default 8). 0 shows none (icons only); the selected plane is always labelled.
  - Tags are placed nearest-first and **decluttered** - a tag that would overlap one
    already drawn is skipped, so dense traffic no longer turns into unreadable
    overlapping text. Data blocks are also now drawn in a second pass on top of all
    icons so they're never painted over by other aircraft.

## 2.3.74
- **NTP self-heal.** `configTime()` only ran once on Wi-Fi connect; if the time
  server was unreachable at that moment the clock could stay unset (header stuck on
  `--:--`). The network task now checks whether the clock is valid and re-issues
  `configTime()` every 60 s until it syncs, then stops. The background SNTP daemon
  still handles its own retries; this just guarantees a kick if the first sync never
  landed.

## 2.3.73
- **Range can now be stepped down as well as up.** Added a `RNG-` button next to
  `RNG+` at the bottom of the panel, so the range can be decreased directly instead
  of having to cycle all the way up and wrap around to reach a smaller setting. Both
  buttons wrap (RNG- below the smallest jumps to the largest).
- **Smaller CFG button.** The `CFG` button is now a narrow target in the bottom-right
  corner to stop it being pressed by accident; the two range buttons share the rest
  of the width.

## 2.3.72
- **Routes: use the airport name, not the town.** The route line was showing the
  airport's municipality (e.g. East Midlands as "Castle Donington", or obscure villages
  for small fields). It now uses the airport's actual name, shortened by dropping
  generic words like "International"/"Airport" - so "Teesside International Airport"
  shows as "Teesside", "London Heathrow Airport" as "London Heathrow". Falls back to the
  town, then the IATA/ICAO code, if no name is available.

## 2.3.71
- **Added airline OCN** (Discover Airlines, the Lufthansa Group carrier formerly
  Eurowings Discover) to the callsign-to-airline table.

## 2.3.70
- **Draw nearest aircraft on top.** Targets are now plotted farthest-first, so the
  nearest aircraft and their data blocks are drawn last and sit on top instead of being
  overwritten by more-distant traffic.

## 2.3.69
- **Added A109 (AgustaWestland AW109)** to the offline aircraft-type table.

## 2.3.68
- **Added L159 (Aero L-159 ALCA)** to the offline aircraft-type table.
- **"Military" operator fallback.** When an aircraft is flagged military by the feed
  (`dbFlags`) and has no airline (callsign prefix) or adsbdb owner, the operator beside
  the callsign now shows "Military" instead of being blank.

## 2.3.67
- **Added an altitude colour legend** in the bottom-left corner of the radar (outside
  the scope circle): a small key showing the six altitude bands and their colours -
  cyan 30k+, green 20-30k, yellow 10-20k, orange 3-10k, red <3k, grey GND. Swatches are
  taken from the same `altColour()` table used to plot targets, so they always match.

## 2.3.66
- **Radar text no longer bleeds into the right panel.** Aircraft tags/data blocks
  drawn near the right edge of the scope could spill over into the right-hand info
  panel. The radar is now rendered with a clip rectangle bounded at the panel divider
  (`cx+radarR+12`), so scope content is kept on the radar side; the clip is cleared
  before the header and panel draw full width. A tag close to the edge is simply cut at
  the boundary rather than overwriting the panel.

## 2.3.65
- **Routes: off by default.** Flight route lookups now default to disabled, since the
  route data isn't always accurate. The feature is still compiled in and can be turned
  on from the Wi-Fi config page ("Look up flight routes"). Split the compile-time flag
  (`SHOW_ROUTES`) from the default state (new `ROUTES_DEFAULT`, 0), and the web label now
  notes the data may be inaccurate. (A device that already saved routes=ON keeps it on
  until toggled off; a fresh/never-saved device starts with it off.)

## 2.3.64
- **Routes: retry once on a connection drop.** A negative HTTP code (-1 refused, -11
  read timeout - common on a weak Wi-Fi link) now triggers a single immediate retry,
  the same as the aircraft fetch, instead of parking the route in a 30 s back-off. Helps
  routes for real airline flights resolve promptly when the link is flaky.

## 2.3.63
- **Routes: skip military / private / GA aircraft.** No route lookup is made for
  airframes the feed flags as military, PIA (privacy ICAO address) or LADD (limited
  data display), nor for GA/private traffic flying under its own registration
  (callsign == registration). This stops pointless queries - and the server-side 500s -
  on aircraft that never have a scheduled route (the `HELLCAT2`/`N2GZ`/`HM63` types in
  the logs). Added `dbFlags` to the parsed aircraft fields to drive this.

## 2.3.62
- **Routes: show town names, smaller text.** The route line now uses the airport's
  town/city (e.g. `Norwich > Aberdeen` instead of `NWI > ABZ`), falling back to the
  IATA then ICAO code if a town isn't listed. Names are ASCII-folded for the built-in
  font and the line is drawn in the smaller font (2), clipped to the panel width if a
  city pair is long. Route string buffers widened from 8 to 28 chars to hold names.

## 2.3.61
- **Route lookups: switch from POST `/routeset` to GET `/route/{cs}/{lat}/{lng}`.** The
  header log proved it: the POST was being answered `201` with a 0-byte `text/html`
  body (`CL='0' CT='text/html'`) - i.e. an edge/proxy in front of adsb.lol was
  intercepting the bare POST, never the real API (which returns `application/json`).
  Plain GETs to the same host work fine, and the API exposes a GET route endpoint that
  returns the same single route plus the `plausible` flag, so we now use that. Parses a
  single object (not an array) and is otherwise unchanged (plausible-gated, cached).
  Note: callsigns with no route in the DB may come back as a server error and simply
  show no route line - that's expected.

## 2.3.60
- **Route lookups: send `Accept: application/json` + log response headers.** The debug
  showed the device gets `HTTP 201` with a 0-byte body, while the same request from the
  API docs (which sends `accept: application/json`) returns the full JSON. The route
  POST now sends that header too. Also logs the response's Content-Length / Transfer-
  Encoding / Content-Type / Location so, if the body is still empty, we can see exactly
  what the proxy is returning.

## 2.3.59
- **Route lookups: fix empty response body (the real cause of "transient fail").**
  v2.3.57 correctly accepted the 201 status, but the body still failed to parse: the
  request used HTTP/1.0 mode (`useHTTP10`), and adsb.lol's `routeset` POST is answered
  201 with a chunked body through a proxy, which in HTTP/1.0 mode comes back as an
  empty string - so the parse failed and it fell through to "transient fail" anyway.
  The route POST now uses HTTP/1.1 (no `useHTTP10`), which reads the chunked POST body
  correctly. Also dropped the JSON filter (the payload is tiny - one plane) and added
  payload-length / parse-error logging so any future issue is obvious.

## 2.3.58
- **Route lookups: log a parse error.** If a 2xx route response can't be parsed, the
  reason is now logged (`ROUTE parse err: ...`) instead of silently being treated as a
  transient failure, so a genuine parsing problem can be told apart from a bad status
  code. (Version bumped so the running build is easy to confirm in the header/web UI.)

## 2.3.57
- **Route lookups: accept HTTP 201.** The adsb.lol `routeset` endpoint answers with
  201 (Created) on success, not 200, so the 2.3.56 code treated every (good) response
  as a transient failure and never showed a route (`ROUTE ... transient fail - retry`).
  Any 2xx is now accepted as a definitive, parseable response and the route array is
  read normally; a genuine miss is still cached so it isn't refetched in a loop.

## 2.3.56
- **Flight routes are back, via adsb.lol `routeset`.** The detail panel now shows
  the focused flight's route as `FROM > TO` (IATA codes). This uses adsb.lol's
  `/api/0/routeset` endpoint - the same route dataset tar1090 and ADSB-Exchange use -
  instead of the adsbdb route data we removed earlier. Crucially, we send the plane's
  live position with the query and the API returns a `plausible` flag that cross-checks
  the route against that position; **only plausible routes are shown**, so the kind of
  nonsense the old source produced (e.g. EWG39T "Nuremberg -> Canary Islands" when it
  was really Dusseldorf -> Edinburgh) is filtered out rather than displayed. Lookups
  are keyed by callsign, cached in RAM (one API hit per flight), aimed at the selected
  plane (or the nearest when nothing is selected), and back off on transient errors.
  New toggle **"Look up flight routes"** on the Wi-Fi config page (and `SHOW_ROUTES` in
  `config.h`) turns it off if the rate limits bite in a busy area.

## 2.3.55
- **OTA: solid-black framebuffer during flashing (the backlight can't be switched
  off).** Testing showed the screen stays *lit* during a flash write, so cutting the
  backlight via the CH422G (2.3.54) doesn't work - on this board the backlight isn't on
  the expander, and the `0x00` write only asserts the panel reset (which can make it
  worse). The shared octal-SPI bus still starves the PSRAM-backed RGB panel during
  flash writes, so the DMA underruns and shows shifted/garbled content. New approach:
  show "Updating - do not power off" briefly, then fill the whole framebuffer **solid
  black** for the entire write phase. An underrun that repeats/shifts solid black is
  still black, so there's nothing visibly garbled. Success/failure is drawn once
  flashing completes.

## 2.3.54
- **OTA: blank the panel during flashing.** A static framebuffer still corrupts
  during an update because the flash writes themselves starve the PSRAM-backed RGB
  panel (shared octal-SPI bus). The display now shows "Updating - do not power off"
  briefly, then the backlight/panel is switched off (via the CH422G expander) for the
  whole write phase so no garbage is visible, and is restored to show success/failure
  (or on reboot). I2C runs on GPIO so the toggle works even while the flash bus is busy.

## 2.3.53
- **OTA display corruption fix (proper).** On the S3-N16R8 the panel framebuffer is
  in PSRAM, which shares the octal-SPI bus with flash, so repainting the screen while
  `Update.write()` programs flash starves the panel DMA and corrupts the display. The
  status screen is now drawn exactly once at the start of the upload and the
  framebuffer is left untouched for the entire write phase (the per-64 KB progress
  redraw added in 2.3.51 actually made this worse). Success/failure is shown after
  flashing completes.

## 2.3.52
- **Bigger plane touch target.** Increased the on-radar tap hit radius from 24 px to
  36 px so aircraft are easier to select with a fingertip. It still picks the nearest
  plane within the radius, so close targets stay distinguishable.

## 2.3.51
- **Fixed flickering OTA update screen.** During a web firmware upload the render
  loop kept repainting the radar between upload chunks, so the "FIRMWARE UPDATE"
  screen flashed in and out. The render is now frozen while an upload is in progress
  (`otaActive`), the status screen shows throttled progress ("Receiving NN KB"), and
  aborted/failed uploads cleanly hand the screen back to the radar.

## 2.3.50
- **Added common UK military types** to the offline type table: fast jets (Typhoon,
  F-35, F-15, F-16, F/A-18, A-10, Tornado, Hawk, Texan II, Grob Tutor), transport/
  tanker/ISR (C-17, C-5M, KC-46, P-8 Poseidon, RC-135 Rivet Joint, B-52, U-2, C295)
  and helicopters/tiltrotor (Chinook, Apache, Merlin, Wildcat, Puma, Osprey, Black
  Hawk). These now resolve to readable names instead of bare ICAO codes.

## 2.3.49
- **Offline aircraft-type names (less reliance on adsbdb).** Added `aircraft_types.h`,
  a static ICAO type-designator -> full-name table (e.g. `B738` -> Boeing 737-800,
  `A20N` -> Airbus A320neo), mirroring the `airlines.h` approach. The short type
  code already arrives in the ADS-B feed, so the full name now resolves instantly and
  reliably with no API call. adsbdb is only queried for type codes the table doesn't
  know, so flaky lookups / 404s (e.g. `TYPE HTTP 404` for airframes missing from
  adsbdb) no longer leave the type blank. Note: for known types the adsbdb call is
  skipped entirely, so the airline shown comes from the callsign prefix
  (`airlines.h`) rather than the adsbdb registered owner.

## 2.3.48
- **Tidier config page layout.** Sub-options ("Keep taxiing aircraft" + its speed
  field, and "Label airports with full name") are now shown in a properly indented,
  bordered block under their parent toggle instead of using `&nbsp;`/arrow hacks that
  left text inputs stranded mid-list. The Home label field moved up beside the
  latitude/longitude (location) fields.

## 2.3.47
- **Airport labels: code or full name.** New "label with full name (else IATA code)"
  option under "Show major airports" lets the scope label airports with their full
  name (e.g. `Heathrow`) instead of the IATA code (`LHR`). Each airport in the
  built-in table gained a name; default stays as the code (`AIRPORT_NAMES` in
  `config.h`), editable from the Wi-Fi config page.

## 2.3.46
- **Ground data block shows speed.** The on-scope tag for an aircraft on the ground
  now reads `GND  <gs>` (e.g. `GND  15`) instead of just `GND`, so a taxiing plane's
  speed is visible at a glance - matching the airborne `FL  speed` format.

## 2.3.45
- **Clearer GND taxi wording on the config page.** The "keep taxiing aircraft"
  checkbox no longer dangles on the word "above"; the speed value now has its own
  label ("Keep if ground speed above (kt)").

## 2.3.44
- **Removed the I2C touch-scan debug mode.** Dropped the `TOUCH_SCAN` boot mode,
  its `runTouchScan()` routine and the `config.h` switch - it was only needed while
  bringing up the touch controller and isn't useful now.

## 2.3.43
- **Tidier detail panel.** The climb/descent/level state moved onto the altitude
  line in brackets (e.g. `Alt 35000ft (Climbing)`) instead of its own row, and the
  "Type " label was dropped so the aircraft type/variant shows on its own.

## 2.3.42
- **GND filter can keep taxiing aircraft.** When "Hide aircraft on the ground" is
  on, a new "keep taxiing aircraft above N kt" sub-option keeps ground traffic that's
  rolling faster than a configurable threshold (default 5 kt) while still dropping
  parked/slow aircraft. New `GND_KEEP_TAXI` / `GND_TAXI_KT` defaults in `config.h`,
  both editable from the Wi-Fi config page.

## 2.3.41
- **Sweep / coastline / airports now toggleable from Wi-Fi config.** Added runtime
  checkboxes for the radar sweep arm, the coastline map overlay and the major-airport
  markers, stored in flash. The `SWEEP_ENABLE` / `SHOW_LANDMARKS` / `SHOW_AIRPORTS`
  defines in `config.h` now act as the compile-time master *and* the default (set to
  0 to drop the feature entirely; leave at 1 to build it in and control it from the
  web page). Disabling the coastline also stops it downloading.

## 2.3.40
- **Hide-ground-traffic filter.** New "Hide aircraft on the ground (GND)" option
  drops parked/taxiing aircraft (`alt_baro == "ground"`) from the feed so the scope
  shows airborne traffic only. Configurable from the Wi-Fi config page and stored
  in flash; default is off (`HIDE_GROUND` in `config.h`). Refresh interval and all
  other behaviour options remain editable from the same page.

## 2.3.39
- **Fixed splash flicker.** `connectWiFi()` was repainting the (empty) radar
  screen mid-boot, so the sequence flashed splash -> radar -> splash. It now keeps
  the splash up during the connect, so the boot flow is a clean
  splash -> "Ready" -> radar.

## 2.3.38
- **Splash shown longer.** Raised the minimum on-screen time from 2.5 s to 5 s so
  the boot title card is comfortably readable instead of flashing past.
- **Selected NEARBY row fully highlighted.** The selected row's text now draws on
  the highlight-bar colour instead of black, so the whole row reads as one solid
  highlight (previously each glyph sat on a black box inside the coloured bar).

## 2.3.37
- **Selection now sticks across refreshes.** A tapped plane stays pinned in the
  detail panel until it leaves the feed, moves beyond the current range (off the
  scope), you select another plane, or you tap empty space. Previously the
  selection was cleared on every data refresh (it was an index into a list that
  re-sorts each cycle), so it appeared to "time out". The selection is now keyed
  by the aircraft hex and re-resolved against the new list each refresh.
- **Snappier touch.** Lowered the touch confirm hold from 70 ms to 40 ms (~1
  render frame). Re-selecting a different plane now registers on the first tap
  instead of taking a few presses, while single-frame / jittery EMI ghost touches
  are still rejected by the stability check.
- **Boot splash screen.** Added an ATC-style title card shown during startup /
  Wi-Fi connect: a green PPI scope (rings, crosshair, frozen sweep, colour-coded
  blips) behind the blue "FLIGHT RADAR" wordmark, with the firmware version and a
  live boot-status line ("Connecting Wi-Fi...", "Ready").

## 2.3.36
- **Radar sweep only runs with a live ADS-B feed.** The rotating arm is now hidden
  (and frozen) until the first successful aircraft fetch, and stops again if the
  connection drops (`dataOk`). A still scope cleanly signals "not receiving / not
  ready" instead of sweeping over an empty display during boot/Wi-Fi setup.

## 2.3.35
- **Airline moved beside the callsign** (same row, in light amber) instead of on
  its own line under it. Sits just right of the flight ID, vertically aligned, and
  clips to the remaining panel width. Frees a line of vertical space in the panel.

## 2.3.34
- **Expanded the airline code table** (`airlines.h`) from ~30 to ~140 carriers,
  covering the major and mid-tier airlines worldwide (Europe, Middle East, Africa,
  Asia-Pacific, the Americas and cargo). A genuinely exhaustive ICAO list (~1500
  designators, many defunct) isn't practical to hand-maintain and risks wrong
  names, so anything outside the table still resolves via the adsbdb
  `registered_owner` fallback - between the two, virtually all traffic is covered.

## 2.3.33
- **Bigger airport markers.** The dot/ring grew (filled r2->r3, ring r4->r6) and
  the IATA code is now drawn in font 2 (was the tiny font 1), for better
  legibility on the scope.

## 2.3.32
- **Airline now derived from the callsign prefix (`airlines.h`).** The ICAO
  callsign prefix identifies the operator instantly with no API call and no 404s
  (e.g. `RYR1JG` -> Ryanair, `KLM65H` -> KLM, `BAW123` -> British Airways). A
  curated, UK/Europe-weighted prefix->name table is now the primary source for the
  airline line, shown immediately for the NEAREST plane; the adsbdb
  `registered_owner` is kept as a fallback for prefixes not in the table. GA
  traffic (registration callsigns like `GABCD`/`N12345`) has no airline prefix and
  is correctly left blank. Extend `AIRLINES[]` to add more carriers.

## 2.3.31
- **Type/airline lookups now retry after a transient failure.** Previously *any*
  non-200 response from adsbdb (a 202 "queued", a 429 rate-limit, a 5xx, or a
  dropped TLS connection) was cached as "unknown" (`state 3`) just like a real
  404, so the airframe was never looked up again. Now only a genuine **404** is
  remembered as permanently unknown; transient failures are **not cached** and the
  request is parked (state 4) and retried after a 30 s back-off, so the type and
  airline eventually fill in once adsbdb responds.

## 2.3.30
- **Airline now shows for the NEAREST plane without tapping it.** The owner line
  was only drawn when the adsbdb lookup had fully resolved the *type* (`state==2`);
  it now shows whenever an owner is cached for the airframe, independent of the
  type. The lookup is already auto-aimed at the nearest in-range plane when
  nothing is selected, so the airline appears on its own. (Note: GA / private
  aircraft often have no airline in adsbdb, so the line stays blank for those.)
- **Header title "FLIGHT RADAR" is now blue** (was white).

## 2.3.29
- **Airline / operator shown under the callsign in the NEAREST/SELECTED panel.**
  Pulled from the `registered_owner` field of the *same* adsbdb aircraft lookup we
  already make for the extended type - so it costs no extra API calls and is
  cached in RAM alongside the type (each airframe fetched once). Drawn in light
  amber, clipped to the panel width. Shows for the nearest plane automatically and
  for any tapped flight.

## 2.3.28
- **NEAREST now shows the extended aircraft type too.** Previously the full
  "Manufacturer model" name only appeared for a *tapped* (selected) flight; the
  auto-shown NEAREST plane only had the short ICAO code. The full-type lookup now
  follows the nearest in-range plane whenever nothing is selected, and the panel
  reads the resolved name straight from the RAM type cache - so NEAREST gets the
  long name with no extra API traffic (each airframe is still fetched only once).
- **Date and time split apart in the header.** They were one run of text; now the
  date sits just left of centre and the time just right, with a small gap.
- **Major airports plotted on the scope (`SHOW_AIRPORTS`).** A built-in table of
  ~25 UK + near-neighbour airports is drawn as a small grey dot + ring with the
  IATA code, for any airport within the current range. Deliberately a static
  offline table rather than a live Overpass query - airports don't move and it
  avoids another flaky network dependency. Set `SHOW_AIRPORTS 0` in `config.h` to
  hide them, or extend the `AIRPORTS[]` list for other regions.

## 2.3.27
- **TRACKED now shows the exact number of planes on the scope.** It previously
  showed the raw feed count (`acCount`), which could exceed what's actually
  plotted when aircraft sit just past the range edge. It now counts only planes
  within `rangeNm` - the same set drawn as dots on the radar.

## 2.3.26
- **Reducing the range now immediately clears out-of-range planes from the panel.**
  The radar scope already culled aircraft beyond the selected range, but the
  "NEAREST" pick and the "NEARBY" list iterated the full aircraft list, so after
  zooming in they kept showing planes that were now off-scope (until the next
  fetch, and any the feed returned just past the boundary). Both now respect
  `rangeNm`: NEAREST only uses the closest plane if it's actually within range
  (else "No aircraft in range."), and the NEARBY list stops at the range edge
  (the list is sorted nearest-first, so it breaks cleanly).

## 2.3.25
- **Handle Overpass server-side timeouts / throttling.** With the truncation bug
  fixed, the failure moved upstream: `overpass-api.de` was returning HTTP 200 with
  a `"remark"` (its `[timeout:25]` compute budget expired - it managed 700 points
  then gave up), so the map never completed. Fixes:
  - Server-side query budget raised `[timeout:25]` -> `[timeout:50]`, and the
    client/HTTP read timeouts raised to ~55 s to cover that compute window.
  - **Mirror rotation:** each attempt now rotates across three public Overpass
    endpoints (kumi.systems first - usually the fastest - then overpass-api.de,
    then maps.mail.ru), so a retry lands on a different, less-loaded backend.
  - **Escalating back-off:** consecutive failures widen the retry gap
    (60s, 120s, ... up to 5 min) instead of a flat 60 s, to stay within Overpass
    fair-use limits and avoid being rate-limited for hammering.
  - Note: a coastline fetch can now block the network task for up to ~55 s in the
    worst case (slow mirror), briefly pausing aircraft refreshes while the map
    builds. This is rare and only happens while (re)building the coastline.

## 2.3.24
- **Added a "Refresh coastline map now" button to the web config page.** Forces a
  fresh Overpass download for the current location without rebooting. The button
  sets a flag that `netTask` (core 0, which owns the network) picks up on its next
  cycle, clearing any download back-off so it fetches immediately; a forced fetch
  that fails keeps retrying every 60 s until it succeeds. Lives on the web UI
  (device IP / `flightradar.local`) rather than the on-device setup screen, since
  the on-device AP portal has no internet connection to download from.

## 2.3.23
- **Fixed the coastline never appearing on the scope.** The `LM draw:` diagnostic
  from 2.3.20 revealed the smoking gun: `drawn=0 clipped=489` with the nearest
  cached point 127 nm away, even at a 50 nm range. The download was being
  *truncated* before it reached the local coast - older fetches stopped at either
  the 800 KB byte cap (130 nm fetch: 597 pts, "800001 bytes scanned") or the
  700-point cap (65 nm fetch: 700 pts), keeping only whatever coastline Overpass
  happened to return first (by way ID, i.e. geographically random). The partial
  result was then committed as "complete", so the refetch logic never tried again.
  Fixes:
  - Coastline coverage cap lowered 120 nm -> 90 nm so the Overpass response is
    small enough to download in full.
  - Decimation coarsened (`minSep = fetchR/120`, was `/350`) and `LM_MAX_PTS`
    raised 700 -> 1200 so the whole bbox fits without hitting the point cap.
  - Raw-byte safety stop raised 800 KB -> 2.5 MB, and hitting it (or a stall /
    Overpass "remark") is now treated as *incomplete*: the partial result is
    discarded and retried instead of being cached as good.
  - `LM_STORE_VER` bumped 3 -> 4 to discard the existing truncated cache on first
    boot and force one clean refetch.

## 2.3.22
- **Removed the origin/destination route feature.** adsbdb maps routes by
  *callsign*, and that mapping is frequently stale - e.g. EWG39T was reported as
  Nuremberg->Gran Canaria while the airframe was actually flying
  Dusseldorf->Edinburgh. Callsigns get reused across different routes, so any
  free callsign-lookup gives unreliable results. Rather than show wrong data, the
  route lookup, RAM cache, travel-direction logic and panel line are all gone.
  The on-tap **aircraft type** lookup (also from adsbdb, by Mode-S hex) stays - it
  keys on the airframe, not the callsign, so it's reliable.

## 2.3.21
- **Diagnostics for route orientation.** Added DEBUG logging of the on-screen
  travel-direction decision (track, bearing to each airport, and the resulting
  `left > right`) so the *displayed* route order can be compared against the
  plane's actual heading - the existing `ROUTE` log line only shows the filed
  origin->destination, not what the panel draws.

## 2.3.20
- **Diagnostics for the coastline overlay.** Added DEBUG logging to
  `drawLandmarks()` reporting, every ~5 s, how many segments are actually drawn
  vs clipped (plus count/range/geometry and the first point), to pin down why a
  loaded coastline cache isn't appearing on screen.

## 2.3.19
- **Ghost-touch filter reworked (duration + stability).** The previous
  fixed-sample burst couldn't reject phantoms that stayed stable for ~21 ms, so
  EMI bursts still auto-cycled the range and opened the setup portal (which
  blanked the radar/map - the "landmarks not working" symptom; the coastline
  cache itself was loading fine all along). `readTouch()` now debounces by time:
  a press must stay within a few pixels for ~70 ms of continuous contact before
  it registers. A real finger holds still; an EMI burst jitters between polls and
  keeps resetting the timer, so it never qualifies. Non-blocking, so no render
  stutter, and once a press qualifies it tracks until release.

## 2.3.18
- **Routes are now cached in RAM too** (keyed by callsign, up to 32 flights,
  ring-buffer eviction), mirroring the type cache. Re-selecting a flight - or
  seeing it again after a refresh - reuses the stored origin/destination (and
  airport coordinates for the travel-direction check) with no adsbdb call. Both
  resolved routes and "n/a" results are remembered to avoid re-querying.

## 2.3.17
- **Accented place/type names now render** (e.g. `Montréal` -> `Montreal`). The
  built-in fonts only cover ASCII, so a `UTF-8 -> ASCII` transliteration folds
  common Latin-1 Supplement and Latin Extended-A letters down to their closest
  ASCII form. Applied to route city names and the full aircraft type.

## 2.3.16
- **Routes now orient to the direction of travel.** adsbdb only returns the
  filed origin->destination for a callsign, not which leg/direction the airframe
  is currently flying, so routes could read "backwards". The route fetch now also
  pulls the origin/destination airport coordinates, and the panel compares the
  selected aircraft's live track against the bearing to each airport: whichever
  it's heading toward is shown on the right. If it's flying the reverse leg the
  `from > to` is swapped so it always reads in the direction of flight. Falls
  back to the filed order when the track or coordinates are unavailable.

## 2.3.15
- **Aircraft types are now cached in RAM** (keyed by Mode-S hex, up to 40
  airframes, ring-buffer eviction). Selecting a plane we've already resolved
  reuses the stored type instantly with no adsbdb call; only first-time hexes
  trigger a lookup. Both known types and 404s are remembered, so unknown
  airframes aren't re-queried every refresh either.

## 2.3.14
- **Coastline coverage capped at 120 nm**, independent of the aircraft range
  (which can still go to 150 nm). An Overpass query for a large bbox is slow and
  often times out (`-11`); limiting coverage keeps the map query fast and
  reliable. At the widest zoom the coastline just stops a little short of the
  scope edge. The refetch trigger now compares the capped target radius (not the
  raw range) against the cached radius, so the widest zoom no longer refetches
  the map every cycle.

## 2.3.13
- **Landmark fix - the real cause.** Ghost touches were still cycling the range
  up to 250 nm on their own; that range's ~264 KB aircraft payload dropped free
  RAM to ~2 KB, which then starved the HTTPS coastline fetch of TLS memory (it
  failed with `-11`). Three changes:
  - The touch filter now requires four consecutive in-tolerance samples
    (~21 ms) before registering a press, killing the noise bursts that the
    earlier two-sample check let through. Real taps are unaffected.
  - Dropped the **250 nm** range step (max is now 150 nm); 264 KB is too large
    to buffer alongside a TLS session on this board's internal RAM.
  - The coastline fetch now **defers** (retries in 15 s) when free heap is below
    100 KB instead of attempting a connection that's bound to fail.

## 2.3.12
- Feed entries whose callsign starts with **`TXLU`** (e.g. `TXLU00`) are now
  filtered out during parse - these are TIS-B/non-aircraft artifacts, not real
  aircraft, so they no longer appear on the scope or in the NEARBY list.

## 2.3.11
- **Snappier touch.** The phantom-touch filter now confirms a press with a
  second sample ~8 ms later *within the same poll* instead of waiting for a
  second render frame, so taps no longer feel laggy. Idle frames add no delay.
- **Type/Reg layout.** The full aircraft type stays on a single row (no forced
  extra line); if it's too long it's clipped with an ellipsis to fit the left
  column. The right column (Reg / Spd / Dst) is nudged right so the type name
  gets more room. The NEARBY list returns to its original position (5 rows).

## 2.3.10
- **Phantom-touch rejection.** The 7" RGB bus radiates noise onto the GT911's
  lines, producing occasional single-frame ghost touches at random coordinates.
  These were making the unit drop into - and (since 2.3.8's BACK button) bounce
  in and out of - the Wi-Fi config portal on its own, which tore down Wi-Fi and
  blanked the radar/map each cycle. `readTouch()` now bounds-checks the
  coordinate and requires two consecutive stable samples before reporting a
  press, so jumping ghosts are ignored while real taps still register.

## 2.3.9
- The selected aircraft's panel now shows the **full type** (e.g.
  `Boeing 777-236ER`) on its own line, looked up on tap from adsbdb.com by
  Mode-S hex and cached per aircraft (same pattern as the route). The
  registration moves to its own line beneath it. On-scope data-block tags keep
  using the short ICAO code (e.g. `B772`), since each can't make a network call.
  Falls back to the short code while the lookup is pending or unavailable. The
  NEARBY list starts a touch lower to make room for the taller detail block.

## 2.3.8
- Added a **`< BACK` button** to the Wi-Fi setup portal screen. Tapping it leaves
  the portal and reconnects to the saved network **without a reboot**. The
  switch-back runs on the network core (core 0, which owns Wi-Fi), matching the
  crash-safe way the portal is started.

## 2.3.7
- On-scope data-block text (callsign / flight-level / type) is now drawn in the
  target's **altitude colour**, matching its icon, so the label encodes height
  too. (The selected target still gets its white highlight rings.)

## 2.3.6
- Route panel: origin/destination now shown on a **single line** in
  `from > to` format, trimmed with an ellipsis (`...`) so it never runs past the
  panel's right edge.

## 2.3.5
- Coastline refetch now streams into a **scratch buffer** and only swaps the new
  data into the live cache once a *complete* response arrives. The existing map
  stays on screen throughout a refetch and is left untouched if the download
  stalls or errors out (no more blank flicker during retries).

## 2.3.4
- **Fixed the landmark cache getting stuck empty after a range change.** A
  truncated / timed-out download or an Overpass `"remark"` (server-side
  timeout / rate-limit) is now treated as a *failure* and retried with backoff,
  instead of being wrongly committed as a valid empty result (which previously
  left the map blank and stopped all further attempts).
- Bumped the on-flash cache version (`LM_STORE_VER`) so a previously-saved bad
  (empty) cache is discarded on the next boot.
- Radar scope nudged **left 5 px**.

## 2.3.3
- Radar scope nudged **down a further 5 px** (10 px total) for better centring.

## 2.3.2
- Added a bundled partition table (`partitions.csv`) providing two OTA app slots
  **and** a `spiffs` data partition, so the LittleFS coastline cache persists
  across reboots. Fixes `partition "spiffs" could not be found` /
  `no LittleFS partition - coastline cached in RAM only`. README updated.

## 2.3.1
- Header: aligned the **WiFi / No WiFi** text with the title and date/time.

## 2.3.0
- **Removed all 2.8" board support** (ELEGOO ESP32-WROOM + ST7789 + CST328 +
  TFT_eSPI). The project is now exclusively the Waveshare ESP32-S3-Touch-LCD-7.
  Deleted `board_28.h`, `User_Setup.h` and the unused `LGFX_Config.h`; removed
  the board-selection switch from `config.h`; rewrote the README for a single
  board.

## 2.2.2
- Route lookup now shows origin/destination **cities** (e.g. `London`,
  `New York`), falling back to the airport code when a city isn't published,
  instead of bare IATA/ICAO codes.

## 2.2.1
- **Fixed a reset when tapping CFG.** The Wi-Fi setup portal is now started from
  the network task (core 0, which owns Wi-Fi) instead of the render core, so the
  Wi-Fi teardown no longer races an in-flight TLS fetch on the other core. Also
  guarded the main-loop Wi-Fi reconnect so it never runs while the AP portal is
  active.
- Verified route direction (origin = departure) against the live adsbdb API.

## 2.2.0
- **Switched the 7" panel scanout from LovyanGFX to Arduino_GFX** with a hardware
  bounce buffer, eliminating the horizontal jitter/shear. LovyanGFX is retained
  only as the off-screen drawing canvas (`LGFX_Sprite`) and the GT911 touch
  driver.
- Fixed colour byte-order (big-endian sprite → `draw16bitBeRGBBitmap`).
- Moved the coastline cache from NVS to LittleFS (avoids the NVS / RF-calibration
  `store calibration data failed 0x1105` boot issue).

## Earlier (pre-2.2.0)
The 2.1.x series was the dual-board era (Waveshare 2.8" ST7789 + the 7" RGB
panel) and built up the core feature set: live ADS-B PPI radar scope with
altitude-coloured targets and history trails, an ATC-style compass rose / range
rings / data blocks, dual-core networking (fetch on core 0, render on core 1),
touch selection + range/CFG buttons, on-demand origin→destination route lookup,
dynamic OpenStreetMap coastline overlay, NTP clock with date, a web config
portal, mDNS, and web OTA firmware updates.

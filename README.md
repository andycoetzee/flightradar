# ESP32 Desk Flight Radar

A small, touch-driven "flight radar" for your desk. An ESP32 board pulls live
ADS-B aircraft positions from a free public API and plots them on a touch LCD as
a classic PPI radar scope: you sit in the centre, range rings show distance, each
aircraft is a triangle pointing the way it's heading and colour-coded by altitude,
with an animated sweep. Tap an aircraft for its details; tap the **RANGE** button
to zoom.

No account, no API key, no SD card and no extra fonts required.

**Target board:** Waveshare **ESP32-S3-Touch-LCD-7** — a 7" 800×480 16-bit RGB
parallel panel with GT911 capacitive touch and a CH422G I/O expander, all on one
PCB.

| Board | Display | Touch | Graphics lib |
|-------|---------|-------|--------------|
| Waveshare ESP32-S3-Touch-LCD-7 | 7" 800×480 RGB parallel | GT911 (I²C) | Arduino_GFX (+LovyanGFX canvas) |

The display/touch code lives in `board_7in.h`; everything else (Wi-Fi, data
fetch, web config, OTA, type lookup) is in `FlightRadar.ino`.

---

## 1. Hardware

| Item | Notes |
|------|-------|
| Waveshare ESP32-S3-Touch-LCD-7 | ESP32-S3-N16R8 (16 MB flash, 8 MB PSRAM) |
| (built-in) 7" 800×480 IPS panel | 16-bit RGB parallel, GT911 touch, CH422G I/O expander |
| USB-C cable | Power + flashing |

This board is a **single integrated unit** — the panel, touch, expander and
ESP32-S3 are all on one PCB, so **there is no wiring to do**. Just plug in USB-C.
The RGB data/sync pins are fixed in `board_7in.h`; the GT911 touch and CH422G
expander sit on the I²C bus defined in `config.h` (`TP_SDA_PIN 8` /
`TP_SCL_PIN 9`). The CH422G drives the backlight and the LCD/touch reset lines,
so the sketch brings it up over I²C at boot before initialising the panel.

> PSRAM is **required** on this board (the RGB framebuffer and the 16-bit back
> buffer live there). Make sure PSRAM is enabled in Tools (see §3).

---

## 2. Build & upload with PlatformIO (recommended)

PlatformIO is the recommended way to build this project. It applies the ESP-IDF
`sdkconfig` options that cure the RGB-panel image roll (PSRAM-bus starvation) —
options the stock Arduino IDE can't set. Everything is already wired up in
`platformio.ini`, and the three required libraries are vendored in `lib/`, so the
build is self-contained — no Library Manager or Boards Manager steps.

1. **Install PlatformIO** — the [VS Code extension](https://platformio.org/install/ide?install=vscode),
   or the CLI (`pip install platformio`).
2. **Build** from the project folder:
   ```
   pio run
   ```
   The first build is slow (~5 min — it compiles the Arduino framework from source
   once, then caches it) and downloads the pioarduino platform plus the ESP-IDF
   managed components automatically.
3. **Flash and watch the log:**
   ```
   pio run -t upload -t monitor
   ```
   (Serial Monitor is 115200 baud.)

> Build settings live in `platformio.ini`: the `esp32s3_120_16_8-qio_opi` board
> profile (120 MHz PSRAM, octal mode), `partitions.csv`, and the `custom_sdkconfig`
> block that enables the RGB DMA auto-resync and runs code/const from PSRAM so the
> LCD scanout isn't starved. The generated `.pio/`, `managed_components/`,
> `sdkconfig*` and `.dummy/` are all reproducible and are git-ignored.

The Arduino IDE flow below (§3) still works as a fallback — the `.ino` is built in
place — but it does **not** apply the display-stability `sdkconfig` options, so the
image roll can return under heavy Wi-Fi + traffic load.

---

## 3. Arduino IDE setup (alternative)

1. **USB driver** – the ESP32-S3 enumerates as native USB and usually needs no
   driver. If no COM port appears, check the cable is data-capable.

2. **ESP32 board support** – in *File → Preferences → Additional Boards Manager
   URLs* add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
   Then *Tools → Board → Boards Manager…* → install **esp32 by Espressif Systems**.

3. **Select the Arduino board + options** (*Tools → Board*):
   - Board: **ESP32S3 Dev Module**
   - Flash Size: **16MB (128Mb)**
   - **PSRAM: OPI PSRAM** *(required — the panel won't init without it)*
   - USB CDC On Boot: **Enabled** (so the Serial Monitor works over native USB)
   - Partition Scheme: see step 5

   Then pick the right **Port**.

4. **Install libraries** – *Tools → Manage Libraries…*:
   - **ArduinoJson** by Benoit Blanchon (v7 or newer)
   - **GFX Library for Arduino** by moononournation
   - **LovyanGFX** by lovyan03

   **Arduino_GFX** drives the RGB panel with a hardware *bounce buffer* (scanout
   is fed from internal SRAM, which is what keeps the image jitter-free), while
   **LovyanGFX** is used as the off-screen drawing canvas *and* the GT911 touch
   driver — it no longer drives the panel. `WiFi`, `WebServer`, `DNSServer`,
   `ESPmDNS`, `Update` and `Preferences` ship with the ESP32 core.

   No library file needs editing: the panel/touch config lives in this project's
   `board_7in.h` and is compiled directly; nothing is copied into any library
   folder.

5. **Partition scheme** – this project **ships its own partition table**
   (`partitions.csv` in the sketch folder). The Arduino IDE picks it up
   automatically and uses it instead of the *Tools → Partition Scheme* menu, so
   there's nothing to choose here. It provides two 3 MB OTA app slots **and** a
   ~9.8 MB `spiffs` data partition, which is what lets the coastline cache
   persist across reboots and keeps OTA working.

   > The IDE's "Sketch uses X%" figure still comes from the *Tools → Partition
   > Scheme* menu (it can't read `partitions.csv`). Leave that menu on a scheme
   > with a **≥ 3 MB** app slot — e.g. **"16M Flash (3MB APP/9.9MB FATFS)"** — so
   > the size report is accurate. The actual layout always comes from
   > `partitions.csv`.

---

## 4. Configure it (web setup – no code editing needed)

You can configure everything from a browser; the values in `config.h` are only
the *defaults* used the very first time. Once you save from the web page,
settings are stored in the ESP32's flash and survive reboots and re-flashes.

**First run / Wi-Fi setup portal**

1. On first boot (or if Wi-Fi can't connect), the screen shows **WI-FI SETUP
   MODE** and the ESP32 creates its own Wi-Fi hotspot.
2. On your phone/PC, join the Wi-Fi network **`FlightRadar-Setup`**
   (password `flightradar`, both changeable in `config.h`).
3. A setup page should pop up automatically (captive portal). If not, open
   **http://192.168.4.1** in a browser.
4. Enter your Wi-Fi, your latitude/longitude, range, units, timezone and data
   source, then tap **Save & Reboot**. The device restarts and connects.

**Changing settings later**

- Tap the **CFG** button on the screen at any time to re-open the setup portal
  (the device becomes the `FlightRadar-Setup` hotspot again), **or**
- While it's connected to your Wi-Fi, browse to the device's IP address on your
  network (printed to the Serial Monitor as `Config UI: http://...`).

> Prefer to hard-code instead? You can still preset the defaults in `config.h`
> (`WIFI_SSID`, `WIFI_PASS`, `HOME_LAT`, `HOME_LON`, `GMT_OFFSET_SEC`,
> `DEFAULT_RANGE_NM`, `USE_METRIC`, `API_HOST`). Saved web settings always win;
> to wipe them back to these defaults, re-flash after an *Erase All Flash
> Contents* (Tools menu) or change a setting in the web page.

---

## 5. Upload & run

1. Open `FlightRadar.ino` in the Arduino IDE (keep all the files in the same
   `FlightRadar` folder).
2. Click **Upload**.
3. Open **Serial Monitor** at **115200** baud to watch status messages.

You should see the radar appear, "WiFi OK", then aircraft populate within a few
seconds.

**Serial debugging.** `config.h` has a `DEBUG` switch (default `1`). With it on, the
UART mirrors every on-screen status message plus the loaded settings, the Wi-Fi
result and IP, each request URL, the HTTP status code, and JSON errors — handy for
validating behaviour or diagnosing a data problem. Set `DEBUG 0` for a quiet build
(the logging compiles away to nothing; only the boot banner and hardware
warnings remain). Set `DEBUG_AIRCRAFT 1` to also print one line per received
aircraft.

### Using it
- **Tap an aircraft** → it gets highlighted and its full details show in the side
  panel (callsign, type, registration, altitude, speed, heading, distance,
  climb/descent). The panel also looks up the **full aircraft type** for that
  airframe from adsbdb.com by Mode-S hex (e.g. `Boeing 777-236ER`); the on-scope
  tag keeps the short ICAO code (e.g. `B772`).
- **Tap empty space** → deselect.
- **Tap RNG+** → cycle the scope range 25 → 50 → 100 → 150 nm.
- **Tap CFG** → open the Wi-Fi/settings web portal (see §4).
- The side panel also shows a tappable **NEARBY** list of the closest aircraft
  (callsign + altitude + distance) — tap a row to select that flight without
  having to hit its dot on the scope.

The scope is styled like an **air-traffic-control display**: a degree-marked
compass rose (labelled every 30° in tens-of-degrees, e.g. `09` = 090°), range
rings with distance labels, an own-position bullseye at the centre, a **velocity
leader line** projecting ahead of each target (longer = faster), **data blocks**
(callsign / flight-level + groundspeed) for the nearest targets and the selection,
and fading **history trails** showing where each aircraft has been.

Altitude colours (symbols **and** their trails): grey = on ground, red < 3 000 ft,
orange < 10 000, yellow < 20 000, green < 30 000, cyan ≥ 30 000.

### Map overlay (coastline / landmarks)

A faint **map overlay** is drawn under the traffic. The **coastline is fetched
dynamically** from [OpenStreetMap](https://www.openstreetmap.org)'s free, keyless
Overpass API for whatever **home location** you configure — so it's correct
wherever you are, with no editing. Anything outside the current range ring is
clipped automatically, so zooming just reveals more or less of the coast.

The coast is fetched for the **range you're actually viewing** (plus a little
headroom) and **cached in flash (a LittleFS file on the `spiffs` partition)**, so
it's drawn instantly on boot. It only re-downloads when it's genuinely needed:
the first time ever, when you **zoom out past what's cached** (the cache then
*grows* to the new range — zooming back in reuses it), or when the **home
location** moves (> ~2 nm). On a normal reboot it reuses the saved copy and never
touches the network for the map. Inland with no coast nearby is cached too, and a
failed download backs off and retries about once a minute.

> If you see `partition "spiffs" could not be found` / `no LittleFS partition -
> coastline cached in RAM only` in the serial log, you flashed without this
> project's `partitions.csv` in the sketch folder — the cache still works but
> lives in RAM and re-downloads each boot. Re-flash with `partitions.csv` present
> (see §3.5) to make it persist.

You can optionally add your **own** fixed features (rivers, borders, labelled
airports/towns) by editing the static arrays in `landmarks.h`. Toggle the whole
overlay with `#define SHOW_LANDMARKS 1` in `config.h` (set the cache size with
`LM_MAX_PTS`). Inland with no coast nearby? The log shows `No coastline here` and
only the (optional) static extras are drawn.

The firmware version (set by `FW_VERSION` in `config.h`) is shown in the
top-left of the header, on the setup screen, on both web pages and in the serial
log. See [`CHANGELOG.md`](CHANGELOG.md) for the version history.

---

## 6. Updating firmware remotely (OTA)

Once the device is on your Wi-Fi you can flash new firmware over the air — no USB
cable needed. This requires the OTA partition scheme from step 3.5.

1. In the Arduino IDE, build a binary with *Sketch → **Export Compiled Binary***.
   The `.bin` appears in a `build/` sub-folder of the sketch (the file *without*
   `.bootloader`/`.partitions` in its name, e.g. `FlightRadar.ino.bin`).
2. Browse to the device: **http://flightradar.local/update** (or
   `http://<device-ip>/update`), or tap **CFG** and follow the *Update firmware*
   link at the bottom of the settings page.
3. Choose the `.bin` and tap **Upload & flash**. A progress bar runs, the screen
   shows **FIRMWARE UPDATE / Receiving**, then **Success**, and the device reboots
   into the new version. Bump `FW_VERSION` before each build so you can confirm it
   took.

> Don't power off during the upload. If it ends in **FAILED**, the old firmware is
> untouched — check the Serial Monitor for the reason (usually a wrong partition
> scheme or a truncated upload) and retry.

---

## 7. Troubleshooting

| Symptom | Fix |
|---------|-----|
| White/blank or no image | Almost always **PSRAM not enabled** — set *Tools → PSRAM → OPI PSRAM*. |
| Horizontal jitter/shear | The bounce buffer normally eliminates this. If it returns, raise `RGB_BOUNCE_PX` in `config.h` (e.g. `PANEL_W * 16`) or lower `RGB_PCLK_HZ` a little. |
| Colours wrong / red↔blue | Flip the `useBigEndian` flag (the `false` near the end of the `Arduino_ESP32RGBPanel(...)` constructor) in `board_7in.h`. |
| Image sideways/upside-down | Change `LCD_ROTATION` in `config.h` (use 0 or 2). Keep `TS_ROTATION` equal to it. |
| Touch hits the wrong spot / mirrored | Change `TS_ROTATION` in `config.h` to match the screen rotation. |
| Touch dead | Try `#define GT911_ADDR 0x14` (its other factory address) in `config.h`. Confirm the CH422G powered up (backlight on). |
| `HTTP 429` or no aircraft | You're being rate-limited or there's simply no traffic in range. Raise `REFRESH_MS`, increase range, or switch `API_HOST`. |
| Compile error about Arduino_GFX / LovyanGFX | You're missing a library — install **both** *GFX Library for Arduino* and *LovyanGFX* (per §3.4). |
| No serial output | Enable *Tools → USB CDC On Boot → Enabled*, then re-upload. |
| Won't connect to Wi-Fi | 2.4 GHz only (the ESP32 has no 5 GHz). It will fall back to the **FlightRadar-Setup** portal so you can re-enter credentials. |
| Setup page won't open | Join `FlightRadar-Setup` first, then browse to **http://192.168.4.1**. Disable mobile data so the phone uses the hotspot. |
| Want to wipe saved settings | Re-open the portal (CFG) and save new values, or use *Tools → Erase All Flash Contents* before uploading. |
| OTA upload fails / "Not enough space" | You're on a non-OTA partition. Set *Partition Scheme* to **Minimal SPIFFS (1.9MB APP with OTA)** (§3.5) and re-flash once over USB. |
| `flightradar.local` won't resolve | Some networks/phones block mDNS. Use the device's IP instead (shown on serial as `Config UI: http://...`). |
| No coastline on the map | Check the serial log: `No coastline here` = none in range (inland, cached so it won't retry); a negative `LANDMARKS HTTP ...` = Overpass busy/rate-limited (it retries about once a minute until it succeeds, then caches). Disable the overlay with `SHOW_LANDMARKS 0` in `config.h`. |

---

## 8. How it works

`FlightRadar.ino`:
1. Connects to Wi-Fi and syncs the clock over NTP.
2. Every `REFRESH_MS`, makes an HTTPS GET to
   `https://<API_HOST>/v2/point/<lat>/<lon>/<range_nm>` (ADSBExchange v2 JSON).
3. Streams the response through ArduinoJson with a **field filter**, so only the
   handful of fields used are kept in RAM.
4. Computes each aircraft's **distance** and **bearing** from your location with
   the haversine formula, sorts by distance.
5. Renders everything into an off-screen 16-bit sprite in PSRAM (flicker-free)
   and copies it once per frame into the Arduino_GFX framebuffer, which scans it
   out via a hardware bounce buffer, animating the sweep at ~30 fps.
6. Polls the touch controller for taps (aircraft selection / NEARBY list / RANGE /
   CFG). When you select a flight it makes a one-off HTTPS lookup to
   `https://api.adsbdb.com/v0/aircraft/<hex>` for the full aircraft type (the main
   ADS-B feed only carries the short ICAO type code).
7. Runs a small web server for configuration and **OTA firmware updates**, plus an
   mDNS responder so it's reachable at `flightradar.local`.

The code is split so the panel-specific parts are isolated: `FlightRadar.ino`
holds all the display-agnostic logic and `#include`s `board_7in.h`, which
provides a small contract — `displayBegin()`, `readTouch()` and the
drawing/`handleTouch` functions.

**Dual-core design.** Networking is the slow part (a TLS handshake + download +
JSON parse takes a second or two), so it runs in a background task pinned to
**core 0**, while the **display + touch render loop runs on core 1**. The two
share the aircraft/trail/type data through a small mutex that the network task
holds only for the sub-millisecond moment it swaps in a fresh snapshot. The
result: the radar sweep and touch stay smooth and responsive even while a fetch
is in flight (previously the screen froze briefly on every refresh).

Data is community-fed ADS-B and is provided free for **non-commercial** use —
please consider feeding data back to whichever network you use.

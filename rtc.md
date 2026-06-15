# RTC (DS3231) Setup & Time-Setting Guide

The clock keeps time with a **DS3231** real-time clock module instead of relying
on Wi-Fi/NTP. The DS3231 has a temperature-compensated crystal (±2 ppm) and a
coin-cell backup, so the time survives power loss and barely drifts. This
document covers wiring, how the time is set and stored, the timezone/DST model,
and troubleshooting.

---

## 1. Wiring

The Seeed XIAO ePaper Driver Board already claims the XIAO's default SPI pins
**and** GPIO4 / GPIO5 (silk **D2 / D3**) for the panel's BUSY and DC lines.
Those pins are **not** free for I²C, so the DS3231 is wired to the secondary I²C
pins **D4 / D5**:

| DS3231 pin | XIAO silk | XIAO GPIO | Notes |
|---|---|---|---|
| **SDA** | D4 | **GPIO6** | I²C data |
| **SCL** | D5 | **GPIO7** | I²C clock |
| **VCC** | 3V3 | — | 3.3 V (the DS3231 module also accepts 5 V if you have it) |
| **GND** | GND | — | common ground |

The relevant solder pads on the XIAO ESP32-C3 are shown here — SDA on **D4**,
SCL on **D5**, plus the **3V3** and **GND** pads. Note the callout: power the
DS3231 from **3V3**, not the 5 V pad two positions above it.

![DS3231 solder points on the XIAO ESP32-C3](assets/screenshots/rtc_solder_points.jpg)

In code this is established with:

```c
Wire.begin(6, 7);   // SDA=GPIO6 (D4), SCL=GPIO7 (D5)
rtc.begin();
```

> ⚠️ Do **not** move the RTC to D2/D3 (GPIO4/GPIO5) — those belong to the
> e-paper driver. Using them for I²C will break the display, the RTC, or both.

Insert a fresh **CR2032** coin cell so the DS3231 keeps time while the board is
unpowered.

---

## 2. How the time is set

There is no NTP and no hard-coded time. You set the clock from any phone or
laptop browser:

1. Power the board over USB-C.
2. Join the Wi-Fi network **`HebTime`** (open network, no password).
3. A **captive-portal** page opens automatically. If it doesn't, browse to
   **http://192.168.4.1**.
4. The page shows your device's live clock. Tap **Set Time**.
5. The browser POSTs its current Unix epoch to `/settime`; the firmware writes
   that epoch to the DS3231 and seeds the ESP32 soft-clock. The panel redraws
   with the new time within a second.

The access point stays up for the life of the session, so you can reconnect and
re-set the time whenever you like (e.g. after replacing the coin cell).

### Web endpoints

| Method | Path | Body | Purpose |
|---|---|---|---|
| `GET` | `/` | — | Captive-portal page |
| `POST` | `/settime` | `epoch=<unix_seconds>` | Set the RTC (UTC) |
| `POST` | `/datemode` | `mode=0` (Gregorian) / `mode=1` (Hebrew) | Top-row date mode (saved to flash) |

`/settime` rejects an epoch below `1000000000` (2001-09-09) as invalid.

---

## 3. Time storage & timezone model

**The DS3231 always stores UTC.** Local time is derived only for display:

- The browser sends `Math.floor(Date.now()/1000)` — an absolute UTC epoch — so
  what gets written to the RTC is independent of the phone's timezone.
- On the device, the POSIX timezone string is applied once at boot:

  ```c
  const char* timeZone = "IST-2IDT,M3.4.4/26,M10.5.0";  // Asia/Jerusalem
  setenv("TZ", timeZone, 1);
  tzset();
  ```

- `getLocalTime()` / `localtime_r()` then convert the UTC epoch to **Israel
  local time**, applying **DST automatically**:
  - **IST** = UTC**+2** (standard time)
  - **IDT** = UTC**+3** (daylight saving)
  - DST switches on the rule `M3.4.4/26,M10.5.0` (begins the 4th Friday of March
    at 02:00, ends the last Sunday of October).

Because the stored value is UTC and the offset is computed at display time, the
clock rolls forward/back for DST on its own — you never re-set it for the time
change.

### Soft-clock re-sync

The ESP32's internal clock is seeded from the RTC at boot and **re-synced from
the DS3231 every 60 seconds** in `loop()`, so any soft-clock drift between reads
is continuously corrected against the RTC.

---

## 4. First boot / lost-power behaviour

On boot the firmware checks `rtc.lostPower()`:

- **Power not lost** → the stored UTC is valid; the clock starts immediately.
- **Power lost** (first run, or dead/removed coin cell) → the time is unknown.
  The panel shows *"Connect to HebTime WiFi to set the time"* and waits. Set the
  time via the web UI (section 2) to clear it.

---

## 5. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Panel shows *"RTC not found - check wiring"* | I²C wiring/power | Verify SDA=D4/GPIO6, SCL=D5/GPIO7, VCC=3V3, GND; check solder joints. Serial logs `RTC not found` at boot. |
| Time resets / wrong after unplugging | Missing/dead coin cell | Insert a fresh CR2032; re-set the time once via the web UI. |
| Time is off by exactly 1 hour | DST boundary or wrong TZ | Confirm `timeZone` is `IST-2IDT,M3.4.4/26,M10.5.0`. A bad POSIX sign (e.g. `UTC-6` meaning UTC+6) causes a constant offset — see note below. |
| Time off by a whole timezone | RTC written with local instead of UTC | Re-set via the web UI; the browser sends UTC epoch, so just tapping **Set Time** fixes it. |
| Can't see the `HebTime` network | AP didn't start / radio | Check serial for `AP started SSID=HebTime`; power-cycle the board. |
| Captive portal doesn't pop | OS probe handling | Manually open **http://192.168.4.1**. |

### POSIX timezone sign gotcha

POSIX `TZ` offsets are **inverted** from what you'd expect: the string before
the DST name is *west-positive*. `IST-2` means **UTC+2**. Writing `UTC-6` would
mean **UTC+6**, not UTC−6 — a frequent source of "constant hour error" bugs.
For Israel always use:

```
IST-2IDT,M3.4.4/26,M10.5.0
```

### Verifying over serial

Open the Serial Monitor at **115200**. Useful lines:

- `RTC found  lostPower=<0|1>` — RTC detected and whether time is valid.
- `syncSystemTimeFromRTC: UTC epoch=<n>` — the raw UTC epoch read from the RTC.
- `applyTimeZone: TZ=IST-2IDT,...` — the active timezone string.
- `RTC adjusted — epoch=<n> local=HH:MM` — confirmation after a **Set Time**.

If the raw `epoch` advances correctly but the displayed hour is wrong, the issue
is in the timezone conversion, not the RTC.

---

## 6. Notes

- **No deep sleep.** The board stays awake so the AP is always reachable for a
  time reset. The RTC is consulted continuously; the coin cell is only the
  backup for when the board is fully unpowered.
- **Battery life.** A healthy CR2032 keeps the DS3231 running for years; the
  module draws only microamps from the backup cell.
- **Accuracy.** The DS3231 is temperature-compensated (±2 ppm ≈ ~1 min/year), so
  re-setting the time should rarely be necessary.

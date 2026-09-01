# litterEink



https://github.com/user-attachments/assets/5ce3907d-8147-40a7-92b5-2e27a177cc9c



Firmware for the LILYGO T5 4.7-inch e-paper V2.3 (`T5-ePaper-S3`). It displays
the litter-box state, records the last cleaning time, keeps a seven-day cleaner
history in ESP32 RTC memory and on SD card, maintains all-time cleaner totals,
schedules midnight refreshes and a 24-hour cleaning reminder through the
onboard PCF8563 RTC, and spends the rest of its time in deep sleep.

## PlatformIO

The checked-in PlatformIO environment configures the board's ESP32-S3, 16 MB
flash, 8 MB OPI PSRAM, native USB serial, and 960 x 540 e-paper driver. The
Arduino-ESP32 platform and LilyGo driver are pinned for reproducible builds.

Before flashing, set the Wi-Fi credentials and time zone in
`src/app_config.h`.

The firmware source is split by responsibility:

| Module | Responsibility |
| --- | --- |
| `main.cpp` | Wake handling, deep-sleep state, and application flow |
| `clock_service.*` | PCF8563 RTC, NTP sync, and midnight/reminder scheduling |
| `cleaning_history.*` | RTC history cache, SD CSV log, and all-time tally |
| `display_ui.*` | E-paper rendering, history panel, monthly pie chart, and battery warning |
| `app_config.h` | Pins, network settings, time zone, and application constants |

Build the firmware:

```sh
pio run
```

Connect the board's USB-C port and upload it:

```sh
pio run --target upload
```

Open the 115200-baud serial monitor:

```sh
pio device monitor
```

If automatic upload cannot enter the bootloader, hold **BOOT**, tap **RESET**,
release **BOOT**, and retry the upload. After flashing, tap **RESET** once to
run the firmware.

## Target pin use

| Function | GPIO |
| --- | ---: |
| PCF8563 SDA | 18 |
| PCF8563 SCL | 17 |
| PCF8563 alarm | 9 |
| Oriana clean button | 15 |
| Matthew clean button / built-in user button | 21 |
| Programming-mode button / e-paper CFG_STR | 0 |
| Battery ADC | 14 |
| SD MISO / MOSI / SCLK / CS | 16 / 15 / 11 / 42 |

Both clean buttons are active-low. GPIO 15 is shared with the SD card's MOSI
line. Before each SD session the firmware waits for that button to be released,
temporarily suspends its awake interrupt, and restores the pulled-up button
input afterward. A press held through the handoff is preserved, but pressing
Oriana during an active SD transfer can still corrupt the transfer because the
button and MOSI are physically the same signal. A short press entirely within
that transfer can also be missed; fully eliminating this limitation requires a
board-level pin remap or isolation between the button and MOSI.

Hold GPIO 0 while the firmware is preparing to sleep to enter programming
mode. The input must remain low for 100 ms; reset/boot transients are ignored.
The device will remain awake until its next reset so there is time to release
the button and upload new firmware. GPIO 0 is shared with the e-paper
controller, so the firmware returns it to input mode after each display update
before reading the button.

The right side of the display shows `Today` and the previous six calendar days.
At midnight the rows advance, the previous `Today` row receives its weekday
label, and a blank `Today` row appears at the top. `M` means Matthew, `O` means
Oriana, and `MO` means both people recorded a cleaning that day.

The seven-day window is cached with a checksum in ESP32 RTC slow memory for fast
deep-sleep continuity and reconstructed from the SD log after a power loss or
cold reset. When the SD card is available, each cleaning is appended to
`/cleanings.csv`:

```csv
timestamp,who
2026-08-20T18:15:00-0700,Oriana
```

All-time totals are cached in `/cleanings.tally`:

```csv
oriana,matthew,csv_size
210,311,24387
```

The CSV byte size is used as a lightweight tally cache key. If the size changes
without a matching tally update, the firmware rebuilds the totals from the
complete log. Normal wakeups load the tally and scan backward only through the
recent rows needed for the seven-day display. On the first day of each month,
the lower-left of the display shows the all-time Matthew/Oriana pie chart.

If an SD mount or write fails, the cleaning remains in the RTC seven-day view
across deep sleep, but it is not queued for a later CSV retry. A cold reset or
power loss can therefore lose that unpersisted event from the history and
all-time tally.

Before deep sleep, the firmware flushes and closes files, unmounts the card,
stops SPI, holds SD clock GPIO 11 low through the RTC I/O domain, and relies on
the board's physical pull-up to keep chip select high. The SD card shares the
ESP32's 3.3 V rail, so it remains powered and settles to its card-specific idle
current rather than being electrically switched off.

The PCF8563 exposes one calendar alarm, so the firmware keeps two logical
deadlines: the next local midnight and, after a cleaning, its 24-hour reminder.
It arms whichever comes first, handles every event due at that wake, then arms
the next deadline. This lets midnight refresh the history without prematurely
changing a clean display to dirty. The hardware clock and alarm run in UTC;
local-time conversion happens in firmware so daylight-saving transitions do
not shift either deadline.

After upgrading from the older local-time RTC firmware, let the first NTP sync
succeed so the PCF8563 is rewritten in UTC. If that sync fails, the old value
can appear offset until a later sync succeeds.

If the external clock reports an invalid time and NTP is unavailable, the
ESP32 adds an independent one-hour recovery wake instead of sleeping forever
without a calendar alarm. Cleaner-button wakes remain active during recovery.

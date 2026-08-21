# litterEink

Firmware for the LILYGO T5 4.7-inch e-paper V2.3 (`T5-ePaper-S3`). It displays
the litter-box state, records the last cleaning time in the onboard PCF8563
RTC, schedules a reminder alarm, keeps a seven-day cleaner history on screen,
logs cleaning events to the onboard SD card, and spends the rest of its time in
deep sleep.

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
| `clock_service.*` | PCF8563 RTC, NTP sync, and reminder alarms |
| `cleaning_history.*` | Cleaner identities, SD-card CSV storage, and seven-day summaries |
| `display_ui.*` | E-paper rendering, cleaner history panel, and battery warning |
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

Both clean buttons are active-low. GPIO 15 is shared with the onboard SD card's
MOSI line: the firmware uses it as a wake input during deep sleep, waits for the
Oriana button to be released, then temporarily assigns it to the SD card. Do not
hold the Oriana button down while the device is awake.

Hold GPIO 0 while the firmware is preparing to sleep to enter programming
mode. The input must remain low for 100 ms; reset/boot transients are ignored.
The device will remain awake until its next reset so there is time to release
the button and upload new firmware. GPIO 0 is shared with the e-paper
controller, so the firmware returns it to input mode after each display update
before reading the button.

Each cleaning is appended to `/cleanings.csv` on the SD card. The file is
created with this schema:

```csv
timestamp,who
2026-08-20T18:15:00-0700,Oriana
```

All-time totals are cached in `/cleanings.tally`:

```csv
oriana,matthew,csv_size
210,311,24387
```

The CSV size validates the cache. If the CSV changes without a matching tally
update, the firmware rebuilds the tally once from the complete log. Normal
wakeups read the tally and scan backward only through the recent CSV rows
needed for the seven-day display.

The right side of the display shows today and the previous six calendar days.
`M` means Matthew, `O` means Oriana, and `MO` means both people recorded a
cleaning that day. The all-time cleaning pie chart appears in the lower-left
only on the first day of each month.

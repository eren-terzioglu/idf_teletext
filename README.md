# Teletext viewer — M5Stack Tab5

Viewer for ČT teletext. The Tab5 downloads every page itself over Wi-Fi, shows it with a
touch keypad and keeps the last download in flash for the next boot. No PC is needed.

## Screen (1280×720 landscape)

```
┌──────────────────────────────────┬───────────────────┐
│                                  │ 100   3/7     12- │  page · subpage · typed digits
│   teletext picture               │  |◀   ◀   ▶   ▶|  │  page −/+ · subpage −/+
│   312×276 scaled 2.5× = 780×690  │   1    2    3     │
│                                  │   4    5    6     │
│                                  │   7    8    9     │
│                                  │  Fav   0   ⌫      │
└──────────────────────────────────┴───────────────────┘
```

- Type three digits to jump to that page; the jump happens on the third digit
  (there is no Enter key). ⌫ removes the last typed digit.
- Swipe on the picture: left/right = next/previous subpage, up/down = next/previous page.
- A page with no picture shows a blue test card and a "not available" note.
- A note at the top right of the picture shows Wi-Fi and download status. Every note,
  errors included, disappears after 10 seconds, so the viewer works normally offline.
- `TELETEXT_SCALE_X10` in `main/teletext_ui.c` sets the scale (25 = 2.5×, 20 = pixel-exact 2×).

## How the pages get on the device

1. **Boot:** the last saved page set is loaded from flash and shown right away.
2. **Download:** the Tab5 connects to Wi-Fi and fetches the index from
   `https://api-teletext.ceskatelevize.cz/pages` and every picture as WebP (~800 pictures,
   ~1.7 MB, about 1.5 minutes).
3. **Show and save:** when at least 95 % of the pictures arrived, the new set replaces the
   shown one in one step (you stay on the page you are reading) and is saved to flash.
   A picture that failed to download keeps its previous version.
4. **Repeat** every 30 minutes.

If Wi-Fi or the download fails, the saved set stays on screen. On the very first boot
(nothing saved yet) the screen shows "No pages yet" until the first download finishes.

When Wi-Fi does not connect (router off, wrong password, out of range), the Tab5 retries
every 5 seconds. After 15 failed attempts in a row (roughly 1.5–2 minutes) it shows
"Wi-Fi connection failed" once, for 10 seconds; after that it keeps retrying without
showing anything until it connects. A later outage shows the message once again.

The saved set is one file, `teletext.bin`, in the SPIFFS `storage` partition (catalog +
WebP pictures, CRC-checked). It is written as `teletext.new` first and renamed afterwards,
so a power cut while saving never leaves a broken copy. On the first boot after updating
from the old PC-based firmware, the old PNG files in that partition are deleted.

## Layout of the project

| Path | What it is |
|---|---|
| `main/main.c` | Start-up: NVS, display, UI, background task |
| `main/teletext_online.c` | Background task: saved set → Wi-Fi → download → show → save |
| `main/teletext_wifi.c` | Wi-Fi through the ESP32-C6 (esp_hosted over SDIO) |
| `main/teletext_cache.c` | Saving/loading the page set in SPIFFS |
| `main/teletext_store.c` | Picture set in PSRAM; WebP decode + nearest-neighbour scale to RGB565 |
| `main/teletext_pages.c` | Page catalog (page number → number of subpages) |
| `main/teletext_ui.c` | LVGL screen: picture, status line, keypad, swipes, status note |
| `main/tab5_display.c` | Tab5 panel detection (ILI9881C / ST7123 / ST7121) and display start |
| `main/Kconfig.projbuild` | "Teletext" settings (Wi-Fi, URL, refresh interval) |
| `components/libwebp/` | libwebp 1.5.0 decoder (portable C only) |
| `partitions.csv` | 3 MB app, 12 MB SPIFFS `storage` for the saved set |

## Wi-Fi settings

Put the credentials in a file `sdkconfig.defaults.local` next to `sdkconfig.defaults`
(it is in `.gitignore`):

```
CONFIG_TELETEXT_WIFI_SSID="my-network"
CONFIG_TELETEXT_WIFI_PASSWORD="my-password"
```

Then delete `sdkconfig` and build again. Alternatively use `idf.py menuconfig` →
"Teletext", which also has the refresh interval, the API URL and the minimum success
percentage. With an empty SSID only the saved pages are shown ("No Wi-Fi configured"
appears for 10 seconds at boot).

`sdkconfig.defaults.local` is the safe place for the credentials: values set only in
menuconfig live in `sdkconfig` and are lost whenever `sdkconfig` is deleted.

## Build and flash

Needs ESP-IDF 6.0 with the esp32p4 tools. Tested with the checkout in
`../esp-idf` (`v6.0.3-986-gae974f8270b`). In an ESP-IDF terminal (`export.bat` /
`export.ps1` first):

```bash
idf.py build
```

```bash
idf.py -p COM5 flash monitor
```

Flashing writes only the bootloader, partition table and app; the saved pages in the
`storage` partition survive. `idf.py erase-flash` removes them (the next boot formats the
partition and downloads again).

The board settings are in `sdkconfig.defaults`. If you change it, delete
`sdkconfig` so the defaults are applied again.

## ESP32-C6 firmware

The P4 has no radio; Wi-Fi goes through the Tab5's ESP32-C6 over SDIO with ESP-Hosted
(`CONFIG_ESP32P4_TAB5_C6_BOARD`). The boot log prints the C6 firmware version
(`teletext_wifi: ESP32-C6 esp_hosted firmware x.y.z`).

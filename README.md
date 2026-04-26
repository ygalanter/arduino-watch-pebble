# Circuit Clock

A Pebble watchface styled after an Arduino Nano with a 7-segment LED display.
The time is rendered as four lit digits over a dimmed `8.8.8.8.` shadow on a
PCB-textured background.

## Platform

- Pebble Time 2 (Emery, 200×228, 64-color)

## Features

- Four-digit time display in a 7-segment style, hours and minutes separated by
  a decimal point. Honors the system 12h/24h setting.
- Configurable digit color (default yellow). All standard Pebble colors are
  selectable from the watchface settings page.
- Shake-to-cycle activity readout shown in a framed band along the bottom:
  - Hidden (no band)
  - Date (e.g. `APRIL 26`)
  - Day of week (e.g. `SATURDAY`)
  - AM / PM / 24H
  - Steps
  - Calories
  - Distance (km or miles, follows the watch's measurement system)
  - Active time
  - Heart rate
  - Battery
- The chosen color and the currently selected activity persist across reboots.

## Building

Requires the Pebble SDK with the Emery platform installed.

```sh
pebble build
```

Produces `build/arduino-watch-pebble.pbw`.

## Installing

To the emulator:

```sh
pebble install --emulator emery
```

To a phone running the Pebble app on the same network:

```sh
pebble install --phone <phone-ip>
```

## Configuration

Open the watchface in the Pebble mobile app and tap **Settings** to pick the
digit color. Changes apply immediately.

## Project layout

```
src/c/main.c          watchface logic
src/pkjs/index.js     Clay bootstrap
src/pkjs/config.json  settings UI
resources/images/     background, board, digit, and dot bitmaps
package.json          app metadata, resource manifest
wscript               build rules
```

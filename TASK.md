# Basic HTML browser for DIY Lilka console

## Hardware
MCU: ESP32-S3 with 8 MB PSRAM
TFT: 280x240 ST7789
SD Card
SNES-like controls: D-pad, SELECT, START; Action buttons: A B C D
https://github.com/lilka-dev/lilka

## Environment
Keira/FreeRTOS with multiboot, capable of loading and running firmwares from OTA partition
Keira stores wi-fi credentials into NVS, check ../lilka-boombox
PlatformIO + Arduino, ESP-IDF
Apple dev tools for sketching and quick tests

## The idea
Very basic HTML browser for a memory-constrained device, similar to Opera Mini. 
Streams a web page, strips out all the JS and CSS on the fly, preserving only critical elements like
input, buttons, links and plain texts.

## Requirements
- HTTP and HTTPS
- basic things like HTTP redirect
- Big elements laid out vertically, large text blocks are shown in collapsed to 3 lines state by default
  (Press A to expand/collapse the current section)
- UP/DOWN buttons to scroll the page, LEFT/RIGHT act like PgUp/PgDown
- SELECT switch selection rect over clickable elements in the visible frame, START - click on the selected element
- Button B - go to previous page or close popups
- Button D - settings popup: font size, etc.
- Button C - Quick start popup: enter address or pick from few last history items listed


## Requirements (added)
- Link list mode: toggleable flat numbered list of all links on the page (Opera Mini style)
- gzip/deflate response decoding (Accept-Encoding: gzip)
- 2-column (key/value) tables rendered as aligned rows
- <select> dropdowns rendered as cycle-able blocks (A = next option)

## Nice to have
- page background color preserved if trivial to obtain
- jpg and png images cached to SD card if small enough
- minimal text styles like <b> or <i>
- <h>-kind elements rendered with slightly larger font


## Additional requirements
1. Investigate with few common sites like google.com which User-Agent (or other form of identification) 
   gives simpler HTML or less or no JS pages
2. Cyrillic font support, we nailed it in the past, check ../lilka-boombox
3. Start with developing the stream parser using this Mac as a test target
4. Copy on-screen keyboard/input implementation from the lilka-sdk
5. Search for suitable requirement improvement ideas, e.g. from Opera Mini

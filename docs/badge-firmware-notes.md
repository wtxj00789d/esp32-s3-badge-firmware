# Badge Firmware Notes

## RGB565 Rainbow Color Pitfall

Symptom: color wallpapers look like harsh rainbow/solarized images on the round GC9A01 LCD, while black, white, red, or simple test pages may look almost acceptable.

Root cause to check first: RGB565 byte order on the LCD transfer path. A normal RGB565 pixel value such as `0xF800` is stored little-endian in ESP32 memory as `00 F8`, but many SPI LCD controllers expect the high byte first on the wire. If the bytes are sent as-is, full-color images turn into rainbow noise.

Debug shortcut:

```text
If a BWP decodes correctly on PC but looks rainbow on the LCD:
1. Keep the BWP file format as normal RGB565 values.
2. Swap each uint16_t pixel byte order immediately before esp_lcd_panel_draw_bitmap().
3. Then check RGB/BGR element order only after byte order is fixed.
```

Do not “fix” this by changing the converter to emit a weird byte order. That makes the file format lie and moves a display-driver problem into every asset. The display driver should adapt normal RGB565 pixels to the panel's wire format.

For the Spotpear ESP32-S3 1.28 GC9A01 badge board, the working combination is:

```text
BWP payload: normal RGB565 values
LCD transfer: byte-swapped before draw_bitmap
GC9A01 color order: BGR / MADCTL 0x48
```

Related commits:

```text
2470a8f fix: correct lcd rgb565 byte order
77dcb94 fix: restore lcd bgr color order
```

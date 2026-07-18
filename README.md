# NerdOCTAXE-Gamma — 3.5" Big Screen Firmware Patch

> **Based on:** [shufps/ESP-Miner-NerdQAxePlus](https://github.com/shufps/ESP-Miner-NerdQAxePlus) — tag `V1.0.37.2-LTS`
>
> **Purpose:** Enables the 3.5" 480×320 ST7789 display on the NerdOCTAXE-Gamma to render the full UI correctly — proper orientation, full-screen fill, no gaps or distortion.

---

## The Problem

The NerdOCTAXE-Gamma ships with a **3.5" 480×320 ST7789 panel** connected via an 8-bit i80 parallel bus.  
The upstream firmware was written for the **T-Display S3** (2.8" 320×170), so on the Gamma:

| Symptom | Root cause |
|---------|-----------|
| Image in wrong orientation / upside down | MADCTL register wrong for this panel |
| UI fills only part of the screen | LVGL resolution hard-coded to 320×170 |
| Black gap at top or bottom | GRAM y-gap set to 35 (T-Display S3 value) |
| Pixel clock too low | 6.5 MHz default → artefacts at 480px wide |

---

## The Solution

### 1. Software pixel scaler in `lvglFlushCallback`

LVGL continues to render at **320×170** (the UI's native design resolution).  
The display driver's flush callback **nearest-neighbour scales every tile 1.5×** before sending to the LCD:

- Scaled image: **480×255 px**
- Centred vertically in the 480×320 panel with **32 px black borders** top and bottom
- LCD is black-filled at boot so borders are clean

This approach avoids any LVGL layout changes and works with any future upstream merge.

### 2. MADCTL correction

The Gamma panel needs `swap_xy=true` + `mirror(false, false)` → **MADCTL 0x20** (landscape, correct orientation).  
The upstream firmware used MADCTL 0x60 which is correct for the T-Display S3 but rotated on the Gamma.

### 3. Board virtual method overrides (`NerdOctaxeGamma`)

All display parameters are now virtual methods on `Board`, overridden per-board:

| Method | T-Display S3 default | NerdOCTAXE-Gamma |
|--------|----------------------|-----------------|
| `getLCDWidth()` | 320 | **480** |
| `getLCDHeight()` | 170 | **320** |
| `getLCDYGap()` | 35 | **0** |
| `getLCDPixelClockHz()` | 6 528 000 | **12 288 000** (~12.3 MHz) |
| `isFlipScreenEnabled()` | NVS value | **inverted** (panel mounted 180°) |

---

## Files Changed

> 📂 **[See full diff vs V1.0.37.2-LTS](https://github.com/ziomik/ESP-Miner-NerdQAxePlus-Gamma-BigScreen/compare/V1.0.37.2-LTS...nerdoctaxe-gamma-3.5inch-display)**  
> 📝 **[Commit e896051 — all 5 modified files](https://github.com/ziomik/ESP-Miner-NerdQAxePlus-Gamma-BigScreen/commit/e896051)**

| File | Change | Direct link |
|------|--------|-------------|
| `CMakeLists.txt` | `PROJECT_VER` set to `"V1.0.37.2-LTS-BIG"` | [view](https://github.com/ziomik/ESP-Miner-NerdQAxePlus-Gamma-BigScreen/blob/nerdoctaxe-gamma-3.5inch-display/CMakeLists.txt) |
| `main/boards/board.h` | Virtual LCD method declarations + defaults | [view](https://github.com/ziomik/ESP-Miner-NerdQAxePlus-Gamma-BigScreen/blob/nerdoctaxe-gamma-3.5inch-display/main/boards/board.h) |
| `main/boards/nerdoctaxegamma.h` | `getLCDWidth/Height/YGap/PixelClock` overrides | [view](https://github.com/ziomik/ESP-Miner-NerdQAxePlus-Gamma-BigScreen/blob/nerdoctaxe-gamma-3.5inch-display/main/boards/nerdoctaxegamma.h) |
| `main/boards/nerdoctaxegamma.cpp` | `isFlipScreenEnabled()` — inverted for 180° mount | [view](https://github.com/ziomik/ESP-Miner-NerdQAxePlus-Gamma-BigScreen/blob/nerdoctaxe-gamma-3.5inch-display/main/boards/nerdoctaxegamma.cpp) |
| `main/displays/displayDriver.cpp` | Pixel scaler + `lvglFlushCallback` rewrite + board-aware `initTDisplayS3()` | [view](https://github.com/ziomik/ESP-Miner-NerdQAxePlus-Gamma-BigScreen/blob/nerdoctaxe-gamma-3.5inch-display/main/displays/displayDriver.cpp) |

---

## How to Build

### Requirements

- Docker
- The upstream repo cloned at tag `V1.0.37.2-LTS`

```bash
git clone https://github.com/shufps/ESP-Miner-NerdQAxePlus.git
cd ESP-Miner-NerdQAxePlus
git checkout V1.0.37.2-LTS
```

### Apply this patch

Either use this fork directly, or apply the patch file:

```bash
# Option A — use this fork
git clone https://github.com/ziomik/ESP-Miner-NerdQAxePlus-Gamma-BigScreen.git
cd ESP-Miner-NerdQAxePlus-Gamma-BigScreen
git checkout nerdoctaxe-gamma-3.5inch-display

# Option B — apply patch to upstream
cd ESP-Miner-NerdQAxePlus
git apply nerdoctaxe-gamma-3.5inch.patch
```

### Build

```bash
docker run --rm -e BOARD=NERDOCTAXEGAMMA \
  -v $(pwd):/home/builder/project \
  -w /home/builder/project \
  shufps/esp-idf-builder:0.0.1 idf.py build
```

Output binary: `build/esp-miner.bin`

> **Note:** If the build fails on the npm step with permission errors, run:
> ```bash
> sudo rm -rf main/http_server/axe-os/node_modules
> sudo rm -rf main/http_server/axe-os/dist
> # then re-run the docker build command above
> ```

---

## How to Flash

### Via web browser (easiest — USB-C)

Use [ESP Web Tools](https://esp.huhn.me/) or the official ESPTool web flasher.

Flash `build/esp-miner.bin` at offset `0x10000`.

### Via esptool (command line)

```bash
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/esp-miner.bin \
  0x410000 build/www.bin \
  0xf10000 build/ota_data_initial.bin
```

### Full flash from build directory

```bash
cd build
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash "@flash_args"
```

---

## Technical Details

### Pixel scaler design

```
LVGL renders tile (x1,y1)→(x2,y2) in 320×170 space
         │
         ▼
lvglFlushCallback():
  dst_x = src_x * 480 / 320   (isotropic 1.5× scale)
  dst_y = src_y * 480 / 320 + 32   (+ 32px vertical centring offset)
         │
         ├── x-LUT precomputed per tile (avoids divide in inner loop)
         │
         └── nearest-neighbour fill → s_scale_buf (DMA-capable PSRAM)
                   │
                   ▼
         esp_lcd_panel_draw_bitmap(dst_x1, dst_y1, dst_x2+1, dst_y2+1, s_scale_buf)
```

- T-Display S3 boards: `s_scale_active = false` → zero overhead, direct pass-through
- Scale factor is isotropic (same X and Y) to preserve aspect ratio

### MADCTL values

| Board | `swap_xy` | `mirror(x,y)` | MADCTL byte | Result |
|-------|-----------|---------------|-------------|--------|
| T-Display S3 (default) | true | (true, false) | 0x60 | Correct landscape |
| NerdOCTAXE-Gamma | true | (false, false) | 0x20 | Correct landscape (panel 180° rotated) |

### Memory usage

| Buffer | Size | Location |
|--------|------|----------|
| LVGL draw buffer | 9 066 px × 2 B = ~18 KB | DMA-capable RAM |
| Scale output buffer | ~25 604 px × 2 B = ~51 KB | DMA-capable RAM |
| LCD black-fill (boot, freed) | 16 × 480 × 2 B = 15 KB | DMA-capable RAM |

---

## Hardware Notes

- **Display controller:** ST7789V (or compatible), 8-bit i80 parallel interface
- **Panel size:** 3.5" 480×320
- **Flip screen setting:** Inverted relative to other boards (panel physically mounted 180°)
- **TMP451 mux:** Present on rev 3.4 (I2C addr 0x4C / 0x4E), auto-detected at boot
- **VR detect pin:** GPIO3 — HIGH = TPS53667, LOW = TPS53647

---

## Version String

The `Current Version` field in the web UI and API shows `V1.0.37.2-LTS-BIG` to distinguish this build from the standard `V1.0.37.2-LTS` release.

---

## Credits

- Original firmware: [shufps/ESP-Miner-NerdQAxePlus](https://github.com/shufps/ESP-Miner-NerdQAxePlus)
- Pixel scaler patch: [@ziomik](https://github.com/ziomik) / Satoshi AI
- Tested on NerdOCTAXE-Gamma hardware (rev 3.1+)

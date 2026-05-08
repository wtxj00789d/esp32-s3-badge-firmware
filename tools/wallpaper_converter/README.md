# Xiaozhi Wallpaper Converter

Converts media into `.gifp` files for the Waveshare ESP32-S3-Touch-LCD-1.46 firmware.

Use `--size 412` for TF-card wallpapers and `--size 64` for built-in default UI animations.
The GUI includes animated preview, square crop selection, drag-to-move, mouse-wheel zoom, progress, and logs.
For 412px wallpapers, omitting `--max-size` uses a realtime playback profile that favors indexed compression instead of full-color files that decode too slowly on the ESP32-S3.
Use `--rgb565` when color fidelity matters more than pixel-perfect resolution. A 256px or 320px RGB565 wallpaper is often closer to the source than a 412px indexed wallpaper.
Transparent GIF/PNG input keeps binary transparency through a GIFP transparent color key.

```powershell
python tools\wallpaper_converter\app.py input.gif --output output.gifp --fps 20 --size 256 --rgb565
python tools\wallpaper_converter\app.py input.mp4 --output output.gifp --fps 15 --size 412 --crop 0,0,720,720
python tools\wallpaper_converter\app.py defaultUI\idle.gif --output main\boards\waveshare\esp32-s3-touch-lcd-1.46\default_ui\idle.gifp --fps 25 --size 64
```

Run without arguments to open the GUI:

```powershell
python tools\wallpaper_converter\app.py
```

Packaged Windows build:

```powershell
tools\wallpaper_converter\dist\XiaozhiWallpaperConverter.exe
tools\wallpaper_converter\dist\XiaozhiWallpaperConverter.exe input.gif --output output.gifp --fps 20 --size 256 --rgb565
```

Bundled ffmpeg is expected beside the exe:

```text
tools\wallpaper_converter\dist\ffmpeg\bin\ffmpeg.exe
```

Rebuild the single-file GUI exe:

```powershell
python -m PyInstaller --onefile --windowed --name XiaozhiWallpaperConverter --distpath tools\wallpaper_converter\dist --workpath tools\wallpaper_converter\pyinstaller_build --specpath tools\wallpaper_converter tools\wallpaper_converter\app.py
```

from __future__ import annotations

import argparse
import sys
import threading
from pathlib import Path
from shutil import which
from tkinter import BOTH, DISABLED, DoubleVar, END, LEFT, NORMAL, RIGHT, X, Canvas, IntVar, StringVar, Tk, filedialog, messagebox
from tkinter import ttk

from PIL import Image, ImageTk

import gifpack


FPS_CHOICES = (10, 15, 18, 20, 25)
SMOOTH_REAL_FPS = 6
SIZE_CHOICES = (412, 360, 320, 288, 256, 206, 64)
MEDIA_FILETYPES = [
    ("Media files", "*.gif *.mp4 *.mov *.m4v *.webm *.mkv *.png *.jpg *.jpeg *.bmp *.webp"),
    ("GIF files", "*.gif"),
    ("Image files", "*.png *.jpg *.jpeg *.bmp *.webp"),
    ("Video files", "*.mp4 *.mov *.m4v *.webm *.mkv"),
    ("All files", "*.*"),
]

if getattr(sys, "frozen", False):
    APP_DIR = Path(sys.executable).resolve().parent
else:
    APP_DIR = Path(__file__).resolve().parent
BUILD_DIR = APP_DIR / "build"


def resolve_ffmpeg() -> str | None:
    candidates = [
        APP_DIR / "ffmpeg" / "bin" / "ffmpeg.exe",
        APP_DIR / "dist" / "ffmpeg" / "bin" / "ffmpeg.exe",
        Path(r"M:\ffmpeg-8.0.1-essentials_build\bin\ffmpeg.exe"),
        Path(r"H:\download\Programs\PyAnime4K\ffmpeg\ffmpeg.exe"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return str(candidate)
    return which("ffmpeg")


def convert_file(
    input_path,
    output_path=None,
    fps=15,
    width=412,
    height=412,
    max_duration_ms=None,
    crop=None,
    max_size=None,
    smooth=False,
    smooth_real_fps=SMOOTH_REAL_FPS,
    force_rgb565=False,
    pre_sharpen=False,
    progress=None,
):
    source = Path(input_path)
    if not source.exists():
        raise FileNotFoundError(source)
    target = Path(output_path) if output_path else source.with_suffix(".gifp")
    target.parent.mkdir(parents=True, exist_ok=True)
    duration = max_duration_ms if max_duration_ms is not None else gifpack.default_max_duration_ms(fps)
    blob = gifpack.convert_media_to_gifpack(
        str(source),
        crop=crop,
        max_size=max_size,
        fps_cap=fps,
        max_duration_ms=duration,
        width=width,
        height=height,
        ffmpeg_path=resolve_ffmpeg(),
        smooth=smooth,
        smooth_real_fps=smooth_real_fps,
        force_rgb565=force_rgb565,
        pre_sharpen=pre_sharpen,
        progress=progress,
    )
    target.write_bytes(blob)
    return target


class ConverterApp:
    def __init__(self, root: Tk):
        self.root = root
        self.root.title("Xiaozhi GIFP Converter")
        self.root.geometry("860x720")

        self.media_path: Path | None = None
        self.media_frames: list[tuple[Image.Image, int]] = []
        self.preview_image: Image.Image | None = None
        self.preview_photo: ImageTk.PhotoImage | None = None
        self.preview_index = 0
        self.preview_job: str | None = None
        self.image_rect = (0, 0, 0, 0)
        self.crop = (0.0, 0.0, 1.0, 1.0)
        self.drag_last: tuple[int, int] | None = None

        self.file_var = StringVar(value="No media selected")
        self.output_var = StringVar(value=str(BUILD_DIR / "animation.gifp"))
        self.fps_var = IntVar(value=20)
        self.size_var = IntVar(value=256)
        self.smooth_var = IntVar(value=0)
        self.rgb565_var = IntVar(value=1)
        self.pre_sharpen_var = IntVar(value=0)
        self.duration_var = StringVar()
        self.status_var = StringVar(value="Ready")
        self.progress_var = DoubleVar(value=0.0)
        self.busy = False

        self._build_ui()
        self.update_duration_label()

    def _build_ui(self) -> None:
        top = ttk.Frame(self.root, padding=10)
        top.pack(fill=X)

        self.choose_button = ttk.Button(top, text="Choose Media", command=self.choose_media)
        self.choose_button.pack(side=LEFT)
        ttk.Label(top, textvariable=self.file_var).pack(side=LEFT, padx=10, fill=X, expand=True)

        self.canvas = Canvas(self.root, width=620, height=430, background="#202020", highlightthickness=0)
        self.canvas.pack(fill=BOTH, expand=True, padx=10, pady=(0, 10))
        self.canvas.bind("<ButtonPress-1>", self.on_mouse_down)
        self.canvas.bind("<B1-Motion>", self.on_mouse_drag)
        self.canvas.bind("<ButtonRelease-1>", self.on_mouse_up)
        self.canvas.bind("<MouseWheel>", self.on_mouse_wheel)
        self.canvas.bind("<Configure>", lambda _event: self.redraw_preview())

        options = ttk.Frame(self.root, padding=(10, 0, 10, 8))
        options.pack(fill=X)

        ttk.Label(options, text="FPS:").pack(side=LEFT)
        self.fps_combo = ttk.Combobox(options, textvariable=self.fps_var, values=FPS_CHOICES, width=5, state="readonly")
        self.fps_combo.pack(side=LEFT, padx=(4, 12))
        self.fps_combo.bind("<<ComboboxSelected>>", lambda _event: self.on_options_changed())

        ttk.Label(options, text="Size:").pack(side=LEFT)
        self.size_combo = ttk.Combobox(options, textvariable=self.size_var, values=SIZE_CHOICES, width=6, state="readonly")
        self.size_combo.pack(side=LEFT, padx=(4, 12))
        self.size_combo.bind("<<ComboboxSelected>>", lambda _event: self.on_options_changed())

        self.smooth_check = ttk.Checkbutton(options, text="Smooth", variable=self.smooth_var, command=self.on_options_changed)
        self.smooth_check.pack(side=LEFT, padx=(0, 12))

        self.rgb565_check = ttk.Checkbutton(options, text="Full color", variable=self.rgb565_var, command=self.on_options_changed)
        self.rgb565_check.pack(side=LEFT, padx=(0, 12))

        self.pre_sharpen_check = ttk.Checkbutton(options, text="Pre-sharpen", variable=self.pre_sharpen_var, command=self.on_options_changed)
        self.pre_sharpen_check.pack(side=LEFT, padx=(0, 12))

        ttk.Label(options, textvariable=self.duration_var).pack(side=LEFT)

        self.convert_button = ttk.Button(options, text="Convert", command=self.start_conversion)
        self.convert_button.pack(side=RIGHT)

        output = ttk.Frame(self.root, padding=(10, 0, 10, 8))
        output.pack(fill=X)
        ttk.Label(output, text="Output:").pack(side=LEFT)
        ttk.Entry(output, textvariable=self.output_var).pack(side=LEFT, fill=X, expand=True, padx=8)
        self.output_button = ttk.Button(output, text="Save As", command=self.choose_output)
        self.output_button.pack(side=RIGHT)

        progress_frame = ttk.Frame(self.root, padding=(10, 0, 10, 6))
        progress_frame.pack(fill=X)
        self.progress_bar = ttk.Progressbar(progress_frame, variable=self.progress_var, maximum=100)
        self.progress_bar.pack(fill=X)

        ttk.Label(self.root, textvariable=self.status_var, padding=(10, 0, 10, 4)).pack(fill=X)

        log_frame = ttk.Frame(self.root, padding=(10, 0, 10, 10))
        log_frame.pack(fill=BOTH)
        self.log_text = ttk.Treeview(log_frame, columns=("message",), show="headings", height=7)
        self.log_text.heading("message", text="Log")
        self.log_text.column("message", width=820, anchor="w")
        self.log_text.pack(fill=BOTH, expand=True)

    def log(self, message: str) -> None:
        self.status_var.set(message)
        self.log_text.insert("", END, values=(message,))
        rows = self.log_text.get_children()
        if rows:
            self.log_text.see(rows[-1])
        self.root.update_idletasks()

    def set_busy(self, busy: bool) -> None:
        self.busy = busy
        button_state = DISABLED if busy else NORMAL
        combo_state = DISABLED if busy else "readonly"
        for widget in (self.choose_button, self.output_button, self.convert_button, self.smooth_check, self.rgb565_check, self.pre_sharpen_check):
            widget.configure(state=button_state)
        for widget in (self.fps_combo, self.size_combo):
            widget.configure(state=combo_state)

    def selected_fps(self) -> int:
        try:
            fps = int(self.fps_var.get())
        except ValueError:
            fps = 15
        return fps if fps in FPS_CHOICES else 15

    def selected_size(self) -> int:
        try:
            size = int(self.size_var.get())
        except ValueError:
            size = 412
        return size if size in SIZE_CHOICES else 412

    def smooth_enabled(self) -> bool:
        return bool(self.smooth_var.get())

    def full_color_enabled(self) -> bool:
        return bool(self.rgb565_var.get())

    def pre_sharpen_enabled(self) -> bool:
        return bool(self.pre_sharpen_var.get())

    def update_duration_label(self) -> None:
        duration_fps = SMOOTH_REAL_FPS if self.smooth_enabled() else self.selected_fps()
        max_seconds = gifpack.default_max_duration_ms(duration_fps) // 1000
        if self.smooth_enabled():
            self.duration_var.set(f"Max duration: {max_seconds}s, keyframes: {SMOOTH_REAL_FPS}fps")
        else:
            self.duration_var.set(f"Max duration: {max_seconds}s")

    def on_options_changed(self) -> None:
        self.update_duration_label()
        if self.media_path is not None:
            self.load_media(str(self.media_path))

    def choose_media(self) -> None:
        if self.busy:
            return
        path = filedialog.askopenfilename(filetypes=MEDIA_FILETYPES)
        if path:
            self.load_media(path)

    def choose_output(self) -> None:
        path = filedialog.asksaveasfilename(defaultextension=".gifp", filetypes=[("GIFP files", "*.gifp"), ("All files", "*.*")])
        if path:
            self.output_var.set(path)

    def load_media(self, path: str) -> None:
        fps = self.selected_fps()
        max_duration_ms = gifpack.default_max_duration_ms(fps)
        try:
            frames = gifpack.load_media_frames(path, fps_cap=fps, max_duration_ms=max_duration_ms, ffmpeg_path=resolve_ffmpeg())
        except Exception as exc:
            messagebox.showerror("Media error", str(exc))
            self.log(f"Failed to load media: {exc}")
            return

        self.media_path = Path(path)
        self.media_frames = [(image.convert("RGB"), delay) for image, delay in frames]
        self.preview_index = 0
        self.preview_image = self.media_frames[0][0]
        self.file_var.set(str(self.media_path))
        self.output_var.set(str(BUILD_DIR / f"{self.media_path.stem}.gifp"))

        width, height = self.preview_image.size
        side = min(width, height)
        left = (width - side) / 2
        top = (height - side) / 2
        self.crop = (left, top, left + side, top + side)
        self.redraw_preview()
        self.start_preview()
        ffmpeg_note = "bundled/system ffmpeg ready" if resolve_ffmpeg() else "ffmpeg not found for video input"
        self.log(f"Loaded {self.media_path.name}: {width} x {height}, {len(frames)} preview frames, fps={fps}, {ffmpeg_note}")

    def start_preview(self) -> None:
        if self.preview_job is not None:
            self.root.after_cancel(self.preview_job)
            self.preview_job = None
        if len(self.media_frames) > 1:
            self.schedule_preview()

    def schedule_preview(self) -> None:
        if not self.media_frames:
            return
        delay = max(20, self.media_frames[self.preview_index][1])
        self.preview_job = self.root.after(delay, self.advance_preview)

    def advance_preview(self) -> None:
        if len(self.media_frames) <= 1:
            self.preview_job = None
            return
        self.preview_index = (self.preview_index + 1) % len(self.media_frames)
        self.preview_image = self.media_frames[self.preview_index][0]
        self.redraw_preview()
        self.schedule_preview()

    def image_to_canvas_rect(self, image: Image.Image) -> tuple[int, int, int, int]:
        canvas_w = max(1, self.canvas.winfo_width())
        canvas_h = max(1, self.canvas.winfo_height())
        scale = min(canvas_w / image.width, canvas_h / image.height)
        draw_w = max(1, int(image.width * scale))
        draw_h = max(1, int(image.height * scale))
        x0 = (canvas_w - draw_w) // 2
        y0 = (canvas_h - draw_h) // 2
        return x0, y0, x0 + draw_w, y0 + draw_h

    def canvas_crop_to_image_crop(self) -> tuple[int, int, int, int]:
        self.clamp_crop()
        if self.preview_image is None:
            return (0, 0, 1, 1)
        left_f, top_f, right_f, bottom_f = self.crop
        side = int(round(min(right_f - left_f, bottom_f - top_f)))
        side = max(1, min(side, self.preview_image.width, self.preview_image.height))
        left = min(max(0, int(round(left_f))), self.preview_image.width - side)
        top = min(max(0, int(round(top_f))), self.preview_image.height - side)
        return left, top, left + side, top + side

    def clamp_crop(self) -> None:
        if self.preview_image is None:
            return
        left, top, right, bottom = self.crop
        side = max(1.0, min(right - left, bottom - top, self.preview_image.width, self.preview_image.height))
        left = min(max(0.0, left), self.preview_image.width - side)
        top = min(max(0.0, top), self.preview_image.height - side)
        self.crop = (left, top, left + side, top + side)

    def redraw_preview(self) -> None:
        self.canvas.delete("all")
        if self.preview_image is None:
            self.canvas.create_text(
                self.canvas.winfo_width() // 2,
                self.canvas.winfo_height() // 2,
                text="Choose an image, GIF, or video",
                fill="#d8d8d8",
            )
            return

        self.image_rect = self.image_to_canvas_rect(self.preview_image)
        x0, y0, x1, y1 = self.image_rect
        draw_w = x1 - x0
        draw_h = y1 - y0
        resized = self.preview_image.resize((draw_w, draw_h), Image.Resampling.LANCZOS)
        self.preview_photo = ImageTk.PhotoImage(resized)
        self.canvas.create_image(x0, y0, image=self.preview_photo, anchor="nw")

        scale = draw_w / self.preview_image.width
        left, top, right, bottom = self.crop
        cx0 = x0 + left * scale
        cy0 = y0 + top * scale
        cx1 = x0 + right * scale
        cy1 = y0 + bottom * scale
        label = f"{self.selected_size()} x {self.selected_size()}"
        self.canvas.create_rectangle(cx0, cy0, cx1, cy1, outline="#00d4ff", width=3)
        self.canvas.create_text(cx0 + 8, cy0 + 14, text=label, fill="#ffffff", anchor="w")

    def _canvas_to_image_delta(self, dx: int, dy: int) -> tuple[float, float]:
        if self.preview_image is None:
            return 0.0, 0.0
        x0, _y0, x1, _y1 = self.image_rect
        scale = (x1 - x0) / self.preview_image.width
        return dx / scale, dy / scale

    def on_mouse_down(self, event) -> None:
        if self.preview_image is not None:
            self.drag_last = (event.x, event.y)

    def on_mouse_drag(self, event) -> None:
        if self.preview_image is None or self.drag_last is None:
            return
        last_x, last_y = self.drag_last
        dx, dy = self._canvas_to_image_delta(event.x - last_x, event.y - last_y)
        left, top, right, bottom = self.crop
        self.crop = (left + dx, top + dy, right + dx, bottom + dy)
        self.drag_last = (event.x, event.y)
        self.clamp_crop()
        self.redraw_preview()

    def on_mouse_up(self, _event) -> None:
        self.drag_last = None

    def on_mouse_wheel(self, event) -> None:
        if self.preview_image is None:
            return
        left, top, right, bottom = self.crop
        center_x = (left + right) / 2
        center_y = (top + bottom) / 2
        side = right - left
        factor = 0.9 if event.delta > 0 else 1.1
        side = max(10.0, min(side * factor, self.preview_image.width, self.preview_image.height))
        self.crop = (center_x - side / 2, center_y - side / 2, center_x + side / 2, center_y + side / 2)
        self.clamp_crop()
        self.redraw_preview()

    def update_progress(self, done: int, total: int, message: str) -> None:
        percent = 0.0 if total <= 0 else min(100.0, max(0.0, done * 100.0 / total))
        self.progress_var.set(percent)
        self.status_var.set(f"{message} {percent:.0f}%")

    def start_conversion(self) -> None:
        if self.busy:
            return
        if self.media_path is None:
            messagebox.showwarning("No media", "Choose an image, GIF, or video first.")
            return
        output = Path(self.output_var.get().strip() or (BUILD_DIR / f"{self.media_path.stem}.gifp"))
        crop = self.canvas_crop_to_image_crop()
        fps = self.selected_fps()
        size = self.selected_size()
        smooth = self.smooth_enabled()
        force_rgb565 = self.full_color_enabled()
        pre_sharpen = self.pre_sharpen_enabled()
        max_duration_ms = gifpack.default_max_duration_ms(SMOOTH_REAL_FPS if smooth else fps)

        self.progress_var.set(0)
        self.set_busy(True)
        self.log("Starting conversion")
        thread = threading.Thread(target=self._run_conversion, args=(self.media_path, output, crop, fps, size, max_duration_ms, smooth, force_rgb565, pre_sharpen), daemon=True)
        thread.start()

    def _run_conversion(self, media_path: Path, output: Path, crop: tuple[int, int, int, int], fps: int, size: int, max_duration_ms: int, smooth: bool, force_rgb565: bool, pre_sharpen: bool) -> None:
        try:
            def progress(done: int, total: int, message: str) -> None:
                self.root.after(0, self.update_progress, done, total, message)

            blob, frames = gifpack.convert_media_to_gifpack_with_details(
                str(media_path),
                crop=crop,
                max_size=None,
                fps_cap=fps,
                max_duration_ms=max_duration_ms,
                width=size,
                height=size,
                ffmpeg_path=resolve_ffmpeg(),
                progress=progress,
                smooth=smooth,
                smooth_real_fps=SMOOTH_REAL_FPS,
                force_rgb565=force_rgb565,
                pre_sharpen=pre_sharpen,
            )
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(blob)
        except Exception as exc:
            self.root.after(0, self._conversion_failed, str(exc))
            return
        self.root.after(0, self._conversion_done, output, len(blob), fps, max_duration_ms, crop, len(frames), smooth, force_rgb565, pre_sharpen)

    def _conversion_failed(self, error: str) -> None:
        self.set_busy(False)
        messagebox.showerror("Conversion error", error)
        self.log(f"Conversion failed: {error}")

    def _conversion_done(self, output: Path, size: int, fps: int, max_duration_ms: int, crop: tuple[int, int, int, int], frame_count: int, smooth: bool, force_rgb565: bool, pre_sharpen: bool) -> None:
        self.progress_var.set(100)
        encoding_summary = ""
        try:
            parsed = gifpack.parse_gifpack(output.read_bytes())
            encoding_summary = gifpack.summarize_encodings(parsed.frames)
        except Exception:
            pass
        mode = f"smooth {SMOOTH_REAL_FPS}->{fps}fps" if smooth else f"{fps}fps"
        if force_rgb565:
            mode += ", RGB565"
        if pre_sharpen:
            mode += ", pre-sharpen"
        message = f"Wrote {output} ({size} bytes), frames={frame_count}, mode={mode}, max={max_duration_ms // 1000}s, crop={crop}"
        if encoding_summary:
            message += f", encodings={encoding_summary}"
        self.log(message)
        self.set_busy(False)
        messagebox.showinfo("Conversion complete", f"Saved:\n{output}")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Convert media to Xiaozhi .gifp files.")
    parser.add_argument("input", nargs="?")
    parser.add_argument("--output", "-o")
    parser.add_argument("--fps", type=int, default=15, choices=FPS_CHOICES)
    parser.add_argument("--size", type=int, default=412, choices=SIZE_CHOICES)
    parser.add_argument("--max-duration-ms", type=int)
    parser.add_argument("--max-size", type=int, default=None, help="Optional output size limit in bytes. Omit to use the realtime playback profile for 412px wallpapers.")
    parser.add_argument("--crop", help="Square crop as left,top,right,bottom in source pixels.")
    parser.add_argument("--smooth", action="store_true", help="Store low-FPS keyframes and ask firmware to synthesize in-between frames.")
    parser.add_argument("--smooth-real-fps", type=int, default=SMOOTH_REAL_FPS, help="Keyframe FPS used with --smooth.")
    parser.add_argument("--rgb565", action="store_true", help="Prefer full-color RGB565 compression instead of 256-color indexed output.")
    parser.add_argument("--pre-sharpen", action="store_true", help="Apply light offline unsharp mask after high-quality resizing.")
    args = parser.parse_args(argv)

    if not args.input:
        root = Tk()
        ConverterApp(root)
        root.mainloop()
        return 0

    crop = None
    if args.crop:
        values = [int(part.strip()) for part in args.crop.split(",")]
        if len(values) != 4:
            raise ValueError("--crop expects left,top,right,bottom")
        crop = tuple(values)

    print(convert_file(args.input, args.output, args.fps, args.size, args.size, args.max_duration_ms, crop=crop, max_size=args.max_size, smooth=args.smooth, smooth_real_fps=args.smooth_real_fps, force_rgb565=args.rgb565, pre_sharpen=args.pre_sharpen))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

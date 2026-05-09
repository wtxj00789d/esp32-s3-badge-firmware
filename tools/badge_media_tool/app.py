from __future__ import annotations

import argparse
import threading
import sys
from pathlib import Path
from shutil import which
from tkinter import BOTH, DISABLED, DoubleVar, END, LEFT, NORMAL, RIGHT, X, Canvas, IntVar, StringVar, Tk, filedialog, messagebox
from tkinter import ttk

from PIL import Image, ImageTk

import audio
import bwp


FPS_CHOICES = (5, 10, 12, 15)
MAX_FRAME_CHOICES = (30, 45, 60, 72)
MEDIA_FILETYPES = [
    ("Media files", "*.gif *.png *.jpg *.jpeg *.bmp *.webp *.mp4 *.mov *.m4v *.webm *.mkv"),
    ("GIF files", "*.gif"),
    ("Image files", "*.png *.jpg *.jpeg *.bmp *.webp"),
    ("Video files", "*.mp4 *.mov *.m4v *.webm *.mkv"),
    ("All files", "*.*"),
]
AUDIO_FILETYPES = [
    ("Audio files", "*.mp3 *.wav *.flac *.ogg *.m4a *.aac"),
    ("MP3 files", "*.mp3"),
    ("WAV files", "*.wav"),
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


class BadgeMediaTool:
    def __init__(self, root: Tk):
        self.root = root
        self.root.title("Badge Media Tool")
        self.root.geometry("900x760")

        self.media_path: Path | None = None
        self.media_frames: list[tuple[Image.Image, int]] = []
        self.preview_image: Image.Image | None = None
        self.preview_photo: ImageTk.PhotoImage | None = None
        self.preview_index = 0
        self.preview_job: str | None = None
        self.image_rect = (0, 0, 0, 0)
        self.crop = (0.0, 0.0, 1.0, 1.0)
        self.drag_last: tuple[int, int] | None = None

        self.wallpaper_file_var = StringVar(value="No media selected")
        self.wallpaper_output_var = StringVar(value=str(BUILD_DIR / "001.BWP"))
        self.fps_var = IntVar(value=15)
        self.max_frames_var = IntVar(value=60)
        self.wallpaper_status_var = StringVar(value="Ready")
        self.wallpaper_progress_var = DoubleVar(value=0.0)

        self.audio_file_var = StringVar(value="No audio selected")
        self.audio_output_var = StringVar(value=str(BUILD_DIR / "001.WAV"))
        self.audio_status_var = StringVar(value="Ready")
        self.audio_progress_var = DoubleVar(value=0.0)

        self.busy = False
        self._build_ui()
        self.update_duration_label()

    def _build_ui(self) -> None:
        notebook = ttk.Notebook(self.root)
        notebook.pack(fill=BOTH, expand=True)
        self.wallpaper_tab = ttk.Frame(notebook)
        self.audio_tab = ttk.Frame(notebook)
        notebook.add(self.wallpaper_tab, text="GIF to BWP")
        notebook.add(self.audio_tab, text="Audio to WAV")
        self._build_wallpaper_tab()
        self._build_audio_tab()

    def _build_wallpaper_tab(self) -> None:
        top = ttk.Frame(self.wallpaper_tab, padding=10)
        top.pack(fill=X)
        self.choose_media_button = ttk.Button(top, text="Choose GIF / Media", command=self.choose_media)
        self.choose_media_button.pack(side=LEFT)
        ttk.Label(top, textvariable=self.wallpaper_file_var).pack(side=LEFT, padx=10, fill=X, expand=True)

        self.canvas = Canvas(self.wallpaper_tab, width=640, height=430, background="#202020", highlightthickness=0)
        self.canvas.pack(fill=BOTH, expand=True, padx=10, pady=(0, 10))
        self.canvas.bind("<ButtonPress-1>", self.on_mouse_down)
        self.canvas.bind("<B1-Motion>", self.on_mouse_drag)
        self.canvas.bind("<ButtonRelease-1>", self.on_mouse_up)
        self.canvas.bind("<MouseWheel>", self.on_mouse_wheel)
        self.canvas.bind("<Configure>", lambda _event: self.redraw_preview())

        options = ttk.Frame(self.wallpaper_tab, padding=(10, 0, 10, 8))
        options.pack(fill=X)
        ttk.Label(options, text="FPS:").pack(side=LEFT)
        self.fps_combo = ttk.Combobox(options, textvariable=self.fps_var, values=FPS_CHOICES, width=5, state="readonly")
        self.fps_combo.pack(side=LEFT, padx=(4, 12))
        self.fps_combo.bind("<<ComboboxSelected>>", lambda _event: self.update_duration_label())
        ttk.Label(options, text="Max frames:").pack(side=LEFT)
        self.max_frames_combo = ttk.Combobox(options, textvariable=self.max_frames_var, values=MAX_FRAME_CHOICES, width=6, state="readonly")
        self.max_frames_combo.pack(side=LEFT, padx=(4, 12))
        self.max_frames_combo.bind("<<ComboboxSelected>>", lambda _event: self.update_duration_label())
        self.duration_var = StringVar()
        ttk.Label(options, textvariable=self.duration_var).pack(side=LEFT)
        self.convert_wallpaper_button = ttk.Button(options, text="Convert BWP", command=self.start_wallpaper_conversion)
        self.convert_wallpaper_button.pack(side=RIGHT)

        output = ttk.Frame(self.wallpaper_tab, padding=(10, 0, 10, 8))
        output.pack(fill=X)
        ttk.Label(output, text="Output:").pack(side=LEFT)
        ttk.Entry(output, textvariable=self.wallpaper_output_var).pack(side=LEFT, fill=X, expand=True, padx=8)
        ttk.Button(output, text="Save As", command=self.choose_wallpaper_output).pack(side=RIGHT)

        progress_frame = ttk.Frame(self.wallpaper_tab, padding=(10, 0, 10, 6))
        progress_frame.pack(fill=X)
        ttk.Progressbar(progress_frame, variable=self.wallpaper_progress_var, maximum=100).pack(fill=X)
        ttk.Label(self.wallpaper_tab, textvariable=self.wallpaper_status_var, padding=(10, 0, 10, 4)).pack(fill=X)
        self.wallpaper_log = ttk.Treeview(self.wallpaper_tab, columns=("message",), show="headings", height=6)
        self.wallpaper_log.heading("message", text="Log")
        self.wallpaper_log.pack(fill=BOTH, expand=False, padx=10, pady=(0, 10))

    def _build_audio_tab(self) -> None:
        top = ttk.Frame(self.audio_tab, padding=10)
        top.pack(fill=X)
        self.choose_audio_button = ttk.Button(top, text="Choose Audio", command=self.choose_audio)
        self.choose_audio_button.pack(side=LEFT)
        ttk.Label(top, textvariable=self.audio_file_var).pack(side=LEFT, padx=10, fill=X, expand=True)

        output = ttk.Frame(self.audio_tab, padding=(10, 0, 10, 8))
        output.pack(fill=X)
        ttk.Label(output, text="Output:").pack(side=LEFT)
        ttk.Entry(output, textvariable=self.audio_output_var).pack(side=LEFT, fill=X, expand=True, padx=8)
        ttk.Button(output, text="Save As", command=self.choose_audio_output).pack(side=RIGHT)

        controls = ttk.Frame(self.audio_tab, padding=(10, 0, 10, 8))
        controls.pack(fill=X)
        ttk.Label(controls, text="Output format: 24000 Hz, mono, signed 16-bit PCM WAV").pack(side=LEFT)
        self.convert_audio_button = ttk.Button(controls, text="Convert WAV", command=self.start_audio_conversion)
        self.convert_audio_button.pack(side=RIGHT)

        progress_frame = ttk.Frame(self.audio_tab, padding=(10, 0, 10, 6))
        progress_frame.pack(fill=X)
        ttk.Progressbar(progress_frame, variable=self.audio_progress_var, maximum=100).pack(fill=X)
        ttk.Label(self.audio_tab, textvariable=self.audio_status_var, padding=(10, 0, 10, 4)).pack(fill=X)
        self.audio_log = ttk.Treeview(self.audio_tab, columns=("message",), show="headings", height=12)
        self.audio_log.heading("message", text="Log")
        self.audio_log.pack(fill=BOTH, expand=True, padx=10, pady=(0, 10))

    def log_wallpaper(self, message: str) -> None:
        self.wallpaper_status_var.set(message)
        self.wallpaper_log.insert("", END, values=(message,))
        rows = self.wallpaper_log.get_children()
        if rows:
            self.wallpaper_log.see(rows[-1])
        self.root.update_idletasks()

    def log_audio(self, message: str) -> None:
        self.audio_status_var.set(message)
        self.audio_log.insert("", END, values=(message,))
        rows = self.audio_log.get_children()
        if rows:
            self.audio_log.see(rows[-1])
        self.root.update_idletasks()

    def set_busy(self, busy: bool) -> None:
        self.busy = busy
        state = DISABLED if busy else NORMAL
        combo_state = DISABLED if busy else "readonly"
        for widget in (self.choose_media_button, self.convert_wallpaper_button, self.choose_audio_button, self.convert_audio_button):
            widget.configure(state=state)
        for widget in (self.fps_combo, self.max_frames_combo):
            widget.configure(state=combo_state)

    def selected_fps(self) -> int:
        try:
            fps = int(self.fps_var.get())
        except ValueError:
            fps = bwp.DEFAULT_FPS
        return fps if fps in FPS_CHOICES else bwp.DEFAULT_FPS

    def selected_max_frames(self) -> int:
        try:
            max_frames = int(self.max_frames_var.get())
        except ValueError:
            max_frames = bwp.DEFAULT_MAX_FRAMES
        return max_frames if max_frames in MAX_FRAME_CHOICES else bwp.DEFAULT_MAX_FRAMES

    def update_duration_label(self) -> None:
        duration = bwp.default_max_duration_ms(self.selected_fps(), self.selected_max_frames()) / 1000
        size_mb = (bwp.HEADER_STRUCT.size + self.selected_max_frames() * bwp.FRAME_BYTES) / (1024 * 1024)
        self.duration_var.set(f"Max duration: {duration:.1f}s, raw size <= {size_mb:.1f}MB")
        if self.media_path is not None:
            self.load_media(str(self.media_path))

    def choose_media(self) -> None:
        if self.busy:
            return
        path = filedialog.askopenfilename(filetypes=MEDIA_FILETYPES)
        if path:
            self.load_media(path)

    def choose_wallpaper_output(self) -> None:
        path = filedialog.asksaveasfilename(defaultextension=".BWP", filetypes=[("BWP files", "*.BWP"), ("All files", "*.*")])
        if path:
            self.wallpaper_output_var.set(path)

    def choose_audio(self) -> None:
        if self.busy:
            return
        path = filedialog.askopenfilename(filetypes=AUDIO_FILETYPES)
        if path:
            self.audio_file_var.set(path)
            source = Path(path)
            self.audio_output_var.set(str(BUILD_DIR / f"{source.stem}.WAV"))
            self.log_audio(f"Loaded {source.name}")

    def choose_audio_output(self) -> None:
        path = filedialog.asksaveasfilename(defaultextension=".WAV", filetypes=[("WAV files", "*.WAV"), ("All files", "*.*")])
        if path:
            self.audio_output_var.set(path)

    def load_media(self, path: str) -> None:
        fps = self.selected_fps()
        max_duration_ms = bwp.default_max_duration_ms(fps, self.selected_max_frames())
        try:
            frames = bwp.load_media_frames(path, fps=fps, max_duration_ms=max_duration_ms, ffmpeg_path=resolve_ffmpeg())
        except Exception as exc:
            messagebox.showerror("Media error", str(exc))
            self.log_wallpaper(f"Failed to load media: {exc}")
            return
        self.media_path = Path(path)
        self.media_frames = [(image.convert("RGB"), delay) for image, delay in frames]
        self.preview_index = 0
        self.preview_image = self.media_frames[0][0]
        self.wallpaper_file_var.set(str(self.media_path))
        self.wallpaper_output_var.set(str(BUILD_DIR / f"{self.media_path.stem}.BWP"))
        width, height = self.preview_image.size
        side = min(width, height)
        left = (width - side) / 2
        top = (height - side) / 2
        self.crop = (left, top, left + side, top + side)
        self.redraw_preview()
        self.start_preview()
        self.log_wallpaper(f"Loaded {self.media_path.name}: {width} x {height}, preview frames={len(frames)}, fps={fps}")

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
            self.canvas.create_text(self.canvas.winfo_width() // 2, self.canvas.winfo_height() // 2, text="Choose media", fill="#d8d8d8")
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
        self.canvas.create_rectangle(cx0, cy0, cx1, cy1, outline="#00d4ff", width=3)
        self.canvas.create_text(cx0 + 8, cy0 + 14, text="240 x 240", fill="#ffffff", anchor="w")

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

    def update_wallpaper_progress(self, done: int, total: int, message: str) -> None:
        percent = 0.0 if total <= 0 else min(100.0, max(0.0, done * 100.0 / total))
        self.wallpaper_progress_var.set(percent)
        self.wallpaper_status_var.set(f"{message} {percent:.0f}%")

    def start_wallpaper_conversion(self) -> None:
        if self.busy:
            return
        if self.media_path is None:
            messagebox.showwarning("No media", "Choose GIF/media first.")
            return
        output = Path(self.wallpaper_output_var.get().strip() or (BUILD_DIR / f"{self.media_path.stem}.BWP"))
        crop = self.canvas_crop_to_image_crop()
        fps = self.selected_fps()
        max_frames = self.selected_max_frames()
        self.wallpaper_progress_var.set(0)
        self.set_busy(True)
        self.log_wallpaper("Starting BWP conversion")
        threading.Thread(target=self._run_wallpaper_conversion, args=(self.media_path, output, crop, fps, max_frames), daemon=True).start()

    def _run_wallpaper_conversion(self, media_path: Path, output: Path, crop: tuple[int, int, int, int], fps: int, max_frames: int) -> None:
        try:
            def progress(done: int, total: int, message: str) -> None:
                self.root.after(0, self.update_wallpaper_progress, done, total, message)

            result = bwp.convert_media_to_bwp(str(media_path), str(output), fps=fps, max_frames=max_frames, crop=crop, ffmpeg_path=resolve_ffmpeg(), progress=progress)
            size = result.stat().st_size
        except Exception as exc:
            self.root.after(0, self._wallpaper_failed, str(exc))
            return
        self.root.after(0, self._wallpaper_done, result, size, fps, crop)

    def _wallpaper_failed(self, error: str) -> None:
        self.set_busy(False)
        messagebox.showerror("BWP conversion error", error)
        self.log_wallpaper(f"Conversion failed: {error}")

    def _wallpaper_done(self, output: Path, size: int, fps: int, crop: tuple[int, int, int, int]) -> None:
        self.wallpaper_progress_var.set(100)
        self.set_busy(False)
        self.log_wallpaper(f"Wrote {output} ({size} bytes), fps={fps}, crop={crop}")
        messagebox.showinfo("BWP conversion complete", f"Saved:\n{output}")

    def start_audio_conversion(self) -> None:
        if self.busy:
            return
        input_path = self.audio_file_var.get()
        if input_path == "No audio selected":
            messagebox.showwarning("No audio", "Choose audio first.")
            return
        output = Path(self.audio_output_var.get().strip() or (BUILD_DIR / "001.WAV"))
        self.audio_progress_var.set(10)
        self.set_busy(True)
        self.log_audio("Starting WAV conversion")
        threading.Thread(target=self._run_audio_conversion, args=(input_path, output), daemon=True).start()

    def _run_audio_conversion(self, input_path: str, output: Path) -> None:
        try:
            result = audio.convert_audio_to_badge_wav(input_path, str(output))
            size = result.stat().st_size
        except Exception as exc:
            self.root.after(0, self._audio_failed, str(exc))
            return
        self.root.after(0, self._audio_done, result, size)

    def _audio_failed(self, error: str) -> None:
        self.set_busy(False)
        messagebox.showerror("Audio conversion error", error)
        self.log_audio(f"Conversion failed: {error}")

    def _audio_done(self, output: Path, size: int) -> None:
        self.audio_progress_var.set(100)
        self.set_busy(False)
        self.log_audio(f"Wrote {output} ({size} bytes), 24000Hz mono s16 PCM")
        messagebox.showinfo("Audio conversion complete", f"Saved:\n{output}")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Convert badge wallpaper/audio media.")
    sub = parser.add_subparsers(dest="command")
    bwp_parser = sub.add_parser("bwp", help="Convert GIF/image/video to BWP")
    bwp_parser.add_argument("input")
    bwp_parser.add_argument("--output", "-o", required=True)
    bwp_parser.add_argument("--fps", type=int, default=bwp.DEFAULT_FPS, choices=FPS_CHOICES)
    bwp_parser.add_argument("--max-frames", type=int, default=bwp.DEFAULT_MAX_FRAMES)
    bwp_parser.add_argument("--crop", help="Square crop as left,top,right,bottom")
    wav_parser = sub.add_parser("wav", help="Convert audio to badge WAV")
    wav_parser.add_argument("input")
    wav_parser.add_argument("--output", "-o", required=True)
    args = parser.parse_args(argv)

    if args.command == "bwp":
        crop = None
        if args.crop:
            values = [int(part.strip()) for part in args.crop.split(",")]
            if len(values) != 4:
                raise ValueError("--crop expects left,top,right,bottom")
            crop = tuple(values)
        print(bwp.convert_media_to_bwp(args.input, args.output, fps=args.fps, max_frames=args.max_frames, crop=crop, ffmpeg_path=resolve_ffmpeg()))
        return 0
    if args.command == "wav":
        print(audio.convert_audio_to_badge_wav(args.input, args.output))
        return 0

    root = Tk()
    BadgeMediaTool(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

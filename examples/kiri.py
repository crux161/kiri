"""
kiri.py — ctypes binding for libkiri.

Usage:
    python kiri.py <input_file> <output_dir> [--playlist]

Loads libkiri from the same directory as this file, then alongside it looks
at standard system paths.
"""
from __future__ import annotations

import ctypes
import os
import sys
from ctypes import (
    CFUNCTYPE, POINTER, Structure,
    c_char, c_char_p, c_int, c_int64, c_void_p,
)


# --- Locate and load the shared library ------------------------------------
def _load_libkiri() -> ctypes.CDLL:
    if sys.platform == "darwin":
        names = ["libkiri.dylib"]
    elif sys.platform == "win32":
        names = ["kiri.dll", "libkiri.dll"]
    else:
        names = ["libkiri.so"]

    here = os.path.dirname(os.path.abspath(__file__))
    search = [here, os.path.dirname(here)]  # examples/ and repo root
    for name in names:
        for d in search:
            path = os.path.join(d, name)
            if os.path.exists(path):
                return ctypes.CDLL(path)
    # Fall back to loader search path
    for name in names:
        try:
            return ctypes.CDLL(name)
        except OSError:
            continue
    raise OSError(f"Could not find libkiri ({', '.join(names)})")


lib = _load_libkiri()


# --- Status codes (match kiri.h) -------------------------------------------
KIRI_OK               =   0
KIRI_ERR_INVALID_ARG  =  -1
KIRI_ERR_OPEN_INPUT   =  -2
KIRI_ERR_INTERRUPTED  = -10

KIRI_LOG_ERROR, KIRI_LOG_WARN, KIRI_LOG_INFO, KIRI_LOG_VERBOSE = range(4)


# --- Structs (layout must match kiri.h exactly) ----------------------------
class KiriInputInfo(Structure):
    _fields_ = [
        ("size_bytes",        c_int64),
        ("duration_us",       c_int64),
        ("nb_streams",        c_int),
        ("has_video",         c_int),
        ("video_width",       c_int),
        ("video_height",      c_int),
        ("format_long_name",  c_char * 128),
        ("video_codec_name",  c_char * 32),
    ]


# Callback types
KiriProgressFn = CFUNCTYPE(c_int, c_void_p, c_int, c_int, c_int64, c_int64)
KiriLogFn      = CFUNCTYPE(None, c_void_p, c_int, c_char_p)


class KiriSplitOptions(Structure):
    _fields_ = [
        ("input_path",           c_char_p),
        ("output_dir",           c_char_p),
        ("max_bytes",            c_int64),
        ("basename",             c_char_p),
        ("extension",            c_char_p),
        ("safety_margin_bytes",  c_int64),
        ("preallocate_bytes",    c_int64),
        ("make_playlist",        c_int),
        ("on_progress",          KiriProgressFn),
        ("on_log",               KiriLogFn),
        ("user_data",            c_void_p),
    ]


# --- Function signatures ---------------------------------------------------
lib.kiri_version_string.restype  = c_char_p
lib.kiri_strerror.argtypes       = [c_int]
lib.kiri_strerror.restype        = c_char_p
lib.kiri_fat32_max_bytes.restype = c_int64
lib.kiri_chunk_count.argtypes    = [c_int64, c_int64]
lib.kiri_chunk_count.restype     = c_int64
lib.kiri_probe.argtypes          = [c_char_p, POINTER(KiriInputInfo)]
lib.kiri_probe.restype           = c_int
lib.kiri_split.argtypes          = [POINTER(KiriSplitOptions)]
lib.kiri_split.restype           = c_int


# --- Pythonic wrappers -----------------------------------------------------
def version() -> str:
    return lib.kiri_version_string().decode()


def strerror(code: int) -> str:
    return lib.kiri_strerror(code).decode()


def probe(path: str) -> dict:
    info = KiriInputInfo()
    rc = lib.kiri_probe(path.encode(), ctypes.byref(info))
    if rc != KIRI_OK:
        raise RuntimeError(f"probe failed: {strerror(rc)}")
    return {
        "size_bytes":   info.size_bytes,
        "duration_us":  info.duration_us,
        "nb_streams":   info.nb_streams,
        "has_video":    bool(info.has_video),
        "video_width":  info.video_width,
        "video_height": info.video_height,
        "format":       info.format_long_name.decode(errors="replace"),
        "video_codec":  info.video_codec_name.decode(errors="replace"),
    }


def split(input_path: str,
          output_dir: str,
          max_bytes: int,
          make_playlist: bool = False,
          on_progress=None,
          on_log=None) -> None:
    """Split a file. on_progress(seg, total, done, total_bytes) returns True to abort."""
    os.makedirs(output_dir, exist_ok=True)

    # Wrap user callbacks. We MUST keep references alive for the duration of the call
    # or the JIT-compiled trampolines get GC'd mid-flight.
    def _progress_trampoline(user, seg, total, done, tot):
        if on_progress is None:
            return 0
        try:
            return 1 if on_progress(seg, total, done, tot) else 0
        except Exception:
            return 1

    def _log_trampoline(user, level, msg):
        if on_log is None:
            return
        try:
            on_log(level, msg.decode(errors="replace"))
        except Exception:
            pass

    progress_cb = KiriProgressFn(_progress_trampoline)
    log_cb      = KiriLogFn(_log_trampoline)

    opts = KiriSplitOptions(
        input_path        = input_path.encode(),
        output_dir        = output_dir.encode(),
        max_bytes         = max_bytes,
        basename          = None,
        extension         = None,
        safety_margin_bytes = 0,
        preallocate_bytes   = -1,
        make_playlist     = 1 if make_playlist else 0,
        on_progress       = progress_cb,
        on_log            = log_cb,
        user_data         = None,
    )

    rc = lib.kiri_split(ctypes.byref(opts))
    # Keep trampolines alive until split returns
    del progress_cb, log_cb
    if rc == KIRI_ERR_INTERRUPTED:
        raise KeyboardInterrupt("split interrupted")
    if rc != KIRI_OK:
        raise RuntimeError(f"split failed: {strerror(rc)}")


# --- Demo ------------------------------------------------------------------
if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input> <output_dir> [--playlist]", file=sys.stderr)
        sys.exit(1)

    input_path = sys.argv[1]
    output_dir = sys.argv[2]
    playlist   = "--playlist" in sys.argv[3:]

    print(f"libkiri {version()}")
    info = probe(input_path)
    print(f"Input: {info['format']}  "
          f"{info['size_bytes'] / 1_048_576:.1f} MiB  "
          f"{info['duration_us'] / 1_000_000:.1f} s  "
          f"{info['video_codec']} {info['video_width']}x{info['video_height']}")

    # Split into 200 MiB chunks
    max_bytes = 200 * 1024 * 1024

    def on_progress(seg, total, done, tot):
        pct = 100.0 * done / tot if tot else 0
        print(f"\r segment {seg}/{total}  [{pct:.1f}%]", end="", flush=True)
        return False

    def on_log(level, msg):
        tag = ("error", "warn", "info", "debug")[level]
        print(f"\n[{tag}] {msg}", file=sys.stderr)

    split(input_path, output_dir, max_bytes,
          make_playlist=playlist,
          on_progress=on_progress,
          on_log=on_log)
    print("\nDone.")

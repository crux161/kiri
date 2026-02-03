# ✃  Kiri (切り)

**Kiri** is a lightweight, systems-level video splitter designed specifically to handle the **FAT32 4GB file size limit**. 

Unlike generic file splitters (like `split`), Kiri is **media-aware**. It uses FFmpeg libraries to parse video containers and perform "smart splits" on video keyframes. This ensures that every segment is a valid, playable video file that can be watched independently or played sequentially without corruption.

## ✨ Features

* 📁 **FAT32 Compatibility:** Automatically splits files exceeding 4GB (or custom sizes) to fit on older filesystems.
* ䷖ **Smart Keyframe Splitting:** Hunts for I-frames (Keyframes) within a "safety margin" before the size limit to ensure clean cuts.
* ⛑️**Corruption Resilience:** Includes sanitizers for broken timestamps (common in downloaded or recovered files) to prevent muxer crashes.
* 📼 **Playlist Generation:** Optionally creates an `.m3u8` playlist for seamless playback of segments.
* 🛄 **Disk Pre-allocation:** Uses POSIX/macOS `fcntl` pre-allocation to reduce disk fragmentation on flash drives.
* 🔥 **Zero-Copy Muxing:** Streams packets directly without re-encoding, preserving original quality and speed.

## 📋 Dependencies

Kiri requires the **FFmpeg** development libraries to build.

### 🖥️ macOS (Homebrew)
```bash
brew install ffmpeg pkg-config
git clone https://github.com/crux161/kiri.git 
cd kiri
make -j$(nproc)
make re
```

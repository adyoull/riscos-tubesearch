# Changelog

## 0.1.0 (2026-10-12): first test build

- Search, related videos, a channel's videos, More, Back (24 pages).
- Thumbnails (JPEG to 32bpp sprites through SpriteExtend), cached.
- Plays in Reel: H.264 up to the chosen quality (360p-1080p) + AAC, HLS
  for live streams, through a yt-dlp-style JSON file and *Reel.
- Pasted YouTube addresses play, with their related videos.
- Non-blocking HTTPS through AcornSSL; client settings in Config.
- tubecli for the command line.

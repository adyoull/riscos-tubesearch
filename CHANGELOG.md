# Changelog

## 0.2.0-rc1 (2026-10-12): first release candidate

- Renamed from Tube to TubeSearch (!TubeSearch, riscos-tubesearch; the
  command-line tool is now tscli, settings in Choices:TubeSearch.Config).
- Info box (Info on the icon bar menu): name, purpose, author, version.
- The version comes from the Makefile (one place for the zip, !Help and Info).
- 0.1.0 tested on a Raspberry Pi 4: search, pictures, playing in Reel,
  related videos and channels work.

## 0.1.0 (2026-10-12): first test build

- Search, related videos, a channel's videos, More, Back (24 pages).
- Thumbnails (JPEG to 32bpp sprites through SpriteExtend), cached.
- Plays in Reel: H.264 up to the chosen quality (360p-1080p) + AAC, HLS
  for live streams, through a yt-dlp-style JSON file and *Reel.
- Pasted YouTube addresses play, with their related videos.
- Non-blocking HTTPS through AcornSSL; client settings in Config.
- tscli for the command line.

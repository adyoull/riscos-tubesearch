# Tube: YouTube on RISC OS

A desktop program for RISC OS 5 that searches YouTube, shows the results
with their pictures, and plays the video you click in **Reel**
(riscos-ffmpeg), which decodes H.264 on the Raspberry Pi's hardware
through ReelHWAccel. No web browser is involved: Tube talks to YouTube's
own API (Innertube) over HTTPS through the AcornSSL module.

Not made by or connected with YouTube or Google.

## Download

From [Releases](https://github.com/adyoull/riscos-tube/releases):
`Tube-x.y.z.zip`. Unzip it on RISC OS (SparkFS or InfoZip keep the
filetypes).

## Using it

Unzip `Tube-x.y.z.zip`, open the directory with `!Reel` in it once (so
the `*Reel` command exists), and run `!Tube`. Click its icon bar icon,
type a search and press Return.

| | |
|---|---|
| Select on a video | plays it in Reel |
| Adjust on a video | the videos related to it |
| Select on a channel's name | that channel's videos |
| More (last tile) | the next part of the list |
| Back | the page before |
| Menu | Play, Related, Channel, Back, Quality (360p–1080p) |
| a pasted YouTube address | plays it, and shows what's related |
| Menu on the icon bar icon | Info, Search, Quality, Quit |

Needs RISC OS 5.28 or later (AcornSSL), SharedUnixLibrary 1.16+ and
ARMEABISupport (PackMan), Reel 0.1.27+, and a 16 or 32 bit-per-pixel
desktop.

## How it works

- **Lists** (search, related, a channel's videos, continuations): the
  WEB client's `youtubei/v1/search`, `next` and `browse`, read by a
  walker that collects `videoRenderer`, `lockupViewModel` and
  `channelRenderer` wherever they are, so small layout changes don't
  break it.
- **Streams**: the VISIONOS client's `player`, which (October 2026) gives
  plain addresses: no JavaScript deciphering and no proof-of-origin
  token. Tube picks H.264 (`avc1`) no taller than the chosen quality and
  the default AAC track (HLS for live streams).
- **Playing**: Tube writes a yt-dlp `-j` style JSON file (title, the two
  addresses, the User-Agent) to `<Wimp$ScrapDir>` and runs `*Reel` on it.
- **Network**: `src/http.c` is a non-blocking HTTP/1.1 client (keep-alive,
  chunked, one connection per host), driven from null events so the
  desktop never waits. `src/transport_riscos.c` hands a UnixLib socket to
  AcornSSL the way riscos-ffmpeg's `tls_acornssl.c` does.
- **Pictures**: `i.ytimg.com` thumbnails drawn once into 32bpp sprites by
  SpriteExtend (`JPEG_PlotScaled` with output switched to the sprite).

YouTube changes its clients every few months. Every client string can be
overridden in `!Tube.Config` or `Choices:Tube.Config` without a rebuild.

`tubecli` (in `!Tube`, and buildable on Linux) runs the same code from a
command line: `tubecli search …`, `related ID`, `channel UC…`,
`play ID [lines] [file]`, with `-dump DIR` to keep YouTube's answers.

## Building and testing

With [riscos-crossdev](https://github.com/adyoull/riscos-crossdev) (a tag,
toolchain installed with `make prebuilt`):

```sh
make RCD_DIR=path/to/riscos-crossdev          # out/Tube-VERSION.zip, after the crossdev checks
make test RCD_DIR=path/to/riscos-crossdev     # all the Linux tests
```

The tests use real YouTube answers recorded by NewPipe's test suite
(`tests/fetch-fixtures.sh` fetches them; they aren't kept here):

- `test_yt`: the JSON reader and the list/stream parsers on those answers;
- `test_http`: the HTTP client against a fake connection that delivers
  bytes in random small pieces (chunked, keep-alive, stale connections,
  cancelling, timeouts);
- `test_ui`: the whole desktop front end against crossdev's fake RISC OS
  (a fake Wimp, SpriteExtend and network): search, pictures, play,
  related, channel, More, Back, menus, resizing, errors;
- `e2e.sh`: `tubecli` over real TLS against a local stand-in for YouTube;
- the portable code again as ARM code under QEMU with RISC OS's alignment
  trap.

Tested by hand on a Raspberry Pi 4 (RISC OS 5, Reel 0.1.27+).

## Licence and support

MIT (`COPYING`). Support is as-is, at the maintainer's own pace; report
problems in GitHub Issues (say which release, and what `tubecli` prints
for the same search or video). Written with Claude as a coding assistant;
Andrew Youll directs the work and tests on a Raspberry Pi 4.

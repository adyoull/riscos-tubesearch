# Changelog

## Unreleased

- Built and tested on GitHub for every push and pull request, in
  riscos-crossdev 1.8.2's container image (.riscos-deps), with a pull
  request when riscos-crossdev makes a new release (riscos-crossdev's CI
  chain: riscos-build.yml, riscos-deps-update.yml).

## 0.2.0-rc12 (2026-10-22): my language, without a territory

- RISC OS 5 has territory modules for only a few countries (none for
  France), and choosing one that isn't there stops the machine, so French
  users stay on the UK territory and rc10/rc11 took them for English. On
  the UK territory (or one with no language) TubeSearch now goes by the
  configured keyboard (OS_Byte 71, 255): a French keyboard gives fr and
  FR, a German one de and DE.
- Sound > My language is now a menu: Automatic (the territory's or the
  keyboard's) or one of 30 languages, which sets the language and country
  YouTube is told. Saved in Choices (my_language).
- The Dvorak keyboards' numbers (70 UK, 71 USA) are known.

## 0.2.0-rc11 (2026-10-21): the languages on the Sound menu

- Sound has two more entries: Language, a list of 30 languages to play
  that one's track (else the original), and I also understand, the same
  list to tick the others you understand (Adjust ticks several). Both are
  saved in Choices:TubeSearch.Choices. In rc10 these were Config only.

## 0.2.0-rc10 (2026-10-21): languages

- The language and country YouTube is told come from RISC OS's territory
  (Territory_Number: France gives fr and FR, Germany de and DE, the USA
  en and US, Canada1 fr-CA...), before the Config files, which can still set them. Until
  now everyone got en-GB and GB, so My language gave French users English
  dubs.
- Titles and names are shown in the desktop's alphabet (OS_Byte 71):
  Latin-1 to Latin-10, Cyrillic, Greek, Hebrew, Arabic, Welsh, as well as
  UTF-8. Before, everything but UTF-8 was treated as Latin-1. In an
  alphabet TubeSearch doesn't know, only ASCII is shown as itself.
  tscli does the same, and sends its search words as UTF-8.
- Search words with accented letters were sent to YouTube in the desktop's
  alphabet rather than UTF-8; they're converted now.
- Config's languages: others you understand (languages = en de). A video
  made in one of them plays as it was made rather than dubbed into yours;
  a dub in one of them is taken over an original you don't. YouTube's
  "default" mark on a track now only decides between equals, so a default
  dub in another language no longer beats one in yours. Sound set to a
  language (audio_language = de) means that one, whatever languages says.
- A sound track's name saying it's the original is recognised in other
  languages too ("Deutsch (Original)"), and the hint about YouTube's bot
  check works when the reason isn't in English ("Bot", "robot").
- The Sound menu names your language (My language (French)).
- A subscribed channel read from its page (when its feed fails) is asked
  for in English, as its videos' times are read from "3 days ago".
- tscli: -lang, -region and -languages.

## 0.2.0-rc9 (2026-10-21): subscriptions

- Channel subscriptions, kept on this machine (no YouTube account):
  Subscribe / Unsubscribe on the window menu (a channel's, or a video's
  for its channel, or a channel's own page); a Subscriptions button (and
  icon bar menu item) showing the channels' newest videos, newest first,
  with the ones since you last looked marked New; My channels (Adjust on
  the button) listing the channels.
- Bring in your YouTube subscriptions by dropping Google Takeout's
  subscriptions.csv (in any language), or a NewPipe or FreeTube export,
  on the window or icon bar icon.
- The videos come from each channel's feed
  (www.youtube.com/feeds/videos.xml?channel_id=...), one at a time with a
  count on the status line; a channel whose feed fails is read from its
  page instead; if some can't be read at all the status line says how
  many, and their new videos are still New next time. Kept in
  Choices:TubeSearch.Subscriptions.
- tscli: feed UC... and import FILE.

## 0.2.0-rc8 (2026-10-17): Refresh

- A Refresh button beside Back (also F5, and Refresh on the window's
  menu) fetches the page on screen again: new results, a channel's newest
  videos, or a live stream that has started since. Asked for by Chris.

## 0.2.0-rc7 (2026-10-15): a frame rate limit, and choices that stick

- Frame rate on the menus: 30 a second (the default) or 60 (Config
  max_fps; tscli -fps). Within both Quality and Frame rate, the tallest;
  if a video only has faster ones at that Quality, the slowest of those.
  DASH and HLS choose the same way (rc5/rc6's HLS only preferred 30 at the
  same height; DASH ignored frame rates).
- Quality, Frame rate, Sound and the Uploaded filter are saved in
  Choices:TubeSearch.Choices as they're chosen and read at the next start,
  after the Config files (before, choosing 720p was forgotten).

## 0.2.0-rc6 (2026-10-15): the right sound track

- Videos with dubbed tracks (YouTube's auto-dubbing adds many) could start
  in German: rc5's HLS took the track the playlist marked default, which
  can be a dub. TubeSearch now reads each track's language and whether
  it's the original (the xtags in its address: acont=original|dubbed,
  lang=; HLS's LANGUAGE and NAME; DASH's audioTrack) and plays the one in
  your language (Config's language), else the original. DASH chooses the
  same way.
- Sound on the menus: My language / Original (Config audio_language =
  mine | original | a language code). The status line says which track
  is playing, as "sound: English (original)". tscli: -sound.

## 0.2.0-rc5 (2026-10-15): HLS, for smooth playing

- Videos are played as HLS by default: TubeSearch fetches YouTube's HLS
  master playlist and gives Reel the H.264 variant no taller than the
  chosen quality (30 frames a second preferred at the same height) and its
  default sound, as two media playlists. YouTube can slow down, or refuse
  part way, its long DASH streams, which showed as buffering or "access
  denied" in Reel; HLS is many short requests. Suggested by Chris, who
  found yt-dlp's HLS formats play smoothly where its DASH ones don't.
- Config: stream_format = hls (default) | dash. Live streams are always
  HLS (now the right variant, not the whole master playlist). If the HLS
  playlist can't be used, the DASH streams are, as before. Videos that
  need the TV-app fallback are still played as DASH.
- tscli play says (HLS) or (DASH).

## 0.2.0-rc4 (2026-10-13): less address space

- A dynamic area reserves its whole maximum of logical address space as
  soon as it's made, and some machines are running out of it. TubeSearch's
  heap now asks for 64 MB (was 128 MB; it uses about 40 MB at most) and
  the solver's for 128 MB (was 256 MB; a first solve needs about 105-112
  MB). With UnixLib 5.0.3.x, a heap that needs more gets a second area
  ("TubeSearch Heap 2", "TubeSearch Solver 2") instead of failing.

## 0.2.0-rc3 (2026-10-13): an upload-date filter, and proxies

- Uploaded filter beside Search: Any time, Today, This week, This month,
  This year (YouTube's own filter, sent as the search's params). Changing
  it searches again; Back returns to the results before; More keeps it.
  Config: search_date. tscli: -date.
- Web proxies (HTTP CONNECT), for connections YouTube keeps calling a bot:
  the http_proxy variable, which Reel's FFmpeg reads too. They must agree
  (YouTube's video addresses only work from the connection that asked for
  them), so TubeSearch reads http_proxy and no_proxy exactly as FFmpeg 5.1
  does, from the system before each search or video. user:password@ for
  proxies that want a login (Basic). Everything TubeSearch fetches goes
  through it: lists, the player, pictures, the solver updates. tscli:
  -proxy URL|none (for trying a proxy).
- Asked for by Chris (5G connection).

## 0.2.0-rc2 (2026-10-12): the TV fallback, with QuickJS

- When YouTube won't give the visionOS client playable addresses ("Sign in
  to confirm you're not a bot"), TubeSearch asks again as YouTube's TV app
  (yt-dlp's tv_samsung client) and answers its address challenges (the
  signature and "n") with yt-dlp's own solver scripts (yt-dlp/ejs 0.8.0)
  running in QuickJS 2026-06-04, built into tscli without its std/os
  modules. Config: stream_client = auto | player | tv.
- The solve runs in a helper task (tscli -wimp solve, through the Solve Obey
  file) with its own memory (up to about 110 MB for a new YouTube player),
  polling the Wimp so the desktop carries on. One at a time; choosing
  another video stops it. The prepared player is kept in
  <Wimp$ScrapDir>.TubeSearch.Players, so only the first video after a
  YouTube player update is slow.
- The solver scripts keep themselves up to date: once a day TubeSearch reads
  ejs.json from its GitHub repository (kept at the version yt-dlp pins by a
  weekly GitHub Action that also checks the new scripts against a recorded
  player), downloads that version from yt-dlp/ejs's releases, and checks
  its SHA3-512 hashes. Scripts that fail are marked bad and the bundled copy
  is used, in the same play. Config: ejs_update, ejs_manifest_url.
- The HTTP client follows redirects (GitHub's release downloads).
- The thumbnail user agent is a Config setting (thumb_user_agent).
- Videos YouTube calls unavailable don't try the TV app.
- tscli: solve, update, -client, -ejs, -state, -work.

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

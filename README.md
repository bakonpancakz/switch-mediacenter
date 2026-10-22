# Media Center

A touch-first media center for a homebrew Nintendo Switch, and the Windows tool that feeds it.

**The PC does the work; the Switch just plays.** You tell *Media Center Sync* on your PC which folders go to the
Switch and as what. It makes everything the Switch needs (thumbnails, video stills, waveforms, show and movie
details from TMDB, book covers, JPEGs of photos the Switch can't open), then copies whatever changed onto the SD
card along with one database holding the library and every picture. The Switch never scans folders or makes
anything itself, so even a folder of thousands of photos opens instantly.

| Tab | What it does |
|-----|--------------|
| **Files** | Your folders as a masonry wall, grid or list. Photo viewer with zoom, rotate, filmstrip and slideshows; animated GIF/APNG/WebP; videos and music play right in it, music with a visualizer |
| **Media** | Shows and movies with posters, overviews and episode names from TMDB, and Continue watching. Video player with subtitles, audio tracks, zoom and resume |
| **Books** | EPUB/TXT reader, and a page reader for comics (CBZ/CBR/CB7/CBT) and PDFs |
| **Spotify** | A Spotify Connect speaker with cover art, a visualizer and an equalizer (Premium required) |

## What you need

- A Switch that runs homebrew (Atmosphère), launched with **title takeover**: hold R while starting a game.
  Applet mode has too little memory for video.
- A Windows 10 or 11 PC.
- A way to put the Switch's SD card in the PC: a card reader, or hekate's USB mass storage
  (hekate → Tools → USB Tools → SD Card).

## Setting up

1. Copy `MediaCenter.nro` to `switch/mediacenter/` on the SD card.
2. Run `MediaCenterSync.exe` on the PC. There's nothing to install; it lives in the tray.
3. Press **Add…**, pick a folder, choose what's in it (Files, Media or Books) and the name it gets on the Switch.
4. Optionally, open **Settings…** and paste a free [TMDB](https://www.themoviedb.org/settings/api) API key for
   posters, titles and episode names.
5. Put the SD card in the PC. The tool finds it by its `switch\mediacenter` folder and syncs (or press
   **Sync now**). Eject the card in Windows before taking it out.

On the PC, Windows does most of the work: photos, lengths and waveforms come from Windows' own decoders, video
and comic thumbnails from the Shell's thumbnail handlers (the ones Explorer uses, such as Icaros), PDF covers from
Windows' PDF renderer, and CBR covers from 7-Zip when it's installed. ffmpeg is only a fallback, found next to the
exe, on the PATH, or set in Settings.

## Controls

Everything works by touch, and with a controller (Joy-Cons attached or as a pair, or a Pro Controller). Each button
means one thing everywhere:

| Button | Does |
| --- | --- |
| Left stick, D-pad | moves the cursor |
| **A** | taps (hold to drag). While the cursor is hidden, the first press only shows it |
| **B** | back |
| Right stick | scrolls; in the viewer, moves around a zoomed-in photo |
| **L** / **R** | the item before / after: photo, episode, page, Spotify track |
| D-pad left / right | in a player, the viewer or a book: −10 s / +10 s, or the photo or page before / after |
| **ZL** / **ZR** | in a player, the viewer or a comic: zoom out / in at the cursor. On lists: scroll up / down |
| **X** | play / pause |
| **Y** | show or hide the controls |
| **+** | quit |

In handheld mode the photo viewer and books turn with the console (Settings → Display → Auto-rotate).

## Files

- **Browsing:** a masonry wall (the default), a grid or a list, small, medium or large, all under **View**.
  - **Sort** by name, date (taken, from EXIF where there is one), size, length or type, either way round.
  - **Group by** type or month; tap a heading to fold it.
  - **Folder thumbnails** can be turned off, for folders whose contents are nobody else's business.
  - Drag the scrollbar's thumb to fly through a long folder; a bubble shows where you are (the letter, the month…).
- **Photos:** swipe, pinch or double-tap to zoom, drag to pan. Tap for the controls: rotate, details and the
  filmstrip. **Slideshow** sets the time per photo and shuffle; photos crossfade and videos play to the end.
- **Formats:** JPEG, PNG, WebP, GIF and BMP open as they are. HEIC/HEIF, AVIF, TIFF and JPEG XL, and photos over
  24 megapixels, get a 2560 px copy made by the PC. Animated GIF, APNG and WebP play.
- **Videos and music** play in the viewer. Music shows its cover with the same visualizer as the Spotify tab.
- Files the PC couldn't read aren't synced, so there are no dead tiles; the sync log names them.

## Media

- **Library:** Continue watching (the ✕ on a card takes it off, **Clear** empties the row), then Shows and Movies.
  Show pages have seasons, episode stills and overviews, and a Play/Resume button for the next episode.
  **Folders** shows the plain folders instead.
- **Wrong show or movie?** **Fix shows/movies…** on the PC lists every match, the ones TMDB didn't find first.
  Search for the right one, or mark it "not on TMDB".
- **Names** are understood the usual ways: `Show/Season 1/Show S01E02.mkv`, `Show 1x02`, `[Group] Show - 05`,
  `Movie (2019)`. Anime numbered past the first season is placed into TMDB's seasons.
- **Player:** subtitle and audio tracks, subtitle size and delay, rotate, fit/fill, aspect, speed and play-next.
  Subtitle files next to a video load by themselves, and playback resumes where you left off. Hardware decoding
  covers H.264, HEVC, VP8/VP9, AV1 and more.

## Spotify

Pick the Switch in Spotify's device list, like any smart speaker. The first pairing needs both on the same Wi-Fi;
after that the saved login is reused.

- **Visualizer** (bars icon): Bars, Wave, Ribbons, Pulse, Mirror, Mountains, Stars, Waterfall or Halo.
- **Equalizer** (faders icon): a 10-band graphic EQ, or a parametric EQ with up to 8 bands, with presets
  (one for the Switch's own speakers) and your own.

## Books

- The shelf shows titles, authors and covers; a book turns to portrait as it opens, on its contents page.
- Every chapter starts on a new page. Headings, bold, italic, lists, quotes and images are rendered, and footnotes
  open in a popup.
- **Aa** settings: 11 fonts, size, spacing, margins, paragraph style, text colour and brightness.
- Comics and PDFs: tap the sides to turn, pinch or double-tap to zoom, right-to-left reading. PDFs are rendered with
  MuPDF and stay sharp when zoomed.

## How a sync works

1. **Build:** scan the mapped folders and make what's missing. Everything is cached in
   `%LOCALAPPDATA%\MediaCenterSync\work.db`, so the next build only redoes changed files.
2. **Compare:** the card's `system.db` records what the tool put there. New and changed files are copied, removed
   ones deleted. If nothing changed, the card isn't written at all.
3. **Copy:** files go to `.incoming` first and move into `.media` once complete.
4. **Write the library** in one transaction, then delete what's gone.

A mapped folder that's missing (an unplugged drive, say) stops the sync instead of reading as "everything was
deleted". Deletes only happen inside mapped folders, and nothing on the PC is ever touched. Pulling the card out
midway loses at most the last couple of seconds; the next sync carries on.

File names on the card are plain ASCII: the Switch can't open names with other characters on a FAT32 card, so the
PC gives those an ASCII name plus a short hash. The app still shows the real name.

```
sdmc:/switch/mediacenter/
├── MediaCenter.nro
├── system.db                      the library and all its pictures (written by the PC, read by the Switch)
├── .media/files|media|books/      your folders, as laid out on the PC
├── settings.ini                   settings (plain text)
├── positions.txt, recent.txt      where you are in each video and book; Continue watching
└── log.txt                        diagnostics
```

## Building

The Switch app needs [devkitPro](https://devkitpro.org) with these switch portlibs: `switch-mpv`, `switch-ffmpeg`,
`switch-sdl2`, `switch-sdl2_ttf`, `switch-sdl2_image`, `switch-libarchive`, `switch-libjpeg-turbo`,
`switch-mbedtls`, `switch-libvorbisidec`, `switch-mupdf`, `switch-libavif` and `switch-curl`. The PC tool and the
tests need [msys2](https://www.msys2.org)'s ucrt64 g++.

```powershell
./build.ps1                 # the Switch app, laid out in dist/switch/mediacenter
pc/build.ps1                # Media Center Sync (a static exe, copied to dist/pc)
pc/build.ps1 test           # the PC tests
test/run_host_tests.ps1     # the Switch app's tests, run on the PC
```

The scripts look for `C:\devkitPro` and `C:\msys64\ucrt64\bin`. To use other locations, create
`toolchain.local.ps1` next to `build.ps1`:

```powershell
$env:DEVKITPRO_WIN = "D:\tools\devkitPro"
$env:UCRT64_BIN = "D:\tools\msys64\ucrt64\bin"
```

How the pieces fit together is in [docs/DESIGN.md](docs/DESIGN.md).

## Known limitations

- No transcoding: videos are copied as they are, so ones the Switch can't decode smoothly stay that way.
- Chinese, Japanese, Korean or emoji text a PDF doesn't embed shows as boxes.
- The visualizer and EQ need the built-in Spotify receiver.

## Credits and license

Media Center is licensed under the GPL-3.0 (see [LICENSE](LICENSE)), as it includes cspot. It uses:

- [cspot](https://github.com/feelfreelinux/cspot) and bell (GPL-3.0), [nanopb](https://github.com/nanopb/nanopb)
  (zlib), [cJSON](https://github.com/DaveGamble/cJSON) (MIT), [SQLite](https://sqlite.org) (public domain),
  bundled in `vendor/`
- [mpv](https://mpv.io), [FFmpeg](https://ffmpeg.org), [MuPDF](https://mupdf.com) (AGPL-3.0),
  [SDL2](https://libsdl.org), libarchive, libjpeg-turbo, libavif, mbedTLS and curl, from devkitPro's portlibs
- Inter, Lexend, Noto Sans, Atkinson Hyperlegible, Literata, Source Serif, EB Garamond, Crimson Pro,
  Libre Baskerville and Tinos fonts (SIL Open Font License, a copy next to each in `romfs/`)

The code was written by Claude, Anthropic's AI model.

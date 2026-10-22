# Media Center: design

The PC is the source of truth. The Switch is a player. They only meet on the SD card.

```
 PC folders ──► MediaCenterSync.exe ──► SD card (card reader / hekate UMS) ──► MediaCenter.nro
 (mappings)     scan, thumbnails,       switch/mediacenter/                    reads system.db,
                TMDB, covers, diff      system.db, .media/                     plays and shows
```

## What lives where

**On the PC** (`%LOCALAPPDATA%\MediaCenterSync\`):

- `config.ini`: the mappings, TMDB token, ffmpeg folder and options.
- `work.db`: every scanned file (size, date, kind, length, size in pixels), the pictures made for them (keyed by
  the file's size and date, so only changed files are redone), the TMDB cache and match fixes.

**On the card** (`sdmc:/switch/mediacenter/`):

| Path | What |
|---|---|
| `system.db` | SQLite, written by the PC, only read by the Switch. Every item and its metadata, every picture (thumbnails, posters, covers) as a blob, and `synced`: the files the PC put on the card. |
| `.media/files/<Name>/…` | a Files mapping, laid out as on the PC |
| `.media/media/<Name>/…` | a Media mapping (movies, shows, subtitle files) |
| `.media/books/<Name>/…` | a Books mapping |
| `.incoming/` | files on their way in during a sync; gone afterwards |
| `settings.ini`, `positions.txt`, `recent.txt`, `log.txt`, `spotify-login.json`, `spotify-eq.txt` | the Switch's own small files. The PC never touches them. |

The Switch never scans folders, makes pictures or talks to TMDB. A folder is one indexed query and its pictures
come out of the one open database, not a file each.

Pictures the Switch can't open (HEIC, TIFF, AVIF, JPEG XL) are synced as a JPEG the PC makes, named
`x.heic.jpg`, instead of the original. The large pictures stay out of `system.db`, which keeps it far from FAT32's
4 GB file limit.

## Making the pictures

What Windows already has comes first, ffmpeg is the fallback:

| | First | Then |
|---|---|---|
| Photos | WIC (with EXIF rotation and date) | the Shell's thumbnail handlers (Icaros), ffmpeg |
| Video stills | the Shell's thumbnail (Icaros) | ffmpeg at a few points, when the Shell's is black |
| Lengths, sizes, rotation | Media Foundation | ffprobe |
| Waveforms | Media Foundation (decodes to PCM) | ffmpeg |
| EPUB covers, titles, authors | the EPUB itself (zip + OPF) | the Shell |
| CBZ | the archive's first page | the Shell |
| CBR, CB7, CBT | the Shell (Icaros) | 7-Zip |
| PDF | Windows' PDF renderer (Windows.Data.Pdf) | |

## A sync

1. **Build.** Scan the mappings, make what's missing, look things up on TMDB. A mapped folder that's missing
   stops the sync: an unplugged drive must not read as "everything was deleted".
2. **Find the card**: any drive with a `switch\mediacenter` folder (the one with `MediaCenter.nro` first).
3. **Compare** the library with the card's `synced` table (what's there, with the size and date it had on the
   PC). New and changed files get copied, removed ones deleted. If `system.db` is missing or from another
   version, a new one is made, and files already on the card with the right size count as copied.
4. **Copy** each file to `.incoming`, then move it into `.media`. Each one is recorded in `synced` (committed
   every couple of seconds).
5. **Write the library** in one transaction: items for the files that are really there, pictures added and
   dropped by hash.
6. **Delete** what the library no longer has (first, instead, when the card is too full otherwise).

If nothing changed, the card isn't written at all. A card pulled out mid-sync loses at most the last couple of
seconds of copying. SQLite's journal undoes a half-written library, on the next sync or when the Switch opens it.

## Switch app

- `core/library.cpp`: `system.db` behind one lock (folders, shows, movies, books, picture blobs).
- `ui/pictures.cpp`: two workers decode what's on screen; a few textures uploaded per frame; least recently used
  dropped.
- `tabs/`: Files, Media, Books. `media/video.cpp`: mpv. `books/`: the readers. `spotify/`: Spotify Connect.

## Non-goals

No transcoding. No syncing back from the Switch. No syncing over USB cable or Wi-Fi: the card goes in the PC
(a card reader, or hekate's USB mass storage).

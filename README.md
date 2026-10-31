# THE RHYTHM OF FIGHTERS — PS Vita port

A PS Vita port of the Android version (1.4.1) of SNK PLAYMORE's rhythm game
**THE RHYTHM OF FIGHTERS**.

The original native library (`libAppMain.so`) runs on the Vita through a
so-loader: the Android/Java side of the game is reimplemented in C, and
OpenGL ES goes through vitaGL.

**This repository contains no game files.** You need your own copy of the
game (the APK and the downloaded song data).

## Disclaimer

- This port was vibecoded by Claude Opus.

## Features

- Runs at a locked 30 fps (the game's own frame rate), stable even at stock clocks.
- Full button support: face buttons, D-pad, L/R = tap/hold, both analog sticks = flick
  (long notes ending in a flick work with the held button), Free Mode list navigation
  and tutorial navigation with buttons. The touch screen works as on a phone.
- Works offline: SNK's servers are gone, so shop purchases are emulated (free) and the
  one-time data download is skipped when the data is installed.
- Songs whose files were lost can be added back later; the port lists the missing
  files in `ux0:data/rof/missing_songs.txt`.
- Native (VFP) Ogg Vorbis decoding, RAM cache for the menu animations and other
  performance fixes, so loads and menus are fast on the Vita.

## Installing on the Vita

Requirements:

- A PS Vita with HENkaku / Enso and VitaShell.
- kubridge.skprx: Recommended version: v0.3.1_hotfix (https://github.com/bythos14/kubridge/releases)
- libshacccg.suprx
- YOUR OWN GAME FILES: APK file (to extract `libAppMain.so` file and `assets` folder from) and game data files (`com.snkplaymore.android009` folder)

Installation:

1. Install rof.vpk with VitaShell.
2. Copy the game files to `ux0:data/rof/`: `libAppMain.so` file, `com.snkplaymore.android009` folder with all the contents and `assets` folder with all the contents.
3. Launch the game.

The first launch prepares the graphics (shaders) and takes a little longer;
later launches are quick.

### Controls

- Touch screen ................ same as on a phone
- Cross Circle Square Triangle  tap / hold (normal and long notes)
- D-pad, L, R ................. tap / hold (during songs)
- Both analog sticks .......... flick in the stick's direction
- Long note ending in a flick . keep the button held, then flick a stick
- START / SELECT .............. back / pause


- In the menus:
- Free Mode ................... up/down scrolls the song list
- Tutorial .................... left/right turns pages,
                              Cross goes on (and presses PLAY), Circle exits
- Tutorial tips in songs ...... Cross or Circle to continue

## Building

Requirements:

- [VitaSDK **softfp**](https://github.com/vitasdk-softfp) (the game's `.so` uses the
  softfp float ABI), installed at `~/vitasdk-softfp`
- CMake, make, unzip

```bash
bash build.sh            # -> build/rof.vpk
bash package.sh <APK> <SONGS_DIR>   # -> ../vita_package (vpk + ux0/data/rof)
```

`package.sh` extracts `libAppMain.so` and the assets from your APK and copies
your downloaded song data next to them.

## Project layout

| Path | What |
|---|---|
| `source/main.c` | entry point, main loop, frame pacing |
| `source/java.c` | the game's Java methods (text drawing, purchases, device info…) via FalsoJNI |
| `source/patch.c` | hooks into the game (hidden dead buttons, credits, save/data fixes…) |
| `source/rof/` | port code: input, text (FreeType), file cache, Vorbis, tutorial, profiler… |
| `lib/` | bundled libraries, several with port-specific fixes (see below) |

## Bundled libraries

| Library | Upstream | Notes |
|---|---|---|
| vitaGL | [Rinnegatamante/vitaGL](https://github.com/Rinnegatamante/vitaGL) @ 9bf3e3f | Makefile: fixed version hash |
| vitaShaRK | [Rinnegatamante/vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) @ df24065 | |
| OpenSL ES | [frangarcj/opensles](https://github.com/frangarcj/opensles) @ e35b063 | zeroed buffer queues, resampling, saturated mixing, audio thread priority |
| so_util | [Rinnegatamante/so_util](https://github.com/Rinnegatamante/so_util) | |
| FalsoJNI | [v-atamanenko/FalsoJNI](https://github.com/v-atamanenko/FalsoJNI) | logger writes to log.txt |
| FalsoNDK | [elliencode/FalsoNDK](https://github.com/elliencode/FalsoNDK) | |
| stb_vorbis | [nothings/stb](https://github.com/nothings/stb) v1.22 | |

## NOTES

- SNK's servers are long gone: purchases in the Shop are free and the game
  works fully offline, provided you supply your own preserved files.
- If notes feel early or late against the music, use "Timing Adjust" in
  the game's options.
- Some downloadable songs seem to be lost in time... But if you have the files, copy them to
  ux0:data/rof/com.snkplaymore.android009/ and they will work; the list of missing files is
  written to ux0:data/rof/missing_songs.txt at launch.

## Credits

- Original game: © SNK PLAYMORE
- Based on [soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate)
  by Volodymyr Atamanenko, and so_util by Andy Nguyen (TheFloW) and Rinnegatamante
- vitaGL / vitaShaRK by Rinnegatamante, kubridge by bythos14

## License

The port code is MIT licensed (see [LICENSE](LICENSE)); bundled libraries keep
their own licenses.

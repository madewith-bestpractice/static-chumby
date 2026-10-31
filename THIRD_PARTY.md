# Third-party software

| Component | Used for | License | Text |
|---|---|---|---|
| Helix MP3 and AAC decoders (RealNetworks, via [pschatzmann/arduino-libhelix](https://github.com/pschatzmann/arduino-libhelix) @ f6e55a3) | Decoding | RealNetworks Public Source License 1.0 | `LICENSES/helix-RPSL.txt`, `LICENSES/helix-LICENSE.txt` |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) 3.6.4 | TLS, SHA-256 | Apache-2.0 | `LICENSES/mbedtls-Apache-2.0.txt` |
| [FFmpeg](https://ffmpeg.org) 7.1.1 (the separate `ffmpeg` program; a fallback decoder) | Decoding uncommon formats | LGPL-3.0-or-later | `LICENSES/ffmpeg-LGPL-3.0.txt`, `LICENSES/ffmpeg-GPL-3.0.txt` |
| [musl](https://musl.libc.org) (via zig) | C library, statically linked | MIT | `LICENSES/musl-MIT.txt` |
| [QR Code generator](https://www.nayuki.io/page/qr-code-generator-library) (Project Nayuki) | Episode-page QR codes | MIT | in `src/third_party/qrcodegen.h` |
| IBM Plex Sans (IBM) | Text, rendered to bitmaps in `src/fonts.h` | SIL OFL 1.1 | `LICENSES/IBMPlexSans-OFL-1.1.txt` |
| [Onest](https://github.com/googlefonts/onest) | Station names, rendered to bitmaps in `src/fonts.h` | SIL OFL 1.1 | `LICENSES/Onest-OFL-1.1.txt` |
| Material Icons and Material Symbols (Google) | Icons, rendered to bitmaps in `src/icons.h` | Apache-2.0 | `LICENSES/mbedtls-Apache-2.0.txt` (same license text) |

FFmpeg is built from the unmodified 7.1.1 release plus
`toolchain/ffmpeg-7.1.1-trim-fft-tables.patch`, configured by
`toolchain/build_deps.sh`. Its source is at
https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz.

The bitmaps in `src/fonts.h` are converted from IBM Plex Sans and Onest. As
modified versions of those fonts they stay under the SIL Open Font License, and
they aren't named "Plex". Our changes to Helix are `ConfigHelix.h`,
`helix_memory.c` and one line in `libhelix-aac/assembly.h` (`__unix__`); they
are available here under the RPSL.

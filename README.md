# okular-mupdf

A PDF backend (generator) for [Okular](https://okular.kde.org/) based on [MuPDF](https://mupdf.com/). It replaces
the default Poppler backend and makes rendering much faster, especially when zooming heavy pages on HiDPI screens.

## Why

Okular renders PDF files through Poppler in a single thread and draws the visible pages again from scratch on every
zoom step. On a 4K screen with fractional scaling a page is several thousand pixels wide, so documents with large
images (print-ready files, scanned books, illustrated rulebooks) take seconds per page to appear.

This backend:

- renders with MuPDF, which is several times faster than Poppler's Splash renderer;
- parses each page once into a MuPDF display list and keeps it, so zooming does not parse the page again;
- splits every image into horizontal bands and renders them in parallel on all CPU cores;
- stops a render in progress as soon as Okular no longer needs it, for example while zooming quickly.

## Benchmarks

One page rendered 3840 px wide (fit to width on a 4K screen), then zoom steps up to 5529 px, on a Ryzen 7 7800X3D
(16 threads). Poppler was measured with `pdftoppm`, which uses the same Splash renderer as Okular; the MuPDF numbers
come from the rendering code of this backend run outside Okular.

| Document                                    | Poppler | okular-mupdf |
|---------------------------------------------|--------:|-------------:|
| Illustrated rulebook, 32 pages, 50 MB       |   2.9 s |     30–80 ms |
| Print sheet of game cards, 16 pages, 60 MB  |   1.3 s |    60–125 ms |
| Print sheet of stickers, 1 page, 22 MB      |  15.6 s |   0.4–1.4 s  |

## Features

- Rendering, including tiled rendering at high zoom levels.
- Text selection, copying and search.
- Internal and external links.
- Table of contents.
- Document properties: title, author, subject, keywords, creator, producer, dates.
- Password-protected documents.
- Printing: the original file is sent to the printer.

## Limitations

These need the Poppler backend:

- Annotations stored in the PDF are drawn but cannot be edited. New annotations are kept in Okular's local storage
  and are not saved into the PDF.
- Forms, digital signatures, embedded files, JavaScript, sounds and movies.
- Overprint preview.

To choose the backend every time a PDF is opened, set `ChooseGenerators=true` in the `[Core General]` group of
`~/.config/okularpartrc`.

## Requirements

- Okular built with Qt 6 and KDE Frameworks 6, with its development files.
- MuPDF with its development files.
- A C++20 compiler, CMake 3.22 or newer, extra-cmake-modules.

Tested with Okular 26.08 and MuPDF 1.27.

On Debian, Ubuntu and KDE neon:

```sh
sudo apt install cmake g++ extra-cmake-modules qt6-base-dev libkf6coreaddons-dev okular-dev libmupdf-dev
```

On Arch Linux:

```sh
sudo pacman -S cmake extra-cmake-modules okular libmupdf
```

## Build and install

System-wide, next to the other Okular backends:

```sh
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

Okular uses the new backend for PDF files right away: it has priority 10, Poppler has 1. To go back to Poppler, delete
`okularGenerator_mupdf.so` from the Okular plugin directory (`okular_generators`).

Without root, install the plugin into your home directory and point Qt to it:

```sh
cmake -B build -DOKULAR_GENERATOR_INSTALL_DIR=$HOME/.local/lib/qt6/plugins/okular_generators
cmake --build build
cmake --install build
QT_PLUGIN_PATH=$HOME/.local/lib/qt6/plugins okular file.pdf
```

To make this permanent, set `QT_PLUGIN_PATH` in a wrapper script named `okular` placed before `/usr/bin` in `PATH`,
or in `~/.config/plasma-workspace/env/`.

Rebuild the plugin after upgrading Okular or MuPDF. If it cannot be loaded, Okular falls back to Poppler.

## Debugging

Render timings are logged under the `okular.mupdf` category:

```sh
QT_LOGGING_RULES=okular.mupdf.debug=true QT_FORCE_STDERR_LOGGING=1 okular file.pdf
```

## How it works

- The backend owns one MuPDF root context with lock callbacks; every thread works with its own clone of it.
- Access to the document is serialized. Display lists of the 32 most recently used pages are cached and shared
  between threads, which MuPDF allows.
- Each pixmap request from Okular is split into bands at least 64 px high, one per hardware thread. Every band is
  rendered straight into the memory of the resulting `QImage` (MuPDF BGRA matches `QImage::Format_RGB32`) with its
  own cancellation cookie. The generator thread checks Okular's cancel flag every 5 ms and aborts all bands at once.
- MuPDF's resource store is 2 GiB, so decoded images survive between zoom steps.
- The text layer is extracted from the same display list as MuPDF structured text.

## License

Copyright (C) 2026 Grief

[GNU Affero General Public License v3.0 or later](LICENSE), the same as MuPDF.

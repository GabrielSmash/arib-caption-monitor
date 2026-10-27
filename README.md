# ARIB Caption Monitor

Linux desktop monitor (Qt6) for ARIB captions in the Brazilian standard (ABNT NBR 15606-1 / ISDB-Tb),
received as MPEG-TS over UDP or read from `.ts` files, to test captions before sending them to the
broadcast system.

> **Current version: 1.0.0** — the first version installable on any Linux distribution.
>
> ⬇️ **[Download arib-caption-monitor-1.0.0.flatpak](https://github.com/GabrielSmash/arib-caption-monitor/releases/download/v1.0.0/arib-caption-monitor-1.0.0.flatpak)**
> · [release notes](https://github.com/GabrielSmash/arib-caption-monitor/releases/tag/v1.0.0) · [all releases](https://github.com/GabrielSmash/arib-caption-monitor/releases)
>
> How to install: see [Installation](#installation-any-linux-distribution).

- Receives MPEG-TS over UDP (unicast/multicast, RTP auto-detected) **or plays files** `.ts`/`.m2ts`
  (188, 192 and 204-byte packets) in real time, paced by the PCR (or the PTS, for caption-only files).
- Shows whether packets are arriving (packets/s, bitrate, source, time since the last datagram).
- Plays the video and audio when the stream carries them (FFmpeg + Qt Multimedia), with the captions on top.
- Decodes captions in real time with libaribcaption (`kABNT_NBR_15606_1_Latin`), keeping the screen like a
  receiver does: `CS` clears it, statements without `CS` draw over it, and positions/colors/sizes follow the encoder.
- Side panel: PAT, PMT (with descriptors), SDT, PIDs (packets, bitrate, continuity errors), caption management
  (language, DMF, format, TCS, rollup_mode), display format (SWF, SDF, SDP, SSM, SHS/SVS, SCR) and the
  regions on screen.
- "Comandos ARIB" tab: every data group received, with its control codes (CS, APR, SDF, SDP, colors…).
- "Mostrar posicionamento": draws the caption plane, the SDF/SDP display area (yellow) and the regions (cyan).
- "Validação" tab: 29 automatic rules (PMT, PES/data group including CRC-16, management data, Latin
  character set, control codes, layout against the SDF/SDP area and the plane, accessibility). Every rule
  shows its **source** — without the ABNT NBR 15606-1 text, rules from Japanese operation (ARIB TR-B14) are
  **warnings**, not errors. Severity, failures/checks count and the latest occurrences.

The user interface is in Portuguese.

## Installation (any Linux distribution)

The app is distributed as a **Flatpak**: an `arib-caption-monitor-<version>.flatpak` file published in the
repository releases. It works on Ubuntu, Debian, Fedora, Mint, Arch, etc.

1. Download the `.flatpak` from the [latest release](https://github.com/GabrielSmash/arib-caption-monitor/releases/latest).
   From the terminal (the repository is private, so the download uses your GitHub CLI login):
   ```bash
   gh release download v1.0.0 -R GabrielSmash/arib-caption-monitor -p '*.flatpak'
   ```
2. Make sure Flatpak and Flathub are set up (most distributions ship them; otherwise see
   https://flathub.org/setup):
   ```bash
   flatpak remote-add --if-not-exists --user flathub https://dl.flathub.org/repo/flathub.flatpakrepo
   ```
3. Install the downloaded file (the first time, it also downloads the KDE/Qt runtime from Flathub):
   ```bash
   flatpak install --user arib-caption-monitor-1.0.0.flatpak
   ```
4. Open it from the application menu (**ARIB Caption Monitor**) or from the terminal:
   ```bash
   flatpak run io.github.gabrielsmash.AribCaptionMonitor               # listens on UDP (saved port, default 1526)
   flatpak run io.github.gabrielsmash.AribCaptionMonitor recording.ts  # plays a file
   ```

To update, install the `.flatpak` of the new version the same way. To remove:
`flatpak uninstall io.github.gabrielsmash.AribCaptionMonitor`.

Package permissions: network (UDP reception), audio, video/GPU and **read-only** access to the home folder,
`/media` and `/run/media` (USB drives). Files elsewhere can be opened with the "Abrir .ts…" button.

### Building the package

```bash
flatpak install --user flathub org.flatpak.Builder   # once
./packaging/flatpak/build.sh                         # creates dist/arib-caption-monitor-<version>.flatpak
```

The build uses the `org.kde.Platform//6.10` runtime (Qt 6.10 and FFmpeg; H.264/AAC come from the
`org.freedesktop.Platform.codecs-extra` extension, installed automatically).

## Development

### Dependencies

```bash
sudo apt install build-essential cmake pkg-config qt6-base-dev qt6-multimedia-dev libfreetype-dev libfontconfig-dev fonts-dejavu \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
```

libaribcaption does **not** need to be installed: CMake fetches a pinned upstream version, applies
`patches/libaribcaption-deferred-wrap.patch` and builds it statically into the app (`git` and internet access
are needed on the first `cmake` run). Details in `patches/README.md`.

### Build

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
ctest --test-dir build        # layout (line wrapping) and validation, with real vectors (Globo, SkylarBox)
```

### Usage

```bash
./build/arib-caption-monitor                        # saved port (default 1526), unicast
./build/arib-caption-monitor -p 1526 -m 239.1.1.1   # multicast
./build/arib-caption-monitor --file recording.ts    # file (or the "Abrir .ts…" button / drag it onto the window)
```

The stream can be plain UDP or RTP (auto-detected). The caption PID is picked from the PMT
(stream_type 0x06 with component_tag 0x30–0x37 or data_component_id 0x0008).

For automated tests without a display: `QT_QPA_PLATFORM=offscreen ./build/arib-caption-monitor --tab 1 --snapshot shot.png --snapshot-after 10`
(`--tab`: 0 MPEG-TS, 1 Validation, 2 Commands, 3 Log).

## Known limitations

- Line wrapping at the edge of the display area is **deferred** until the next character (libaribcaption
  patch), as on real receivers; upstream libaribcaption wraps immediately.
- `SCR` (scroll) is shown in the panel, but libaribcaption ignores it when drawing.
- Captions are shown as they arrive (live monitor); the PTS is displayed but not used to delay them.
- Files: no seeking; "Repetir" (loop) restarts from the beginning. Interlaced video is shown without deinterlacing.
- The app forces Qt's PulseAudio audio backend (`QT_AUDIO_BACKEND`), because Qt 6.10's PipeWire backend does
  not list Bluetooth outputs.

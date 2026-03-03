# Ultramaster KR-106

An iPlug2 synthesizer plugin emulating the Roland Juno-106. 6-voice polyphonic, MPE capable, with AU / VST3 / CLAP / Standalone formats.

## Prerequisites

- macOS with **Xcode** installed (not just Command Line Tools)
- **iPlug2** cloned as a sibling directory at `../iPlug2`

## iPlug2 Setup (first time)

```bash
cd ~/src   # or wherever this repo lives
git clone https://github.com/iPlug2/iPlug2
cd iPlug2
git submodule update --init --recursive
cd Dependencies/Build/src
./download-prebuilt-libs.sh
cd ../../../IPlug/Extras
./download-iplug-sdks.sh
```

This project is tested against iPlug2 `v1.0.0-beta.508` (commit `3b32d40`). Any recent commit from `main` should work.

## Building

From the repo root:

```bash
make app       # Standalone .app
make vst3      # VST3 plugin
make au        # Audio Unit (AUv2)
make clap      # CLAP plugin
make all       # All formats at once
```

Default build configuration is `Debug`. For a release build:

```bash
CONFIG=Release make vst3
```

To build VST3 and immediately relaunch Reaper with a fresh plugin cache:

```bash
make reaper
```

Built plugins are placed in the standard system locations (`~/Library/Audio/Plug-Ins/`, etc.) by the Xcode project.

## Presets

Presets are compiled into `KR106_Presets.h` from `Factory_Patches.pat`. To regenerate after editing the patch file:

```bash
make presets
```

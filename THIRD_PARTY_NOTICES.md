# Third-party notices

This repository builds and/or integrates with third-party projects. Their licenses apply to those components, and may impose additional obligations when you distribute binaries.

## This repository

- License: GPL-3.0-or-later (see `LICENSE`)
- Note: the effective license obligations for distributed binaries depend on the full dependency closure and build configuration (especially FFmpeg).

## L-SMASH-Works

- Repo: `https://github.com/AkarinVS/L-SMASH-Works`
- License: ISC (see headers in upstream sources, e.g. `common/audio_output.c`).

## FFmpeg

- Repo: `https://github.com/AkarinVS/FFmpeg` (branch: `lsmas`)
- Used as: native dependency for demux/codec/scale/resample, linked into `lsmasnative.dll`.
- License: FFmpeg is LGPL/GPL depending on configuration and enabled components.
  - The provided build scripts enable `--enable-gpl --enable-version3`, so any distributed `lsmasnative.dll` built via these scripts is expected to be subject to GPLv3-or-later terms (at minimum for the combined work).
  - If you change FFmpeg configuration to an LGPL-only build, the resulting distribution obligations may differ.

## NuGet package note

The `MIR.LsmasSharp` NuGet package is managed-only and does not bundle native binaries. If you distribute `lsmasnative.dll` (or an app that includes it), ensure you comply with the licenses of the native dependencies you link to (FFmpeg, etc.).

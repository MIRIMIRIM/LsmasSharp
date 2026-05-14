# LsmasSharp

LsmasSharp is a .NET video/audio provider that ports the `LWLibavSource` and `LWLibavAudioSource` behavior from L-SMASH-Works into a standalone library. It does not require VapourSynth at runtime. The managed NuGet package is native-free; applications load `lsmasnative` explicitly when they need decoding.

The native build currently targets Windows x64 and links against the FFmpeg `lsmas` fork used by L-SMASH-Works.

## License

This repository is licensed as `GPL-3.0-or-later`. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.

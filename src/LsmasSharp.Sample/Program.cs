using LsmasSharp;
using LsmasSharp.Native;
using System.Security.Cryptography;
using System.Text.Json;

if (args.Length == 0)
{
    Console.Error.WriteLine("Usage:");
    Console.Error.WriteLine("  LsmasSharp.Sample <mediaPath> [cacheDir]");
    Console.Error.WriteLine("  LsmasSharp.Sample --native <nativeDir> <mediaPath> [cacheDir] [options]");
    Console.Error.WriteLine("  LsmasSharp.Sample --native <nativeDir> --versions");
    Console.Error.WriteLine("");
    Console.Error.WriteLine("Options:");
    Console.Error.WriteLine("  --frame <index>                 Single frame index (default: 0)");
    Console.Error.WriteLine("  --frames <list>                 Comma list: 0,mid,last,123 (overrides --frame)");
    Console.Error.WriteLine("  --repeat-open <n>               Open/close loop count (default: 1)");
    Console.Error.WriteLine("  --parallel <n>                  Concurrent handles per open (default: 1)");
    Console.Error.WriteLine("  --random-frames <n>             Add n random frames (+0/mid/last); overrides --frames/--frame");
    Console.Error.WriteLine("  --seed <n>                      RNG seed for --random-frames");
    Console.Error.WriteLine("  --progress                      Print index/open progress callbacks (requires --parallel 1)");
    Console.Error.WriteLine("  --versions                      Print lsmasnative + dependency versions JSON and exit");
    Console.Error.WriteLine("  --seek-mode <Normal|Unsafe|Aggressive>");
    Console.Error.WriteLine("  --seek-threshold <n>");
    Console.Error.WriteLine("  --prefer-hw <None|NvidiaCuvid|IntelQsv|CuvidThenQsv>");
    Console.Error.WriteLine("  --probe                         Print container stream list (tracks) and exit");
    Console.Error.WriteLine("  --video-stream <n>              Select lavf video stream index (-1=auto)");
    Console.Error.WriteLine("  --audio-stream <n>              Select lavf audio stream index (-1=auto)");
    Console.Error.WriteLine("  --cache-per-stream              Use distinct .lwi per (video,audio) stream selection");
    Console.Error.WriteLine("  --fps <num>/<den>               Enable vfr2cfr; 0/1 disables (default: 0/1)");
    Console.Error.WriteLine("  --ff-loglevel <0..8>            Match VS mapping; 0=quiet, 4=warning, 7=debug");
    Console.Error.WriteLine("  --no-repeat                     Disable ApplyRepeat");
    Console.Error.WriteLine("  --pixel-format <native|gray8|gray8padded16|bgra|rgba> Output format for --verify (default: bgra)");
    Console.Error.WriteLine("  --timecodes                     Print PTS/time_base for selected frames");
    Console.Error.WriteLine("  --check-timecodes               Validate PTS list (monotonic/VFR) and print summary");
    Console.Error.WriteLine("  --dump-timecodes-v2 <path>      Write '# timecode format v2' file (ms, normalized)");
    Console.Error.WriteLine("  --dump-frames <dir>             Dump raw frame bytes for selected frames (requires --parallel 1)");
    Console.Error.WriteLine("  --dump-frames-json <path>       Write dump manifest JSON (default: <dir>/frames.json)");
    Console.Error.WriteLine("  --verify                        Compute sha256 of packed32 frame(s)");
    Console.Error.WriteLine("  --props                         Print SAR/color metadata (stream+frame)");
    Console.Error.WriteLine("  --frame-lists                   Print source I/P/B & keyframe lists summary");
    Console.Error.WriteLine("  --hdr                           Print HDR10/CLL/DV stream metadata if present");
    Console.Error.WriteLine("  --seek-only                     Call SeekFrame() for selected frames (no copy)");
    Console.Error.WriteLine("  --flush-before                  Flush() before each SeekFrame()/decode");
    Console.Error.WriteLine("  --audio-only                    Skip video open; requires --audio-*");
    Console.Error.WriteLine("  --audio-info                    Print audio stream info");
    Console.Error.WriteLine("  --audio-verify                  Read audio chunk and sha256");
    Console.Error.WriteLine("  --audio-format <f32|s16|s32>    Output PCM format (default: f32)");
    Console.Error.WriteLine("  --audio-start <n>               Start sample-frame (default: 0)");
    Console.Error.WriteLine("  --audio-samples <n>             Sample-frame count (default: 1s)");
    Console.Error.WriteLine("  --audio-av-sync                 Enable av_sync (A/V gap padding)");
    Console.Error.WriteLine("  --av-sync-check                 Print video t=0 mapping to audio samples for selected frames (requires audio)");
    Console.Error.WriteLine("  --av-sync-audio                 Read per-frame audio block (mapped from video frame start) and sha256");
    Console.Error.WriteLine("  --av-sync-max-samples <n>       Cap per-frame audio read (default: 48000)");
    Console.Error.WriteLine("  --av-time-ms <ms>               Resolve time(ms) -> frame/sample (requires A/V open)");
    Console.Error.WriteLine("  --strict-timeline               Require strict increasing PTS for time mapping (fail-fast)");
    return 2;
}

string? nativeDir = null;
var frameIndex = 0;
string? framesArg = null;
var repeatOpen = 1;
var parallel = 1;
int? randomFrames = null;
int? seed = null;
var verify = false;
var props = false;
var frameLists = false;
var hdr = false;
var seekOnly = false;
var flushBefore = false;
var timecodes = false;
var checkTimecodes = false;
string? dumpTimecodesV2Path = null;
string? dumpFramesDir = null;
string? dumpFramesJsonPath = null;
var probe = false;
int? videoStreamIndex = null;
int? audioStreamIndex = null;
var cachePerStream = false;
var showProgress = false;
var showVersions = false;
var audioOnly = false;
var audioInfo = false;
var audioVerify = false;
var audioFormat = "f32";
long audioStart = 0;
int? audioSamples = null;
bool? audioAvSync = null;
var avSyncCheck = false;
var avSyncAudio = false;
int avSyncMaxSamples = 48000;
long? avTimeMs = null;
var strictTimeline = false;
SeekMode? seekMode = null;
int? seekThreshold = null;
HardwareDecoderPreference? preferHw = null;
int? fpsNum = null;
int? fpsDen = null;
int? ffLogLevel = null;
bool? applyRepeat = null;
var pixelFormat = "bgra";
bool? cacheIndex = null;
var positional = new List<string>();
for (var i = 0; i < args.Length; i++)
{
    var arg = args[i];
    if (arg is "--native" && i + 1 < args.Length)
    {
        nativeDir = args[++i];
        continue;
    }

    if (arg is "--frame" && i + 1 < args.Length)
    {
        frameIndex = int.Parse(args[++i]);
        continue;
    }

    if (arg is "--frames" && i + 1 < args.Length)
    {
        framesArg = args[++i];
        continue;
    }

    if (arg is "--repeat-open" && i + 1 < args.Length)
    {
        repeatOpen = int.Parse(args[++i]);
        continue;
    }

    if (arg is "--parallel" && i + 1 < args.Length)
    {
        parallel = int.Parse(args[++i]);
        continue;
    }

    if (arg is "--random-frames" && i + 1 < args.Length)
    {
        randomFrames = int.Parse(args[++i]);
        continue;
    }

    if (arg is "--probe")
    {
        probe = true;
        continue;
    }

    if (arg is "--video-stream" && i + 1 < args.Length)
    {
        videoStreamIndex = int.Parse(args[++i]);
        continue;
    }

    if (arg is "--audio-stream" && i + 1 < args.Length)
    {
        audioStreamIndex = int.Parse(args[++i]);
        continue;
    }

    if (arg is "--cache-per-stream")
    {
        cachePerStream = true;
        continue;
    }

    if (arg is "--no-cache-index")
    {
        cacheIndex = false;
        continue;
    }

    if (arg is "--cache-index")
    {
        cacheIndex = true;
        continue;
    }

    if (arg is "--progress")
    {
        showProgress = true;
        continue;
    }

    if (arg is "--versions")
    {
        showVersions = true;
        continue;
    }

    if (arg is "--seed" && i + 1 < args.Length)
    {
        seed = int.Parse(args[++i]);
        continue;
    }

    if (arg is "--seek-mode" && i + 1 < args.Length)
    {
        seekMode = Enum.Parse<SeekMode>(args[++i], ignoreCase: true);
        continue;
    }

    if (arg is "--seek-threshold" && i + 1 < args.Length)
    {
        seekThreshold = int.Parse(args[++i]);
        continue;
    }

    if (arg is "--prefer-hw" && i + 1 < args.Length)
    {
        preferHw = Enum.Parse<HardwareDecoderPreference>(args[++i], ignoreCase: true);
        continue;
    }

    if (arg is "--fps" && i + 1 < args.Length)
    {
        var s = args[++i];
        var parts = s.Split('/', 2, StringSplitOptions.TrimEntries);
        if (parts.Length != 2)
            throw new ArgumentException("Invalid --fps, expected <num>/<den>.");
        fpsNum = int.Parse(parts[0]);
        fpsDen = int.Parse(parts[1]);
        continue;
    }

    if (arg is "--ff-loglevel" && i + 1 < args.Length)
    {
        ffLogLevel = int.Parse(args[++i]);
        continue;
    }

    if (arg is "--pixel-format" && i + 1 < args.Length)
    {
        pixelFormat = args[++i].Trim();
        continue;
    }

    if (arg is "--no-repeat")
    {
        applyRepeat = false;
        continue;
    }

    if (arg is "--verify")
    {
        verify = true;
        continue;
    }

    if (arg is "--props")
    {
        props = true;
        continue;
    }

    if (arg is "--frame-lists")
    {
        frameLists = true;
        continue;
    }

    if (arg is "--hdr")
    {
        hdr = true;
        continue;
    }

    if (arg is "--seek-only")
    {
        seekOnly = true;
        continue;
    }

    if (arg is "--flush-before")
    {
        flushBefore = true;
        continue;
    }

    if (arg is "--audio-only")
    {
        audioOnly = true;
        continue;
    }

    if (arg is "--audio-info")
    {
        audioInfo = true;
        continue;
    }

    if (arg is "--audio-verify")
    {
        audioVerify = true;
        continue;
    }

    if (arg is "--audio-format" && i + 1 < args.Length)
    {
        audioFormat = args[++i].Trim();
        continue;
    }

    if (arg is "--audio-start" && i + 1 < args.Length)
    {
        audioStart = long.Parse(args[++i]);
        continue;
    }

    if (arg is "--audio-samples" && i + 1 < args.Length)
    {
        audioSamples = int.Parse(args[++i]);
        continue;
    }

    if (arg is "--audio-av-sync")
    {
        audioAvSync = true;
        continue;
    }

    if (arg is "--av-sync-check")
    {
        avSyncCheck = true;
        continue;
    }

    if (arg is "--av-sync-audio")
    {
        avSyncAudio = true;
        continue;
    }

    if (arg is "--av-sync-max-samples" && i + 1 < args.Length)
    {
        avSyncMaxSamples = int.Parse(args[++i]);
        continue;
    }

    if (arg is "--av-time-ms" && i + 1 < args.Length)
    {
        avTimeMs = long.Parse(args[++i]);
        continue;
    }

    if (arg is "--strict-timeline")
    {
        strictTimeline = true;
        continue;
    }

    if (arg is "--timecodes")
    {
        timecodes = true;
        continue;
    }

    if (arg is "--check-timecodes")
    {
        checkTimecodes = true;
        continue;
    }

    if (arg is "--dump-timecodes-v2" && i + 1 < args.Length)
    {
        dumpTimecodesV2Path = args[++i];
        continue;
    }

    if (arg is "--dump-frames" && i + 1 < args.Length)
    {
        dumpFramesDir = args[++i];
        continue;
    }

    if (arg is "--dump-frames-json" && i + 1 < args.Length)
    {
        dumpFramesJsonPath = args[++i];
        continue;
    }

    positional.Add(arg);
}

if (positional.Count == 0)
{
    if (showVersions)
    {
        if (string.IsNullOrWhiteSpace(nativeDir))
        {
            Console.Error.WriteLine("--versions requires --native <nativeDir>.");
            return 2;
        }

        Console.WriteLine($"native:   {nativeDir}");
        LsmasSharpNativeLoader.Install(nativeDir);
        Console.WriteLine(LsmasSharpRuntimeVersions.GetVersionsJson());
        return 0;
    }

    Console.Error.WriteLine("Missing mediaPath.");
    return 2;
}

var mediaPath = positional[0];
var cacheDir = positional.Count >= 2 ? positional[1] : "";
var pixelFormatNorm = (pixelFormat ?? "bgra").Trim().ToLowerInvariant();
var frameOutputFormat = pixelFormatNorm switch
{
    "native" => LsmasSharpFrameOutputFormat.Native,
    "gray8" => LsmasSharpFrameOutputFormat.Gray8,
    "gray8padded16" => LsmasSharpFrameOutputFormat.Gray8Padded16,
    "bgra" => LsmasSharpFrameOutputFormat.Bgra,
    "rgba" => LsmasSharpFrameOutputFormat.Rgba,
    _ => throw new ArgumentException($"Unsupported --pixel-format: {pixelFormat} (expected: native|gray8|gray8padded16|bgra|rgba).")
};

if ((dumpFramesDir is not null || dumpFramesJsonPath is not null) && parallel != 1)
    throw new ArgumentException("--dump-frames requires --parallel 1.");

if ((dumpFramesDir is not null || dumpFramesJsonPath is not null) && seekOnly)
    throw new ArgumentException("--dump-frames is not compatible with --seek-only.");

var dumpFramesDirFull = dumpFramesDir is null ? null : Path.GetFullPath(dumpFramesDir);
var dumpFramesJsonFull = dumpFramesJsonPath is null ? null : Path.GetFullPath(dumpFramesJsonPath);
if (dumpFramesDirFull is not null && dumpFramesJsonFull is null)
    dumpFramesJsonFull = Path.Combine(dumpFramesDirFull, "frames.json");
if (dumpFramesDirFull is not null)
    Directory.CreateDirectory(dumpFramesDirFull);
if (dumpFramesJsonFull is not null)
    Directory.CreateDirectory(Path.GetDirectoryName(dumpFramesJsonFull)!);

Console.WriteLine($"media:    {mediaPath}");
Console.WriteLine($"cachedir: {cacheDir}");
var indexPath = cachePerStream
    ? LwiPath.CreatePerStream(mediaPath, cacheDir, videoStreamIndex ?? -1, audioStreamIndex ?? -1)
    : LwiPath.Create(mediaPath, cacheDir);
Console.WriteLine($"index:    {indexPath}");
if (LwiIndexReader.TryReadHeader(indexPath, out var header))
    Console.WriteLine($"lwi:      version=0x{header.LwindexVersion:X8} indexFile={header.IndexFileVersion}");

if (!string.IsNullOrWhiteSpace(nativeDir))
{
    Console.WriteLine($"native:   {nativeDir}");
    try
    {
        LsmasSharpNativeLoader.Install(nativeDir);
        if (showVersions)
        {
            Console.WriteLine(LsmasSharpRuntimeVersions.GetVersionsJson());
            return 0;
        }
        if (repeatOpen <= 0)
            throw new ArgumentOutOfRangeException(nameof(repeatOpen));
        if (parallel <= 0)
            throw new ArgumentOutOfRangeException(nameof(parallel));
        if (showProgress && parallel != 1)
            throw new ArgumentException("--progress requires --parallel 1.");
        if ((audioOnly || audioInfo || audioVerify || avSyncCheck || avSyncAudio) && parallel != 1)
            throw new ArgumentException("--audio-* options require --parallel 1.");
        if ((dumpFramesDirFull is not null || dumpFramesJsonFull is not null) && audioOnly)
            throw new ArgumentException("--dump-frames is not compatible with --audio-only.");
        if (audioSamples is <= 0)
            throw new ArgumentOutOfRangeException(nameof(audioSamples));
        if (audioStart < 0)
            throw new ArgumentOutOfRangeException(nameof(audioStart));
        if (dumpTimecodesV2Path is not null && parallel != 1)
            throw new ArgumentException("--dump-timecodes-v2 requires --parallel 1.");
        if (props && parallel != 1)
            throw new ArgumentException("--props requires --parallel 1.");
        if (frameLists && parallel != 1)
            throw new ArgumentException("--frame-lists requires --parallel 1.");
        if (hdr && parallel != 1)
            throw new ArgumentException("--hdr requires --parallel 1.");
        if (randomFrames is <= 0)
            throw new ArgumentOutOfRangeException(nameof(randomFrames));

        var options = new VideoSourceOptions
        {
            CacheIndex = cacheIndex ?? true,
            CacheDir = cacheDir,
        };

        if (videoStreamIndex is { } vsi) options = options with { StreamIndex = vsi };
        if (seekMode is { } sm) options = options with { SeekMode = sm };
        if (seekThreshold is { } st) options = options with { SeekThreshold = st };
        if (preferHw is { } hw) options = options with { PreferHw = hw };
        if (fpsNum is { } n && fpsDen is { } d) options = options with { FpsNum = n, FpsDen = d };
        if (ffLogLevel is { } ll) options = options with { FfLogLevel = ll };
        if (applyRepeat is { } ar) options = options with { ApplyRepeat = ar };

        if (cachePerStream)
        {
            var asi = audioStreamIndex ?? -1;
            options = options with { CacheFile = LwiPath.CreatePerStream(mediaPath, cacheDir, options.StreamIndex, asi) };
        }

        if (probe)
        {
            var mi = LsmasSharpMediaProbe.Probe(mediaPath);
            foreach (var s in mi.Streams)
            {
                var extras = s.Type switch
                {
                    "video" => $" {s.Width}x{s.Height} sar={s.Sar?.Num}/{s.Sar?.Den} fps={s.AvgFrameRate?.Num}/{s.AvgFrameRate?.Den}",
                    "audio" => $" rate={s.SampleRate} ch={s.Channels} layout=0x{(s.ChannelLayout ?? 0):X}",
                    _ => ""
                };
                Console.WriteLine($"stream:   idx={s.Index} type={s.Type} codec={s.Codec} default={s.Default} lang={s.Language}{extras}");
            }
            return 0;
        }

        AudioSampleFormat ParseAudioFormat(string value)
        {
            return value.Trim().ToLowerInvariant() switch
            {
                "f32" or "float" or "float32" => AudioSampleFormat.Float32,
                "s16" or "int16" => AudioSampleFormat.Int16,
                "s32" or "int32" => AudioSampleFormat.Int32,
                _ => throw new ArgumentException($"Unsupported --audio-format: {value} (expected: f32|s16|s32).")
            };
        }

        int[] ParseFrames(LsmasSharpVideoInfo info)
        {
            if (randomFrames is { } rf)
            {
                var rng = seed is { } s ? new Random(s) : Random.Shared;
                var set = new HashSet<int> { 0, info.NumFrames / 2, info.NumFrames - 1 };
                while (set.Count < rf + 3 && set.Count < info.NumFrames)
                {
                    set.Add(rng.Next(0, info.NumFrames));
                }
                return set.OrderBy(x => x).ToArray();
            }

            if (string.IsNullOrWhiteSpace(framesArg))
                return [frameIndex];

            var tokens = framesArg.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            var list = new List<int>(tokens.Length);
            foreach (var token in tokens)
            {
                if (token.Equals("mid", StringComparison.OrdinalIgnoreCase))
                {
                    list.Add(info.NumFrames / 2);
                    continue;
                }

                if (token.Equals("last", StringComparison.OrdinalIgnoreCase))
                {
                    list.Add(info.NumFrames - 1);
                    continue;
                }

                list.Add(int.Parse(token));
            }

            if (list.Count == 0)
                throw new ArgumentException("Empty --frames.", nameof(framesArg));
            return list.ToArray();
        }

        var allDumpedFrames = new List<FrameDump>();

        for (var iter = 0; iter < repeatOpen; iter++)
        {
            if (repeatOpen > 1)
                Console.WriteLine($"open:     {iter + 1}/{repeatOpen}");

            if (parallel == 1)
            {
                if (audioOnly)
                {
                    if (!audioInfo && !audioVerify)
                        throw new ArgumentException("--audio-only requires --audio-info and/or --audio-verify.");

                     var aopt = new AudioSourceOptions
                     {
                         CacheIndex = cacheIndex ?? true,
                         CacheDir = cacheDir,
                         SampleFormat = ParseAudioFormat(audioFormat),
                     };
                    if (audioStreamIndex is { } asi) aopt = aopt with { StreamIndex = asi };
                    if (ffLogLevel is { } logLevel) aopt = aopt with { FfLogLevel = logLevel };
                    if (audioAvSync is { } avs) aopt = aopt with { AvSync = avs };
                    if (cachePerStream) aopt = aopt with { CacheFile = LwiPath.CreateVariant(mediaPath, cacheDir, $"a{aopt.StreamIndex}") };

                    using var a = showProgress
                        ? LsmasSharpAudioSource.Open(mediaPath, aopt, (msg, pct) => { Console.WriteLine($"progress: {pct}% {msg}"); return true; })
                        : LsmasSharpAudioSource.Open(mediaPath, aopt);
                    var ai = a.GetInfo();
                    if (audioInfo)
                        Console.WriteLine($"audio:    rate={ai.SampleRate} ch={ai.Channels} layout=0x{ai.ChannelLayout:X} fmt={ai.SampleFormat} bps={ai.BitsPerSample} align={ai.BlockAlign} samples={ai.TotalSamples} delay={ai.DelaySamples}");

                    if (audioVerify)
                    {
                        var audioFrameCount = audioSamples ?? ai.SampleRate;
                        var bytes = checked((int)((long)audioFrameCount * ai.BlockAlign));
                        var audioBuffer = new byte[bytes];
                        _ = a.CopySamplesRaw(audioStart, audioFrameCount, audioBuffer);
                        var hash = Convert.ToHexStringLower(SHA256.HashData(audioBuffer));
                        Console.WriteLine($"audiochk: ok start={audioStart} samples={audioFrameCount} sha256={hash}");
                    }

                    continue;
                }

                using var source = showProgress
                    ? LsmasSharpVideoSource.Open(mediaPath, options, (msg, pct) => { Console.WriteLine($"progress: {pct}% {msg}"); return true; })
                    : LsmasSharpVideoSource.Open(mediaPath, options);
                var info = source.GetInfo();
                Console.WriteLine($"video:    {info.Width}x{info.Height} frames={info.NumFrames} fps={info.FpsNum}/{info.FpsDen}");
                var tb = timecodes ? source.GetTimeBase() : default;

                if (props)
                {
                    var sp = source.GetStreamProperties();
                    Console.WriteLine($"stream:   sar={sp.SarNum}/{sp.SarDen} range={sp.ColorRange} space={sp.ColorSpace} prim={sp.ColorPrimaries} trc={sp.ColorTransfer} chromaLoc={sp.ChromaLocation} fieldOrder={sp.FieldOrder}");
                }

                if (frameLists)
                {
                    var types = source.GetSourcePictureTypes();
                    var keys = source.GetSourceKeyframeFlags();
                    var srcCount = Math.Min(types.Length, keys.Length);
                    var iCnt = 0;
                    var pCnt = 0;
                    var bCnt = 0;
                    var kCnt = 0;
                    for (var i = 0; i < srcCount; i++)
                    {
                        if (keys[i] != 0) kCnt++;
                        if (types[i] == (sbyte)PictureType.I) iCnt++;
                        else if (types[i] == (sbyte)PictureType.P) pCnt++;
                        else if (types[i] == (sbyte)PictureType.B) bCnt++;
                    }
                    Console.WriteLine($"source:   frames={srcCount} I={iCnt} P={pCnt} B={bCnt} key={kCnt}");
                }

                if (hdr)
                {
                    if (source.TryGetMasteringDisplayMetadata(out var mdm))
                        Console.WriteLine($"hdr:      mastering primaries={mdm.HasPrimaries} luminance={mdm.HasLuminance} maxLum={mdm.MaxLuminance.Num}/{mdm.MaxLuminance.Den} minLum={mdm.MinLuminance.Num}/{mdm.MinLuminance.Den}");
                    else
                        Console.WriteLine("hdr:      mastering (none)");

                    if (source.TryGetContentLightMetadata(out var cll))
                        Console.WriteLine($"hdr:      cll maxCLL={cll.MaxCll} maxFALL={cll.MaxFall}");
                    else
                        Console.WriteLine("hdr:      cll (none)");

                    if (source.TryGetDoviConfiguration(out var dovi))
                        Console.WriteLine($"hdr:      dovi v={dovi.VersionMajor}.{dovi.VersionMinor} profile={dovi.Profile} level={dovi.Level} rpu={dovi.RpuPresent} el={dovi.ElPresent} bl={dovi.BlPresent} compatId={dovi.BlSignalCompatibilityId}");
                    else
                        Console.WriteLine("hdr:      dovi (none)");
                }

                if (checkTimecodes)
                {
                    var timeBase = source.GetTimeBase();
                    var ptsList = source.GetPtsList();
                    if (ptsList.Length != info.NumFrames)
                        throw new InvalidOperationException($"PTS list size mismatch. ptsList={ptsList.Length} info.NumFrames={info.NumFrames}.");

                    var badOrder = 0;
                    var unknown = 0;
                    var nonPositiveDelta = 0;
                    long? minDelta = null;
                    long? maxDelta = null;
                    var distinctDeltas = new HashSet<long>();
                    long? prevKnown = null;
                    for (var i = 0; i < ptsList.Length; i++)
                    {
                        var cur = ptsList[i];
                        if (cur == long.MinValue)
                        {
                            unknown++;
                            continue;
                        }

                        if (prevKnown is { } prev)
                        {
                            if (cur < prev)
                                badOrder++;
                            var delta = cur - prev;
                            if (delta <= 0)
                                nonPositiveDelta++;
                            if (minDelta is null || delta < minDelta) minDelta = delta;
                            if (maxDelta is null || delta > maxDelta) maxDelta = delta;
                            if (distinctDeltas.Count <= 64)
                                distinctDeltas.Add(delta);
                        }

                        prevKnown = cur;
                    }

                    var isVfr = distinctDeltas.Count > 1;
                    Console.WriteLine($"timebase: {timeBase.Num}/{timeBase.Den}");
                    Console.WriteLine($"pts:      unknown={unknown} monotonicErrors={badOrder} nonPositiveDelta={nonPositiveDelta} minDelta={minDelta} maxDelta={maxDelta} distinctDeltaCount~={distinctDeltas.Count} vfr={isVfr}");
                }

                if (dumpTimecodesV2Path is not null && iter == 0)
                {
                    var timeBase = source.GetTimeBase();
                    var ptsList = source.GetPtsList();
                    if (ptsList.Length != info.NumFrames)
                        throw new InvalidOperationException($"PTS list size mismatch. ptsList={ptsList.Length} info.NumFrames={info.NumFrames}.");
                    TimecodesV2.Write(dumpTimecodesV2Path, timeBase, ptsList, info.FpsNum, info.FpsDen, fracDigits: 6);
                    Console.WriteLine($"timecodes: wrote {dumpTimecodesV2Path}");
                }

                var frames = ParseFrames(info);

                foreach (var fi in frames)
                {
                    if (fi < 0 || fi >= info.NumFrames)
                        throw new ArgumentOutOfRangeException(nameof(frameIndex), $"Frame out of range: {fi} not in [0, {info.NumFrames}).");

                    if (seekOnly)
                    {
                        if (flushBefore)
                            source.Flush();
                        source.SeekFrame(fi);
                        Console.WriteLine($"seek:     ok frame={fi}");
                        continue;
                    }

                    var layout = source.GetFrameLayout(fi, frameOutputFormat);
                    var bytesRequired = layout.RequiredBytes;
                    Console.WriteLine($"frame:    {fi} {pixelFormatNorm}Bytes={bytesRequired} planes={layout.PlaneCount} pixfmt={layout.PixelFormat}");

                    if (props)
                    {
                        var fp = source.GetFrameProperties(fi);
                        Console.WriteLine($"props:    sar={fp.SarNum}/{fp.SarDen} range={fp.ColorRange} space={fp.ColorSpace} prim={fp.ColorPrimaries} trc={fp.ColorTransfer} chromaLoc={fp.ChromaLocation} fieldOrder={fp.FieldOrder} interlaced={fp.InterlacedFrame} tff={fp.TopFieldFirst}");
                    }

                    if (timecodes)
                    {
                        var pts = source.GetFramePts(fi);
                        var tc = source.GetFrameTimecode(fi);
                        var tcStr = tc is { } t ? t.ToString() : "(null)";
                        Console.WriteLine($"timecode: frame={fi} pts={pts} tb={tb.Num}/{tb.Den} ts={tcStr}");
                    }

                    var needDump = dumpFramesDirFull is not null || dumpFramesJsonFull is not null;
                    if (verify || needDump)
                    {
                        if (bytesRequired > int.MaxValue)
                            throw new InvalidOperationException("Frame too large for a single managed buffer.");
                        var buffer = new byte[(int)bytesRequired];
                        layout = source.CopyFrame(fi, frameOutputFormat, buffer);

                        var bytesRequiredInt = checked((int)bytesRequired);
                        var span = new ReadOnlySpan<byte>(buffer, 0, bytesRequiredInt);
                        var hash = Convert.ToHexStringLower(SHA256.HashData(span));
                        if (verify)
                            Console.WriteLine($"verify:   ok frame={fi} sha256={hash}");

                        if (needDump)
                        {
                            var rawPath = "";
                            if (dumpFramesDirFull is not null)
                            {
                                var openDir = Path.Combine(dumpFramesDirFull, $"open_{iter + 1:D3}");
                                Directory.CreateDirectory(openDir);
                                rawPath = Path.Combine(openDir, $"frame_{fi:D6}_{pixelFormatNorm}_{info.Width}x{info.Height}.raw");
                                using var fs = new FileStream(rawPath, FileMode.Create, FileAccess.Write, FileShare.Read);
                                fs.Write(buffer, 0, bytesRequiredInt);
                                Console.WriteLine($"dump:     ok frame={fi} sha256={hash} raw={rawPath}");
                            }

                            var pts = source.GetFramePts(fi);
                            var tc = source.GetFrameTimecode(fi);
                            allDumpedFrames.Add(new FrameDump(
                                Open: iter + 1,
                                Frame: fi,
                                PixelFormat: pixelFormatNorm,
                                Width: info.Width,
                                Height: info.Height,
                                Stride: layout.PlaneCount > 0 ? layout.Planes[0].Stride : 0,
                                Bytes: bytesRequired,
                                RawPath: rawPath,
                                Sha256: hash,
                                Pts: pts,
                                Timecode: tc?.ToString()
                            ));
                        }
                    }
                }

                if (dumpFramesJsonFull is not null && iter == repeatOpen - 1)
                {
                    var manifest = new FramesDumpManifest(
                        MediaPath: Path.GetFullPath(mediaPath),
                        NativeDir: string.IsNullOrWhiteSpace(nativeDir) ? "" : Path.GetFullPath(nativeDir),
                        CacheDir: string.IsNullOrWhiteSpace(cacheDir) ? "" : Path.GetFullPath(cacheDir),
                        GeneratedAtUtc: DateTime.UtcNow,
                        RepeatOpen: repeatOpen,
                        Frames: allDumpedFrames
                    );

                    var json = JsonSerializer.Serialize(manifest, new JsonSerializerOptions { WriteIndented = true });
                    File.WriteAllText(dumpFramesJsonFull, json);
                    Console.WriteLine($"dumpjson: ok frames={allDumpedFrames.Count} path={dumpFramesJsonFull}");
                }

                if (audioInfo || audioVerify)
                {
                     var aopt = new AudioSourceOptions
                     {
                         CacheIndex = cacheIndex ?? true,
                         CacheDir = cacheDir,
                         SampleFormat = ParseAudioFormat(audioFormat),
                     };
                    if (ffLogLevel is { } logLevel) aopt = aopt with { FfLogLevel = logLevel };
                    if (audioAvSync is { } avs) aopt = aopt with { AvSync = avs };

                    using var a = showProgress
                        ? LsmasSharpAudioSource.Open(mediaPath, aopt, (msg, pct) => { Console.WriteLine($"progress: {pct}% {msg}"); return true; })
                        : LsmasSharpAudioSource.Open(mediaPath, aopt);
                    var ai = a.GetInfo();
                    if (audioInfo)
                        Console.WriteLine($"audio:    rate={ai.SampleRate} ch={ai.Channels} layout=0x{ai.ChannelLayout:X} fmt={ai.SampleFormat} bps={ai.BitsPerSample} align={ai.BlockAlign} samples={ai.TotalSamples} delay={ai.DelaySamples}");

                    if (audioVerify)
                    {
                        var audioFrameCount = audioSamples ?? ai.SampleRate;
                        var bytes = checked((int)((long)audioFrameCount * ai.BlockAlign));
                        var audioBuffer = new byte[bytes];
                        _ = a.CopySamplesRaw(audioStart, audioFrameCount, audioBuffer);
                        var hash = Convert.ToHexStringLower(SHA256.HashData(audioBuffer));
                        Console.WriteLine($"audiochk: ok start={audioStart} samples={audioFrameCount} sha256={hash}");
                    }
                }

                if (avSyncCheck)
                {
                    try
                    {
                         var aopt = new AudioSourceOptions
                         {
                             CacheIndex = cacheIndex ?? true,
                             CacheDir = cacheDir,
                             SampleFormat = ParseAudioFormat(audioFormat),
                             AvSync = true,
                         };
                        if (audioStreamIndex is { } asi) aopt = aopt with { StreamIndex = asi };
                        if (ffLogLevel is { } logLevel) aopt = aopt with { FfLogLevel = logLevel };
                        if (cachePerStream && !string.IsNullOrWhiteSpace(options.CacheFile))
                            aopt = aopt with { CacheFile = options.CacheFile };

                        using var a = showProgress
                            ? LsmasSharpAudioSource.Open(mediaPath, aopt, (msg, pct) => { Console.WriteLine($"progress: {pct}% {msg}"); return true; })
                            : LsmasSharpAudioSource.Open(mediaPath, aopt);
                        var ai = a.GetInfo();

                        var tb2 = source.GetTimeBase();
                        var pts2 = source.GetPtsList();
                        var idx = AvSync.BuildIndex(tb2, pts2, info.FpsNum, info.FpsDen, ai.SampleRate);
                        long[] ticks0;
                        if (strictTimeline)
                        {
                            if (!TimecodesV2.TryBuildNormalizedTicksStrict(tb2, pts2, out ticks0, out var err))
                                throw new InvalidOperationException($"Strict timeline failed: {err}");
                        }
                        else
                        {
                            ticks0 = TimecodesV2.BuildNormalizedTicks(tb2, pts2, info.FpsNum, info.FpsDen);
                        }

                        var frameList = ParseFrames(info);
                        foreach (var fi in frameList)
                        {
                            var t0 = TimeSpan.FromTicks(ticks0[fi]);
                            var s0 = idx.GetFrameStartSample(fi);
                            Console.WriteLine($"avsync:   frame={fi} t0={t0} sample0={s0} (rate={ai.SampleRate} delay={ai.DelaySamples})");
                        }
                    }
                    catch (Exception ex)
                    {
                        if (strictTimeline)
                            throw;
                        Console.WriteLine($"avsync:   skipped ({ex.GetType().Name}: {ex.Message})");
                    }
                }

                if (avSyncAudio)
                {
                    try
                    {
                         var aopt = new AudioSourceOptions
                         {
                             CacheIndex = cacheIndex ?? true,
                             CacheDir = cacheDir,
                             SampleFormat = ParseAudioFormat(audioFormat),
                             AvSync = true,
                         };
                        if (audioStreamIndex is { } asi) aopt = aopt with { StreamIndex = asi };
                        if (ffLogLevel is { } logLevel) aopt = aopt with { FfLogLevel = logLevel };

                        using var av = showProgress
                            ? SyncedAvSource.Open(mediaPath, options, aopt, strictTimeline: strictTimeline, progress: (msg, pct) => { Console.WriteLine($"progress: {pct}% {msg}"); return true; })
                            : SyncedAvSource.Open(mediaPath, options, aopt, strictTimeline: strictTimeline);
                        var ai = av.Audio.GetInfo();
                        var frameList = ParseFrames(av.Video.GetInfo());
                        foreach (var fi in frameList)
                        {
                            var (start, len) = av.GetAudioRangeForFrame(fi, avSyncMaxSamples);
                            var bytes = checked((int)((long)len * ai.BlockAlign));
                            var buf = bytes == 0 ? Array.Empty<byte>() : new byte[bytes];
                            if (len > 0)
                                _ = av.Audio.CopySamplesRaw(start, len, buf);
                            var hash = Convert.ToHexStringLower(SHA256.HashData(buf));
                            Console.WriteLine($"avaudio:  frame={fi} start={start} samples={len} sha256={hash}");
                        }
                    }
                    catch (Exception ex)
                    {
                        if (strictTimeline)
                            throw;
                        Console.WriteLine($"avaudio:  skipped ({ex.GetType().Name}: {ex.Message})");
                    }
                }

                if (avTimeMs is { } ms)
                {
                    try
                    {
                         var aopt = new AudioSourceOptions
                         {
                             CacheIndex = cacheIndex ?? true,
                             CacheDir = cacheDir,
                             SampleFormat = ParseAudioFormat(audioFormat),
                             AvSync = true,
                         };
                        if (audioStreamIndex is { } asi) aopt = aopt with { StreamIndex = asi };
                        if (ffLogLevel is { } logLevel) aopt = aopt with { FfLogLevel = logLevel };

                        using var av = showProgress
                            ? SyncedAvSource.Open(mediaPath, options, aopt, strictTimeline: strictTimeline, progress: (msg, pct) => { Console.WriteLine($"progress: {pct}% {msg}"); return true; })
                            : SyncedAvSource.Open(mediaPath, options, aopt, strictTimeline: strictTimeline);
                        var t = TimeSpan.FromMilliseconds(ms);
                        var fi = av.FindFrameAtOrBeforeTime(t);
                        var si = av.TimeToSample(t);
                        var (fs, fl) = av.GetAudioRangeForFrame(fi, avSyncMaxSamples);
                        var f0 = av.GetFrameStartTime(fi);
                        var fd = av.GetFrameDuration(fi);
                        Console.WriteLine($"avtime:   t={t} frame={fi} frameStart={f0} frameDur={fd} sample={si} frameAudioStart={fs} frameAudioLen={fl}");
                    }
                    catch (Exception ex)
                    {
                        if (strictTimeline)
                            throw;
                        Console.WriteLine($"avtime:   skipped ({ex.GetType().Name}: {ex.Message})");
                    }
                }
            }
            else
            {
                Task Worker()
                {
                    using var source = showProgress
                        ? LsmasSharpVideoSource.Open(mediaPath, options, (msg, pct) => { Console.WriteLine($"progress: {pct}% {msg}"); return true; })
                        : LsmasSharpVideoSource.Open(mediaPath, options);
                    var info = source.GetInfo();
                    var frames = ParseFrames(info);

                    foreach (var fi in frames)
                    {
                        if (fi < 0 || fi >= info.NumFrames)
                            throw new ArgumentOutOfRangeException(nameof(frameIndex), $"Frame out of range: {fi} not in [0, {info.NumFrames}).");

                        var layout = source.GetFrameLayout(fi, frameOutputFormat);
                        var bytesRequired = layout.RequiredBytes;
                        if (bytesRequired <= 0)
                            throw new InvalidOperationException($"Unexpected {pixelFormatNorm.ToUpperInvariant()} bytes: {bytesRequired}.");

                        if (verify)
                        {
                            if (bytesRequired > int.MaxValue)
                                throw new InvalidOperationException("Frame too large for a single managed buffer.");
                            var buffer = new byte[(int)bytesRequired];
                            source.CopyFrame(fi, frameOutputFormat, buffer);
                            _ = SHA256.HashData(buffer);
                        }
                    }

                    return Task.CompletedTask;
                }

                var tasks = Enumerable.Range(0, parallel).Select(_ => Task.Run(Worker)).ToArray();
                Task.WaitAll(tasks);
                Console.WriteLine($"parallel: ok handles={parallel}");
            }
        }
    }
    catch (Exception ex)
    {
        Console.Error.WriteLine($"native open failed: {ex.Message}");
        return 1;
    }
}
return 0;

sealed record FrameDump(
    int Open,
    int Frame,
    string PixelFormat,
    int Width,
    int Height,
    int Stride,
    long Bytes,
    string RawPath,
    string Sha256,
    long Pts,
    string? Timecode
);

sealed record FramesDumpManifest(
    string MediaPath,
    string NativeDir,
    string CacheDir,
    DateTime GeneratedAtUtc,
    int RepeatOpen,
    List<FrameDump> Frames
);

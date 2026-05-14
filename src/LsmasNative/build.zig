const std = @import("std");

pub fn build(b: *std.Build) void {
    const io = b.graph.io;
    const path_exists = struct {
        fn check(build_io: std.Io, path: []const u8) bool {
            std.Io.Dir.cwd().access(build_io, path, .{}) catch return false;
            return true;
        }
    }.check;

    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const l_smash_works_dir = b.option([]const u8, "l_smash_works_dir", "L-SMASH-WORKS repo directory (must contain common/)") orelse "";
    if (l_smash_works_dir.len == 0) {
        std.log.err("Missing L-SMASH-WORKS directory. Pass -Dl_smash_works_dir=...", .{});
        return;
    }
    const lsw_common_dir = b.pathJoin(&.{ l_smash_works_dir, "common" });

    const ffmpeg_prefix = b.option([]const u8, "ffmpeg_prefix", "FFmpeg prefix containing include/ and lib/") orelse "";
    if (ffmpeg_prefix.len == 0) {
        std.log.err("Missing FFmpeg prefix. Pass -Dffmpeg_prefix=...", .{});
        return;
    }

    const linkage_opt = b.option([]const u8, "linkage", "Build linkage: dynamic|static (default: dynamic)") orelse "dynamic";
    const linkage: std.builtin.LinkMode = if (std.ascii.eqlIgnoreCase(linkage_opt, "static")) .static else .dynamic;

    const root_module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    const lib = b.addLibrary(.{
        .name = "lsmasnative",
        .root_module = root_module,
        .linkage = linkage,
    });

    lib.root_module.addIncludePath(b.path("."));
    lib.root_module.addIncludePath(.{ .cwd_relative = lsw_common_dir });
    lib.root_module.addIncludePath(.{ .cwd_relative = b.pathJoin(&.{ ffmpeg_prefix, "include" }) });

    lib.root_module.addLibraryPath(.{ .cwd_relative = b.pathJoin(&.{ ffmpeg_prefix, "lib" }) });

    lib.root_module.addCMacro("_FILE_OFFSET_BITS", "64");

    const legacy_xxhash_c = b.pathJoin(&.{ lsw_common_dir, "xxhash.c" });
    const hoe_lwindex_utils_c = b.pathJoin(&.{ lsw_common_dir, "lwindex_utils.c" });

    const is_legacy_lsw = path_exists(io, legacy_xxhash_c);
    const is_hoe_lsw = path_exists(io, hoe_lwindex_utils_c);

    if (is_hoe_lsw and !is_legacy_lsw) {
        lib.root_module.addCMacro("LSMAS_LSW_VARIANT_HOE", "1");
        // HOE's common/ expects <xxhash.h> and does not ship xxhash.{c,h} inside common/.
        lib.root_module.addIncludePath(.{ .cwd_relative = b.pathJoin(&.{ l_smash_works_dir, "xxHash" }) });
    } else {
        lib.root_module.addCMacro("LSMAS_LSW_VARIANT_HOE", "0");
    }

    const lsmasnative_version = b.option([]const u8, "lsmasnative_version", "lsmasnative version string") orelse "0.0.0-dev";
    lib.root_module.addCMacro("LSMAS_NATIVE_VERSION", b.fmt("\"{s}\"", .{ lsmasnative_version }));

    const lsmasnative_zig_version = b.option([]const u8, "lsmasnative_zig_version", "zig version string used for lsmasnative build") orelse "unknown";
    lib.root_module.addCMacro("LSMAS_NATIVE_ZIG_VERSION", b.fmt("\"{s}\"", .{ lsmasnative_zig_version }));

    const lsmasnative_git_head = b.option([]const u8, "lsmasnative_git_head", "lsmasnative git head (short)") orelse "unknown";
    lib.root_module.addCMacro("LSMAS_NATIVE_GIT_HEAD", b.fmt("\"{s}\"", .{ lsmasnative_git_head }));

    const lsmashworks_git_url = b.option([]const u8, "lsmashworks_git_url", "L-SMASH-WORKS git remote URL") orelse "unknown";
    lib.root_module.addCMacro("LSMASHWORKS_GIT_URL", b.fmt("\"{s}\"", .{ lsmashworks_git_url }));

    const lsmashworks_git_head = b.option([]const u8, "lsmashworks_git_head", "L-SMASH-WORKS git head (short)") orelse "unknown";
    lib.root_module.addCMacro("LSMASHWORKS_GIT_HEAD", b.fmt("\"{s}\"", .{ lsmashworks_git_head }));

    const lsmashworks_git_branch = b.option([]const u8, "lsmashworks_git_branch", "L-SMASH-WORKS git branch/ref name") orelse "unknown";
    lib.root_module.addCMacro("LSMASHWORKS_GIT_BRANCH", b.fmt("\"{s}\"", .{ lsmashworks_git_branch }));

    const ffmpeg_git_url = b.option([]const u8, "ffmpeg_git_url", "FFmpeg(lsmas fork) git remote URL") orelse "unknown";
    lib.root_module.addCMacro("FFMPEG_GIT_URL", b.fmt("\"{s}\"", .{ ffmpeg_git_url }));

    const ffmpeg_git_head = b.option([]const u8, "ffmpeg_git_head", "FFmpeg(lsmas fork) git head (short)") orelse "unknown";
    lib.root_module.addCMacro("FFMPEG_GIT_HEAD", b.fmt("\"{s}\"", .{ ffmpeg_git_head }));

    const ffmpeg_git_branch = b.option([]const u8, "ffmpeg_git_branch", "FFmpeg(lsmas fork) git branch/ref name") orelse "unknown";
    lib.root_module.addCMacro("FFMPEG_GIT_BRANCH", b.fmt("\"{s}\"", .{ ffmpeg_git_branch }));

    const dav1d_version = b.option([]const u8, "dav1d_version", "dav1d version string (optional)") orelse "unknown";
    lib.root_module.addCMacro("DAV1D_VERSION", b.fmt("\"{s}\"", .{ dav1d_version }));

    const zlib_version = b.option([]const u8, "zlib_version", "zlib version string (optional)") orelse "unknown";
    lib.root_module.addCMacro("ZLIB_VERSION_STR", b.fmt("\"{s}\"", .{ zlib_version }));

    if (target.result.os.tag == .windows) {
        lib.root_module.addCMacro("_CRT_SECURE_NO_WARNINGS", "1");
        lib.root_module.addCMacro("LSMAS_NATIVE_EXPORTS", "1");
    }

    var sources = std.ArrayList([]const u8).empty;
    defer sources.deinit(b.allocator);

    sources.append(b.allocator, "lsmas_native.c") catch @panic("OOM");

    const common_sources = [_][]const u8{
        "utils.c",
        "osdep.c",
        "decode.c",
        "lwlibav_dec.c",
        "lwlibav_video.c",
        "lwlibav_audio.c",
        "lwindex.c",
        "video_output.c",
        "audio_output.c",
        "resample.c",
    };
    inline for (common_sources) |name| {
        sources.append(b.allocator, b.pathJoin(&.{ lsw_common_dir, name })) catch @panic("OOM");
    }
    const optional_common_sources = [_][]const u8{
        "qsv.c",
    };
    inline for (optional_common_sources) |name| {
        const p = b.pathJoin(&.{ lsw_common_dir, name });
        if (path_exists(io, p)) {
            sources.append(b.allocator, p) catch @panic("OOM");
        }
    }

    if (is_legacy_lsw) {
        // Legacy common/ ships xxhash.{c,h} and xxh3.h.
        sources.append(b.allocator, legacy_xxhash_c) catch @panic("OOM");
    } else if (is_hoe_lsw) {
        // HOE common/ splits index helpers into separate translation units.
        const hoe_extra = [_][]const u8{
            "lwindex_parser.c",
            "lwindex_utils.c",
            "planar_yuv_sse2.c",
        };
        inline for (hoe_extra) |name| {
            const p = b.pathJoin(&.{ lsw_common_dir, name });
            if (path_exists(io, p)) {
                sources.append(b.allocator, p) catch @panic("OOM");
            }
        }
        // Provide xxhash implementation expected by <xxhash.h> calls.
        const xxhash_impl = b.pathJoin(&.{ l_smash_works_dir, "xxHash", "xxhash.c" });
        if (path_exists(io, xxhash_impl)) {
            sources.append(b.allocator, xxhash_impl) catch @panic("OOM");
        } else {
            std.log.err("HOE L-SMASH-Works detected, but missing xxHash/xxhash.c under: {s}", .{l_smash_works_dir});
            @panic("Missing xxHash/xxhash.c for HOE variant.");
        }
    } else {
        std.log.err("Unable to detect L-SMASH-Works common/ variant under: {s}", .{l_smash_works_dir});
        std.log.err("Expected either common/xxhash.c (legacy) or common/lwindex_utils.c (HOE).", .{});
        @panic("Unknown L-SMASH-Works common/ variant.");
    }

    lib.root_module.addCSourceFiles(.{ .files = sources.items, .flags = &.{ "-DXXH_INLINE_ALL", "-Wno-deprecated-declarations" } });

    const prefer_static_ffmpeg = b.option(bool, "ffmpeg_static", "Prefer linking FFmpeg libraries statically into lsmasnative.dll") orelse true;
    if (prefer_static_ffmpeg) {
        lib.root_module.linkSystemLibrary("avcodec", .{ .preferred_link_mode = .static });
        lib.root_module.linkSystemLibrary("avformat", .{ .preferred_link_mode = .static });
        lib.root_module.linkSystemLibrary("avutil", .{ .preferred_link_mode = .static });
        lib.root_module.linkSystemLibrary("swscale", .{ .preferred_link_mode = .static });
        lib.root_module.linkSystemLibrary("swresample", .{ .preferred_link_mode = .static });
        // libavcodec may depend on external decoders (e.g. dav1d) when enabled in FFmpeg.
        lib.root_module.linkSystemLibrary("dav1d", .{ .preferred_link_mode = .static });
        // libav* may depend on zlib when enabled in FFmpeg.
        lib.root_module.linkSystemLibrary("z", .{ .preferred_link_mode = .static });
    } else {
        lib.root_module.linkSystemLibrary("avcodec", .{});
        lib.root_module.linkSystemLibrary("avformat", .{});
        lib.root_module.linkSystemLibrary("avutil", .{});
        lib.root_module.linkSystemLibrary("swscale", .{});
        lib.root_module.linkSystemLibrary("swresample", .{});
        lib.root_module.linkSystemLibrary("dav1d", .{});
        lib.root_module.linkSystemLibrary("z", .{});
    }

    // Optional dependency: l-smash (liblsmash.a). HOE/LSW may use it for MP4/qt indexing.
    const lsmash_static = b.pathJoin(&.{ ffmpeg_prefix, "lib", "liblsmash.a" });
    if (path_exists(io, lsmash_static)) {
        lib.root_module.linkSystemLibrary("lsmash", .{ .preferred_link_mode = .static });
    }

    if (target.result.os.tag == .windows) {
        lib.root_module.linkSystemLibrary("bcrypt", .{});
        lib.root_module.linkSystemLibrary("crypt32", .{});
        lib.root_module.linkSystemLibrary("ncrypt", .{});
        lib.root_module.linkSystemLibrary("ws2_32", .{});
        lib.root_module.linkSystemLibrary("secur32", .{});
        lib.root_module.linkSystemLibrary("ole32", .{});
        lib.root_module.linkSystemLibrary("user32", .{});
        lib.root_module.linkSystemLibrary("shell32", .{});
        lib.root_module.linkSystemLibrary("advapi32", .{});
    } else if (target.result.os.tag == .linux) {
        lib.root_module.linkSystemLibrary("pthread", .{});
        lib.root_module.linkSystemLibrary("dl", .{});
        lib.root_module.linkSystemLibrary("m", .{});
    } else if (target.result.os.tag == .macos) {
        lib.root_module.linkSystemLibrary("pthread", .{});
    }

    b.installArtifact(lib);
}

using System.Runtime.InteropServices;

namespace LsmasSharp.Native;

internal static class LsmasSharpNativeMethods
{
    public const string LibraryName = "lsmasnative";

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int ProgressCallback(nint userdata, nint message_utf8, int percent);

    [StructLayout(LayoutKind.Sequential)]
    public struct VideoOpenOptions
    {
        public int stream_index;
        public int threads;
        public int seek_mode;
        public int seek_threshold;
        public int direct_rendering;

        public int fpsnum;
        public int fpsden;

        public int variable_info;
        public nint output_format;
        public nint decoder;
        public int prefer_hw;

        public int ff_loglevel;

        public int cache_index;
        public nint cachefile;
        public nint cachedir;

        public int soft_reset;
        public int framelist;

        public int repeat;
        public int dominance;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct AudioOpenOptions
    {
        public int stream_index;
        public int threads;
        public int av_sync;

        public int ff_loglevel;
        public nint decoder;

        public int cache_index;
        public nint cachefile;
        public nint cachedir;

        public ulong channel_layout;
        public int sample_rate;
        public int sample_format;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct VideoInfo
    {
        public int width;
        public int height;
        public int num_frames;
        public int fps_num;
        public int fps_den;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct AudioInfo
    {
        public int stream_index;

        public int sample_rate;
        public int channels;
        public ulong channel_layout;

        public int sample_format;
        public int bits_per_sample;
        public int bytes_per_sample;
        public int block_align;

        public long decoded_samples;
        public long delay_samples;
        public long total_samples;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct VideoProps
    {
        public int sar_num;
        public int sar_den;

        public int color_range;
        public int colorspace;
        public int color_primaries;
        public int color_trc;
        public int chroma_location;
        public int field_order;

        public int interlaced_frame;
        public int top_field_first;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Rational32
    {
        public int num;
        public int den;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MasteringDisplayMetadata
    {
        public int has_primaries;
        public int has_luminance;

        public Rational32 primary_r_x;
        public Rational32 primary_r_y;
        public Rational32 primary_g_x;
        public Rational32 primary_g_y;
        public Rational32 primary_b_x;
        public Rational32 primary_b_y;

        public Rational32 white_x;
        public Rational32 white_y;

        public Rational32 max_luminance;
        public Rational32 min_luminance;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ContentLightMetadata
    {
        public int max_cll;
        public int max_fall;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct DoviConf
    {
        public int dv_version_major;
        public int dv_version_minor;
        public int dv_profile;
        public int dv_level;
        public int rpu_present_flag;
        public int el_present_flag;
        public int bl_present_flag;
        public int dv_bl_signal_compatibility_id;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct VideoFrameBufferLayout
    {
        public int output_format;
        public int width;
        public int height;
        public int pix_fmt;
        public int plane_count;
        public long required_bytes;

        public int plane_width0;
        public int plane_width1;
        public int plane_width2;
        public int plane_width3;

        public int plane_height0;
        public int plane_height1;
        public int plane_height2;
        public int plane_height3;

        public int plane_stride0;
        public int plane_stride1;
        public int plane_stride2;
        public int plane_stride3;

        public long plane_offset0;
        public long plane_offset1;
        public long plane_offset2;
        public long plane_offset3;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct VideoFrameProps
    {
        public int width;
        public int height;
        public int pix_fmt;
        public int plane_count;

        public int sar_num;
        public int sar_den;

        public int color_range;
        public int colorspace;
        public int color_primaries;
        public int color_trc;
        public int chroma_location;
        public int field_order;

        public int interlaced_frame;
        public int top_field_first;
        public int repeat_pict;

        public int crop_left;
        public int crop_top;
        public int crop_right;
        public int crop_bottom;

        public int display_rotation_degrees;
        public int display_hflip;
        public int display_vflip;

        public int has_mastering_display_metadata;
        public int has_content_light_metadata;
        public int has_dynamic_hdr_plus;
        public int has_dovi_metadata;
        public int has_dovi_rpu;
        public int has_film_grain_params;
        public int has_displaymatrix;
    }

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern nint lsmas_video_open_utf8(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string file_path_utf8,
        nint options,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern nint lsmas_video_open_with_progress_utf8(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string file_path_utf8,
        nint options,
        ProgressCallback progress_cb,
        nint progress_userdata,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern nint lsmas_audio_open_utf8(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string file_path_utf8,
        nint options,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern nint lsmas_audio_open_with_progress_utf8(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string file_path_utf8,
        nint options,
        ProgressCallback progress_cb,
        nint progress_userdata,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern nint lsmas_av_open_utf8(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string file_path_utf8,
        nint video_options,
        nint audio_options,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern nint lsmas_av_open_with_progress_utf8(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string file_path_utf8,
        nint video_options,
        nint audio_options,
        ProgressCallback progress_cb,
        nint progress_userdata,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern nint lsmas_probe_streams_json_utf8(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string file_path_utf8,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern nint lsmas_get_versions_json_utf8(
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_get_api_version();

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern void lsmas_video_close(nint handle);

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern void lsmas_audio_close(nint handle);

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern void lsmas_av_close(nint handle);

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_get_info(
        nint handle,
        out VideoInfo out_info,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_audio_get_info(
        nint handle,
        out AudioInfo out_info,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_get_stream_props(
        nint handle,
        out VideoProps out_props,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_get_stream_mastering_display_metadata(
        nint handle,
        out MasteringDisplayMetadata out_metadata,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_get_stream_content_light_metadata(
        nint handle,
        out ContentLightMetadata out_metadata,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_get_stream_dovi_conf(
        nint handle,
        out DoviConf out_conf,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_seek_frame(
        nint handle,
        int frame_index,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_flush(
        nint handle,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_get_frame_props(
        nint handle,
        int frame_index,
        out VideoProps out_props,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_get_time_base(
        nint handle,
        out int out_num,
        out int out_den,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern long lsmas_video_get_frame_pts(
        nint handle,
        int frame_index,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern long lsmas_video_get_frame(
        nint handle,
        int frame_index,
        int output_format,
        nint dst,
        int dst_stride,
        out VideoFrameBufferLayout out_layout,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_get_pts_list(
        nint handle,
        nint out_pts,
        int out_count,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_get_source_frame_count(
        nint handle,
        out int out_count,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_get_source_pict_type_list(
        nint handle,
        nint out_types,
        int out_count,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_get_source_keyframe_flags(
        nint handle,
        nint out_flags,
        int out_count,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_acquire_avframe(
        nint handle,
        int frame_index,
        out nint out_frame,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern nint lsmas_video_frame_get_avframe(nint frame);

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern nint lsmas_video_frame_get_pix_fmt_name(nint frame);

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_frame_get_props(
        nint frame,
        out VideoFrameProps out_props,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lsmas_video_frame_get_plane(
        nint frame,
        int plane_index,
        out nint out_data,
        out int out_stride,
        out int out_width,
        out int out_height,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern void lsmas_video_release_frame(nint frame);

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern long lsmas_audio_get_samples(
        nint handle,
        nint dst,
        long start,
        long wanted_length,
        out nint error_message
    );

    [DllImport(LibraryName, ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
    public static extern void lsmas_free(nint p);
}

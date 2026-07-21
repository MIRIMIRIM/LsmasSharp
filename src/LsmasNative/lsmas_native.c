#include "lsmas_native.h"

/* L-SMASH-Works common headers (provided via build include path) */
#include "cpp_compat.h"

/* FFmpeg headers (lsmas branch) */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/log.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(56, 61, 100)
#include <libavutil/film_grain_params.h>
#endif
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(56, 25, 100)
#include <libavutil/hdr_dynamic_metadata.h>
#endif
#if LSMAS_LSW_VARIANT_HOE
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavcodec/packet.h>
#endif

#include "utils.h"
#include "progress.h"
#include "video_output.h"
#include "audio_output.h"
#include "resample.h"
#include "lwlibav_dec.h"
#include "lwlibav_video.h"
#include "lwlibav_video_internal.h"
#include "lwlibav_audio.h"
#include "lwlibav_audio_internal.h"
#include "lwindex.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define PREFERRED_DECODER_NAMES_BUFSIZE 512

/* common/lwlibav_audio.c defines this but does not declare it in the public header. */
void lwlibav_audio_set_log_handler( lwlibav_audio_decode_handler_t *adhp, lw_log_handler_t *lh );

#if defined(_WIN32) && !LSMAS_LSW_VARIANT_HOE
static int lockmgr_cb( void **mutex, enum AVLockOp op )
{
    if( !mutex )
        return 1;

    switch( op )
    {
        case AV_LOCK_CREATE:
        {
            CRITICAL_SECTION *cs = (CRITICAL_SECTION *)malloc( sizeof(CRITICAL_SECTION) );
            if( !cs )
                return 1;
            InitializeCriticalSection( cs );
            *mutex = cs;
            return 0;
        }
        case AV_LOCK_OBTAIN:
            EnterCriticalSection( (CRITICAL_SECTION *)(*mutex) );
            return 0;
        case AV_LOCK_RELEASE:
            LeaveCriticalSection( (CRITICAL_SECTION *)(*mutex) );
            return 0;
        case AV_LOCK_DESTROY:
        {
            CRITICAL_SECTION *cs = (CRITICAL_SECTION *)(*mutex);
            if( cs )
            {
                DeleteCriticalSection( cs );
                free( cs );
            }
            *mutex = NULL;
            return 0;
        }
        default:
            return 1;
    }
}

static INIT_ONCE g_ffmpeg_init_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK ffmpeg_init_once( PINIT_ONCE once, PVOID param, PVOID *ctx )
{
    (void)once;
    (void)param;
    (void)ctx;

    /* Older FFmpeg requires av_lockmgr_register to be thread-safe across instances. */
    (void)av_lockmgr_register( lockmgr_cb );
    return TRUE;
}

static void ensure_ffmpeg_threadsafe( void )
{
    InitOnceExecuteOnce( &g_ffmpeg_init_once, ffmpeg_init_once, NULL, NULL );
}
#elif defined(_WIN32)
static void ensure_ffmpeg_threadsafe( void ) { }
#else
static void ensure_ffmpeg_threadsafe( void ) { }
#endif

#ifdef _WIN32
static INIT_ONCE g_open_lock_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_open_lock;
static BOOL CALLBACK open_lock_init_once( PINIT_ONCE once, PVOID param, PVOID *ctx )
{
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection( &g_open_lock );
    return TRUE;
}
static void open_lock_enter( void )
{
    InitOnceExecuteOnce( &g_open_lock_once, open_lock_init_once, NULL, NULL );
    EnterCriticalSection( &g_open_lock );
}
static void open_lock_leave( void )
{
    LeaveCriticalSection( &g_open_lock );
}
#else
static void open_lock_enter( void ) { }
static void open_lock_leave( void ) { }
#endif

struct lsmas_handle_t
{
    lwlibav_file_handler_t lwh;
    lwlibav_video_decode_handler_t *vdhp;
    lwlibav_video_output_handler_t *vohp;
    lwlibav_audio_decode_handler_t *adhp;
    lwlibav_audio_output_handler_t *aohp;
    lw_video_scaler_handler_t rgba_scaler;
    int rgba_scaler_inited;
    lw_video_scaler_handler_t gray8_scaler;
    int gray8_scaler_inited;
    lw_video_scaler_handler_t yuv420p8_scaler;
    int yuv420p8_scaler_inited;
    char preferred_decoder_names_buf[PREFERRED_DECODER_NAMES_BUFSIZE]; /* video */
    const char **preferred_decoder_names;
    int prefer_hw_decoder; /* HOE variant expects an int* with stable lifetime */
    char preferred_audio_decoder_names_buf[PREFERRED_DECODER_NAMES_BUFSIZE];
    const char **preferred_audio_decoder_names;
    lsmas_video_info_t info;
    lsmas_audio_info_t audio_info;
    int audio_info_valid;
    char *last_error;
#ifdef _WIN32
    CRITICAL_SECTION error_lock;
    int error_lock_inited;
#endif
};

struct lsmas_video_frame_t
{
    AVFrame *frame;
};

static int trace_enabled( void )
{
    const char *v = getenv( "LSMAS_NATIVE_TRACE" );
    return v && v[0] && v[0] != '0';
}

/* Some inputs advertise a tail frame that is not actually decodable.
 * We probe and clamp the tail during open by default for compatibility, but it can
 * trigger decoder edge cases on some sources.
 * Workaround (opt-out): set LSMAS_NATIVE_CLAMP_TAIL=0. */
static int clamp_tail_enabled( void )
{
    const char *v = getenv( "LSMAS_NATIVE_CLAMP_TAIL" );
    if( !v || !v[0] )
        return 1;
    /* Simple opt-out parsing to avoid locale/strcasecmp portability issues. */
    return !(v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N');
}

#define TRACE( ... )                                 \
    do                                               \
    {                                                \
        if( trace_enabled() )                        \
        {                                            \
            fprintf( stderr, __VA_ARGS__ );           \
            fputc( '\n', stderr );                    \
            fflush( stderr );                         \
        }                                            \
    } while( 0 )

static void set_last_error( lsmas_handle_t *h, const char *message )
{
    if( !h )
        return;
#ifdef _WIN32
    if( h->error_lock_inited )
        EnterCriticalSection( &h->error_lock );
#endif
    if( h->last_error )
        free( h->last_error );
    h->last_error = NULL;
    if( !message )
    {
#ifdef _WIN32
        if( h->error_lock_inited )
            LeaveCriticalSection( &h->error_lock );
#endif
        return;
    }
    size_t n = strlen( message );
    h->last_error = (char *)malloc( n + 1 );
    if( !h->last_error )
    {
#ifdef _WIN32
        if( h->error_lock_inited )
            LeaveCriticalSection( &h->error_lock );
#endif
        return;
    }
    memcpy( h->last_error, message, n );
    h->last_error[n] = '\0';
#ifdef _WIN32
    if( h->error_lock_inited )
        LeaveCriticalSection( &h->error_lock );
#endif
}

static void on_log( lw_log_handler_t *lhp, lw_log_level level, const char *message )
{
    (void)level;
    lsmas_handle_t *h = (lsmas_handle_t *)lhp->priv;
    if( trace_enabled() )
        TRACE( "lwlog: %s", message ? message : "(null)" );
    set_last_error( h, message );
}

static void set_error_message( char **error_message, const char *message )
{
    if( !error_message )
        return;
    *error_message = NULL;
    if( !message )
        return;
    size_t n = strlen( message );
    char *copy = (char *)malloc( n + 1 );
    if( !copy )
        return;
    memcpy( copy, message, n );
    copy[n] = '\0';
    *error_message = copy;
}

typedef struct sbuf_t
{
    char   *ptr;
    size_t  len;
    size_t  cap;
} sbuf_t;

#define APPEND_LIT( B, LIT ) sbuf_append_bytes( (B), (LIT), sizeof(LIT) - 1 )

static int sbuf_reserve( sbuf_t *b, size_t add )
{
    if( !b )
        return -1;
    size_t need = b->len + add + 1; /* +NUL */
    if( need <= b->cap )
        return 0;
    size_t new_cap = b->cap ? b->cap : 256;
    while( new_cap < need )
        new_cap *= 2;
    char *p = (char *)realloc( b->ptr, new_cap );
    if( !p )
        return -1;
    b->ptr = p;
    b->cap = new_cap;
    return 0;
}

static int sbuf_append_bytes( sbuf_t *b, const char *s, size_t n )
{
    if( !b || (!s && n != 0) )
        return -1;
    if( sbuf_reserve( b, n ) != 0 )
        return -1;
    if( n != 0 )
        memcpy( b->ptr + b->len, s, n );
    b->len += n;
    b->ptr[b->len] = '\0';
    return 0;
}

static int sbuf_append_cstr( sbuf_t *b, const char *s )
{
    if( !s )
        return APPEND_LIT( b, "null" );
    return sbuf_append_bytes( b, s, strlen(s) );
}

static int sbuf_append_u32( sbuf_t *b, uint32_t v )
{
    char tmp[32];
    int n = snprintf( tmp, sizeof(tmp), "%u", v );
    if( n <= 0 )
        return -1;
    return sbuf_append_bytes( b, tmp, (size_t)n );
}

static int sbuf_append_i64( sbuf_t *b, int64_t v )
{
    char tmp[64];
    int n = snprintf( tmp, sizeof(tmp), "%lld", (long long)v );
    if( n <= 0 )
        return -1;
    return sbuf_append_bytes( b, tmp, (size_t)n );
}

static int sbuf_append_u64( sbuf_t *b, uint64_t v )
{
    char tmp[64];
    int n = snprintf( tmp, sizeof(tmp), "%llu", (unsigned long long)v );
    if( n <= 0 )
        return -1;
    return sbuf_append_bytes( b, tmp, (size_t)n );
}

static int sbuf_append_json_escaped( sbuf_t *b, const char *s )
{
    if( sbuf_append_bytes( b, "\"", 1 ) != 0 )
        return -1;
    if( s )
    {
        for( const unsigned char *p = (const unsigned char *)s; *p; p++ )
        {
            unsigned char c = *p;
            switch( c )
            {
                case '\\': if( sbuf_append_bytes( b, "\\\\", 2 ) != 0 ) return -1; break;
                case '\"': if( sbuf_append_bytes( b, "\\\"", 2 ) != 0 ) return -1; break;
                case '\b': if( sbuf_append_bytes( b, "\\b", 2 ) != 0 ) return -1; break;
                case '\f': if( sbuf_append_bytes( b, "\\f", 2 ) != 0 ) return -1; break;
                case '\n': if( sbuf_append_bytes( b, "\\n", 2 ) != 0 ) return -1; break;
                case '\r': if( sbuf_append_bytes( b, "\\r", 2 ) != 0 ) return -1; break;
                case '\t': if( sbuf_append_bytes( b, "\\t", 2 ) != 0 ) return -1; break;
                default:
                {
                    if( c < 0x20 )
                    {
                        char tmp[8];
                        int n = snprintf( tmp, sizeof(tmp), "\\u%04x", (unsigned)c );
                        if( n <= 0 || sbuf_append_bytes( b, tmp, (size_t)n ) != 0 )
                            return -1;
                    }
                    else
                    {
                        if( sbuf_append_bytes( b, (const char *)&c, 1 ) != 0 )
                            return -1;
                    }
                    break;
                }
            }
        }
    }
    if( sbuf_append_bytes( b, "\"", 1 ) != 0 )
        return -1;
    return 0;
}

static void set_av_error_message( char **error_message, int err, const char *prefix )
{
    if( !error_message )
        return;
    char buf[256];
    buf[0] = '\0';
    av_strerror( err, buf, sizeof(buf) );
    if( prefix && prefix[0] )
    {
        char msg[512];
        snprintf( msg, sizeof(msg), "%s: %s", prefix, buf[0] ? buf : "unknown error" );
        set_error_message( error_message, msg );
    }
    else
    {
        set_error_message( error_message, buf[0] ? buf : "unknown error" );
    }
}

/* Progress callback bridging (lwindex progress_indicator_t -> user callback).
   Implemented via TLS so we can add open_with_progress without duplicating the open body. */
#if defined(_MSC_VER)
  #define LSMAS_TLS __declspec(thread)
#else
  #define LSMAS_TLS _Thread_local
#endif

static LSMAS_TLS lsmas_progress_callback_t g_tls_progress_cb = NULL;
static LSMAS_TLS void *g_tls_progress_userdata = NULL;

typedef struct lsmas_progress_handler_t
{
    lsmas_progress_callback_t cb;
    void                    *userdata;
} lsmas_progress_handler_t;

static void lsmas_indicator_open( progress_handler_t *hp )
{
    lsmas_progress_handler_t *p = (lsmas_progress_handler_t *)hp;
    if( p && p->cb )
        (void)p->cb( p->userdata, "indexing", 0 );
}

static int lsmas_indicator_update( progress_handler_t *hp, const char *message, int percent )
{
    lsmas_progress_handler_t *p = (lsmas_progress_handler_t *)hp;
    if( !p || !p->cb )
        return 0;
    return p->cb( p->userdata, message ? message : "", (int32_t)percent );
}

static void lsmas_indicator_close( progress_handler_t *hp )
{
    lsmas_progress_handler_t *p = (lsmas_progress_handler_t *)hp;
    if( p && p->cb )
        (void)p->cb( p->userdata, "done", 100 );
}

static void init_progress_indicator(
    progress_indicator_t *indicator,
    progress_handler_t **out_php,
    lsmas_progress_handler_t *php,
    lsmas_progress_callback_t cb,
    void *userdata
)
{
    if( indicator )
        memset( indicator, 0, sizeof(*indicator) );
    if( out_php )
        *out_php = NULL;
    if( !indicator || !out_php || !php || !cb )
        return;

    php->cb = cb;
    php->userdata = userdata;

    indicator->open   = lsmas_indicator_open;
    indicator->update = lsmas_indicator_update;
    indicator->close  = lsmas_indicator_close;
    *out_php = (progress_handler_t *)php;
}

/* Version reporting */
#ifndef LSMAS_NATIVE_VERSION
  #define LSMAS_NATIVE_VERSION "0.0.0-dev"
#endif

#ifndef LSMAS_NATIVE_ZIG_VERSION
  #define LSMAS_NATIVE_ZIG_VERSION "unknown"
#endif

#ifndef LSMAS_NATIVE_GIT_HEAD
  #define LSMAS_NATIVE_GIT_HEAD "unknown"
#endif

#ifndef LSMASHWORKS_GIT_URL
  #define LSMASHWORKS_GIT_URL "unknown"
#endif

#ifndef LSMASHWORKS_GIT_HEAD
  #define LSMASHWORKS_GIT_HEAD "unknown"
#endif

#ifndef LSMASHWORKS_GIT_BRANCH
  #define LSMASHWORKS_GIT_BRANCH "unknown"
#endif

#ifndef FFMPEG_GIT_URL
  #define FFMPEG_GIT_URL "unknown"
#endif

#ifndef FFMPEG_GIT_HEAD
  #define FFMPEG_GIT_HEAD "unknown"
#endif

#ifndef FFMPEG_GIT_BRANCH
  #define FFMPEG_GIT_BRANCH "unknown"
#endif

#ifndef DAV1D_VERSION
  #define DAV1D_VERSION "unknown"
#endif

#ifndef ZLIB_VERSION_STR
  #define ZLIB_VERSION_STR "unknown"
#endif

static int sbuf_append_version_triplet_from_u32( sbuf_t *b, uint32_t v )
{
    uint32_t major = (v >> 16) & 0xff;
    uint32_t minor = (v >> 8) & 0xff;
    uint32_t micro = v & 0xff;
    char tmp[32];
    int n = snprintf( tmp, sizeof(tmp), "%u.%u.%u", major, minor, micro );
    if( n <= 0 )
        return -1;
    return sbuf_append_bytes( b, tmp, (size_t)n );
}

LSMAS_NATIVE_API int32_t lsmas_get_api_version( void )
{
    return (int32_t)LSMAS_NATIVE_API_VERSION;
}

LSMAS_NATIVE_API char *lsmas_get_versions_json_utf8( char **error_message )
{
    if( error_message )
        *error_message = NULL;

    sbuf_t b = { 0 };
    if( APPEND_LIT( &b, "{" ) != 0 )
        goto oom;

    if( APPEND_LIT( &b, "\"lsmasnative\":{" ) != 0 )
        goto oom;
    if( APPEND_LIT( &b, "\"version\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, LSMAS_NATIVE_VERSION ) != 0 )
        goto oom;

    if( APPEND_LIT( &b, ",\"api_version\":" ) != 0 )
        goto oom;
    if( sbuf_append_i64( &b, (int64_t)lsmas_get_api_version() ) != 0 )
        goto oom;

    if( APPEND_LIT( &b, ",\"api_version_string\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, LSMAS_NATIVE_API_VERSION_STRING ) != 0 )
        goto oom;

    if( APPEND_LIT( &b, ",\"zig_version\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, LSMAS_NATIVE_ZIG_VERSION ) != 0 )
        goto oom;

    if( APPEND_LIT( &b, ",\"c_compiler_version\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, __VERSION__ ) != 0 )
        goto oom;

    if( APPEND_LIT( &b, ",\"git_head\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, LSMAS_NATIVE_GIT_HEAD ) != 0 )
        goto oom;

#if defined(_WIN32)
    if( APPEND_LIT( &b, ",\"platform\":\"windows\"" ) != 0 )
        goto oom;
#else
    if( APPEND_LIT( &b, ",\"platform\":\"non-windows\"" ) != 0 )
        goto oom;
#endif

    if( APPEND_LIT( &b, "}," ) != 0 )
        goto oom;

    if( APPEND_LIT( &b, "\"l_smash_works\":{" ) != 0 )
        goto oom;
    if( APPEND_LIT( &b, "\"git_url\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, LSMASHWORKS_GIT_URL ) != 0 )
        goto oom;
    if( APPEND_LIT( &b, ",\"git_head\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, LSMASHWORKS_GIT_HEAD ) != 0 )
        goto oom;
    if( APPEND_LIT( &b, ",\"git_branch\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, LSMASHWORKS_GIT_BRANCH ) != 0 )
        goto oom;
    if( APPEND_LIT( &b, "}," ) != 0 )
        goto oom;

    if( APPEND_LIT( &b, "\"ffmpeg\":{" ) != 0 )
        goto oom;

    if( APPEND_LIT( &b, "\"git_url\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, FFMPEG_GIT_URL ) != 0 )
        goto oom;
    if( APPEND_LIT( &b, ",\"git_head\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, FFMPEG_GIT_HEAD ) != 0 )
        goto oom;
    if( APPEND_LIT( &b, ",\"git_branch\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, FFMPEG_GIT_BRANCH ) != 0 )
        goto oom;

    if( APPEND_LIT( &b, ",\"av_version_info\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, av_version_info() ) != 0 )
        goto oom;

    if( APPEND_LIT( &b, ",\"license\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, avcodec_license() ) != 0 )
        goto oom;

    if( APPEND_LIT( &b, ",\"configuration\":" ) != 0 )
        goto oom;
    if( sbuf_append_json_escaped( &b, avcodec_configuration() ) != 0 )
        goto oom;

    /* External lib versions (best-effort, build-time pinned) */
    if( APPEND_LIT( &b, ",\"dav1d_version\":" ) != 0 ) goto oom;
    if( sbuf_append_json_escaped( &b, DAV1D_VERSION ) != 0 ) goto oom;

    if( APPEND_LIT( &b, ",\"zlib_version\":" ) != 0 ) goto oom;
    if( sbuf_append_json_escaped( &b, ZLIB_VERSION_STR ) != 0 ) goto oom;

    /* Library versions */
    if( APPEND_LIT( &b, ",\"libavutil\":\"" ) != 0 ) goto oom;
    if( sbuf_append_version_triplet_from_u32( &b, (uint32_t)avutil_version() ) != 0 ) goto oom;
    if( APPEND_LIT( &b, "\"" ) != 0 ) goto oom;

    if( APPEND_LIT( &b, ",\"libavcodec\":\"" ) != 0 ) goto oom;
    if( sbuf_append_version_triplet_from_u32( &b, (uint32_t)avcodec_version() ) != 0 ) goto oom;
    if( APPEND_LIT( &b, "\"" ) != 0 ) goto oom;

    if( APPEND_LIT( &b, ",\"libavformat\":\"" ) != 0 ) goto oom;
    if( sbuf_append_version_triplet_from_u32( &b, (uint32_t)avformat_version() ) != 0 ) goto oom;
    if( APPEND_LIT( &b, "\"" ) != 0 ) goto oom;

    if( APPEND_LIT( &b, ",\"libswscale\":\"" ) != 0 ) goto oom;
    if( sbuf_append_version_triplet_from_u32( &b, (uint32_t)swscale_version() ) != 0 ) goto oom;
    if( APPEND_LIT( &b, "\"" ) != 0 ) goto oom;

    if( APPEND_LIT( &b, ",\"libswresample\":\"" ) != 0 ) goto oom;
    if( sbuf_append_version_triplet_from_u32( &b, (uint32_t)swresample_version() ) != 0 ) goto oom;
    if( APPEND_LIT( &b, "\"" ) != 0 ) goto oom;

    if( APPEND_LIT( &b, "}}" ) != 0 )
        goto oom;

    return b.ptr;

oom:
    free( b.ptr );
    set_error_message( error_message, "Out of memory." );
    return NULL;
}

LSMAS_NATIVE_API char *lsmas_probe_streams_json_utf8( const char *file_path_utf8, char **error_message )
{
    if( error_message )
        *error_message = NULL;
    if( !file_path_utf8 || !file_path_utf8[0] )
    {
        set_error_message( error_message, "file_path_utf8 is null/empty." );
        return NULL;
    }

    AVFormatContext *fmt = NULL;
    int ret = avformat_open_input( &fmt, file_path_utf8, NULL, NULL );
    if( ret < 0 )
    {
        set_av_error_message( error_message, ret, "avformat_open_input failed" );
        return NULL;
    }

    /* Best-effort; probing should still return what avformat already knows. */
    (void)avformat_find_stream_info( fmt, NULL );

    sbuf_t b = { 0 };
    if( APPEND_LIT( &b, "{" ) != 0 )
        goto oom;

    if( APPEND_LIT( &b, "\"streams\":[" ) != 0 )
        goto oom;

    for( unsigned i = 0; i < (unsigned)fmt->nb_streams; i++ )
    {
        AVStream *st = fmt->streams[i];
        if( !st || !st->codecpar )
            continue;

        if( b.len && b.ptr && b.ptr[b.len - 1] != '[' )
        {
            if( APPEND_LIT( &b, "," ) != 0 )
                goto oom;
        }

        AVCodecParameters *par = st->codecpar;
        const char *type = "unknown";
        if( par->codec_type == AVMEDIA_TYPE_VIDEO ) type = "video";
        else if( par->codec_type == AVMEDIA_TYPE_AUDIO ) type = "audio";
        else if( par->codec_type == AVMEDIA_TYPE_SUBTITLE ) type = "subtitle";
        else if( par->codec_type == AVMEDIA_TYPE_DATA ) type = "data";
        else if( par->codec_type == AVMEDIA_TYPE_ATTACHMENT ) type = "attachment";

        const char *codec = avcodec_get_name( par->codec_id );
        AVDictionaryEntry *lang = av_dict_get( st->metadata, "language", NULL, 0 );
        int is_default = (st->disposition & AV_DISPOSITION_DEFAULT) ? 1 : 0;

        if( APPEND_LIT( &b, "{" ) != 0 ) goto oom;

        if( APPEND_LIT( &b, "\"index\":" ) != 0 ) goto oom;
        if( sbuf_append_u32( &b, (uint32_t)st->index ) != 0 ) goto oom;

        if( APPEND_LIT( &b, ",\"type\":" ) != 0 ) goto oom;
        if( sbuf_append_json_escaped( &b, type ) != 0 ) goto oom;

        if( APPEND_LIT( &b, ",\"codec\":" ) != 0 ) goto oom;
        if( sbuf_append_json_escaped( &b, codec ? codec : "unknown" ) != 0 ) goto oom;

        if( APPEND_LIT( &b, ",\"default\":" ) != 0 ) goto oom;
        if( is_default )
        {
            if( APPEND_LIT( &b, "true" ) != 0 ) goto oom;
        }
        else
        {
            if( APPEND_LIT( &b, "false" ) != 0 ) goto oom;
        }

        if( lang && lang->value && lang->value[0] )
        {
            if( APPEND_LIT( &b, ",\"language\":" ) != 0 ) goto oom;
            if( sbuf_append_json_escaped( &b, lang->value ) != 0 ) goto oom;
        }

        if( st->time_base.den != 0 )
        {
            if( APPEND_LIT( &b, ",\"time_base\":{\"num\":" ) != 0 ) goto oom;
            if( sbuf_append_i64( &b, (int64_t)st->time_base.num ) != 0 ) goto oom;
            if( APPEND_LIT( &b, ",\"den\":" ) != 0 ) goto oom;
            if( sbuf_append_i64( &b, (int64_t)st->time_base.den ) != 0 ) goto oom;
            if( APPEND_LIT( &b, "}" ) != 0 ) goto oom;
        }

        if( st->start_time != AV_NOPTS_VALUE )
        {
            if( APPEND_LIT( &b, ",\"start_time\":" ) != 0 ) goto oom;
            if( sbuf_append_i64( &b, (int64_t)st->start_time ) != 0 ) goto oom;
        }
        if( st->duration != AV_NOPTS_VALUE )
        {
            if( APPEND_LIT( &b, ",\"duration\":" ) != 0 ) goto oom;
            if( sbuf_append_i64( &b, (int64_t)st->duration ) != 0 ) goto oom;
        }

        if( par->codec_type == AVMEDIA_TYPE_VIDEO )
        {
            if( APPEND_LIT( &b, ",\"width\":" ) != 0 ) goto oom;
            if( sbuf_append_i64( &b, (int64_t)par->width ) != 0 ) goto oom;
            if( APPEND_LIT( &b, ",\"height\":" ) != 0 ) goto oom;
            if( sbuf_append_i64( &b, (int64_t)par->height ) != 0 ) goto oom;

            AVRational sar = st->sample_aspect_ratio;
            if( sar.num > 0 && sar.den > 0 )
            {
                if( APPEND_LIT( &b, ",\"sar\":{\"num\":" ) != 0 ) goto oom;
                if( sbuf_append_i64( &b, (int64_t)sar.num ) != 0 ) goto oom;
                if( APPEND_LIT( &b, ",\"den\":" ) != 0 ) goto oom;
                if( sbuf_append_i64( &b, (int64_t)sar.den ) != 0 ) goto oom;
                if( APPEND_LIT( &b, "}" ) != 0 ) goto oom;
            }

            AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
            if( fr.num > 0 && fr.den > 0 )
            {
                if( APPEND_LIT( &b, ",\"avg_frame_rate\":{\"num\":" ) != 0 ) goto oom;
                if( sbuf_append_i64( &b, (int64_t)fr.num ) != 0 ) goto oom;
                if( APPEND_LIT( &b, ",\"den\":" ) != 0 ) goto oom;
                if( sbuf_append_i64( &b, (int64_t)fr.den ) != 0 ) goto oom;
                if( APPEND_LIT( &b, "}" ) != 0 ) goto oom;
            }
        }
        else if( par->codec_type == AVMEDIA_TYPE_AUDIO )
        {
            if( APPEND_LIT( &b, ",\"sample_rate\":" ) != 0 ) goto oom;
            if( sbuf_append_i64( &b, (int64_t)par->sample_rate ) != 0 ) goto oom;
            if( APPEND_LIT( &b, ",\"channels\":" ) != 0 ) goto oom;

#if LSMAS_LSW_VARIANT_HOE
            if( sbuf_append_i64( &b, (int64_t)par->ch_layout.nb_channels ) != 0 ) goto oom;
            if( APPEND_LIT( &b, ",\"channel_layout\":" ) != 0 ) goto oom;
            {
                uint64_t mask = 0;
                if( par->ch_layout.order == AV_CHANNEL_ORDER_NATIVE )
                    mask = (uint64_t)par->ch_layout.u.mask;
                if( sbuf_append_u64( &b, mask ) != 0 ) goto oom;
            }
#else
            if( sbuf_append_i64( &b, (int64_t)par->channels ) != 0 ) goto oom;
            if( APPEND_LIT( &b, ",\"channel_layout\":" ) != 0 ) goto oom;
            if( sbuf_append_u64( &b, (uint64_t)par->channel_layout ) != 0 ) goto oom;
#endif
        }

        if( APPEND_LIT( &b, "}" ) != 0 ) goto oom;
    }

    if( APPEND_LIT( &b, "]}" ) != 0 )
        goto oom;

    avformat_close_input( &fmt );
    return b.ptr;

oom:
    if( fmt )
        avformat_close_input( &fmt );
    free( b.ptr );
    set_error_message( error_message, "Out of memory." );
    return NULL;
}

static int update_info( lsmas_handle_t *h, const lsmas_video_open_options_t *opt )
{
    int64_t fps_num = 25;
    int64_t fps_den = 1;
    lwlibav_video_setup_timestamp_info( &h->lwh, h->vdhp, h->vohp, &fps_num, &fps_den, opt->repeat );
    h->info.width      = h->vohp->output_width;
    h->info.height     = h->vohp->output_height;

    uint32_t num_frames = h->vohp->frame_count;
    TRACE(
        "update_info: vohp.frame_count=%u frame_order_count=%u repeat_control=%d vfr2cfr=%d vdhp.frame_count=%u",
        (unsigned)h->vohp->frame_count,
        (unsigned)h->vohp->frame_order_count,
        h->vohp->repeat_control,
        h->vohp->vfr2cfr,
        (unsigned)(h->vdhp ? h->vdhp->frame_count : 0) );
    /* If output is not transformed (no repeat control and no VFR->CFR), prefer vdhp->frame_count.
     * Some FFmpeg versions can make vohp->frame_count drift by +1 for certain containers. */
    if( !h->vohp->repeat_control && !h->vohp->vfr2cfr && h->vdhp && h->vdhp->frame_count > 0 )
    {
        if( h->vdhp->frame_count < num_frames )
            num_frames = h->vdhp->frame_count;
    }
    /* Some sources report a tail frame that is not actually decodable.
       Probing can trigger decoder edge cases, so keep it opt-in. */
    if( clamp_tail_enabled() && num_frames > 0 )
    {
        /* lwlibav is 1-based (we pass frame_index+1). Probe tail picture numbers and clamp accordingly. */
        uint32_t cand = num_frames;
        int clamped = 0;
        for( int attempt = 0; attempt < 3 && cand > 0; attempt++ )
        {
            /* Force the seek-based path so the probe matches typical random access behavior. */
            lwlibav_video_force_seek( h->vdhp );
            int ret = lwlibav_video_get_frame( h->vdhp, h->vohp, cand );
            TRACE( "clamp_tail probe: cand=%u ret=%d", (unsigned)cand, ret );
            if( ret >= 0 )
            {
                lwlibav_video_force_seek( h->vdhp );
                num_frames = cand;
                clamped = 1;
                break;
            }
            lwlibav_video_force_seek( h->vdhp );
            if( cand > 0 )
                cand -= 1;
        }
        if( !clamped )
        {
            /* If multiple tail frames fail, clamp to the last candidate we tried. */
            num_frames = cand;
            TRACE( "clamp_tail: clamp to %u frames", (unsigned)num_frames );
        }
    }
    h->info.num_frames = (int32_t)num_frames;
    h->info.fps_num    = (int32_t)fps_num;
    h->info.fps_den    = (int32_t)fps_den;
    return 0;
}

static int choose_best_stream_index_utf8( const char *file_path_utf8, enum AVMediaType type, int *out_stream_index )
{
    if( !out_stream_index )
        return -1;
    *out_stream_index = -1;

    if( !file_path_utf8 || !file_path_utf8[0] )
        return -1;

    AVFormatContext *fmt = NULL;
    int ret = avformat_open_input( &fmt, file_path_utf8, NULL, NULL );
    if( ret < 0 || !fmt )
    {
        if( fmt )
            avformat_close_input( &fmt );
        return -1;
    }

    (void)avformat_find_stream_info( fmt, NULL );

    int best = -1;
    int64_t best_score = INT64_MIN;
    for( unsigned i = 0; i < (unsigned)fmt->nb_streams; i++ )
    {
        AVStream *st = fmt->streams[i];
        if( !st || !st->codecpar )
            continue;
        if( st->codecpar->codec_type != type )
            continue;

        int is_default = (st->disposition & AV_DISPOSITION_DEFAULT) ? 1 : 0;
        int64_t score = 0;

        if( type == AVMEDIA_TYPE_VIDEO )
        {
            int64_t w = st->codecpar->width > 0 ? st->codecpar->width : 0;
            int64_t h = st->codecpar->height > 0 ? st->codecpar->height : 0;
            score = w * h;
            if( is_default )
                score += (int64_t)1e12;
        }
        else if( type == AVMEDIA_TYPE_AUDIO )
        {
#if LSMAS_LSW_VARIANT_HOE
            int64_t ch = st->codecpar->ch_layout.nb_channels > 0 ? st->codecpar->ch_layout.nb_channels : 0;
#else
            int64_t ch = st->codecpar->channels > 0 ? st->codecpar->channels : 0;
#endif
            int64_t sr = st->codecpar->sample_rate > 0 ? st->codecpar->sample_rate : 0;
            score = ch * 100000 + sr;
            if( is_default )
                score += (int64_t)1e12;
        }
        else
        {
            score = is_default ? 1 : 0;
        }

        if( score > best_score )
        {
            best_score = score;
            best = (int)st->index;
        }
    }

    avformat_close_input( &fmt );

    if( best < 0 )
        return -1;

    *out_stream_index = best;
    return 0;
}

static AVStream *get_selected_stream( lsmas_handle_t *h, char **error_message )
{
    if( !h || !h->vdhp || !h->vdhp->format || h->vdhp->stream_index < 0 )
    {
        set_error_message( error_message, "Invalid internal state (no stream selected)." );
        return NULL;
    }
    if( h->vdhp->stream_index >= h->vdhp->format->nb_streams )
    {
        set_error_message( error_message, "Invalid internal state (stream_index out of range)." );
        return NULL;
    }
    AVStream *st = h->vdhp->format->streams[ h->vdhp->stream_index ];
    if( !st )
    {
        set_error_message( error_message, "Invalid internal state (stream is NULL)." );
        return NULL;
    }
    return st;
}

static int is_valid_rational( AVRational r )
{
    return r.num > 0 && r.den > 0;
}

static void fill_stream_props( lsmas_video_props_t *out_props, AVStream *st )
{
    memset( out_props, 0, sizeof(*out_props) );
    out_props->sar_num = 0;
    out_props->sar_den = 1;
    out_props->interlaced_frame = -1;
    out_props->top_field_first  = -1;

    if( !st )
        return;

    AVCodecParameters *par = st->codecpar;

    AVRational sar = st->sample_aspect_ratio;
    if( !is_valid_rational( sar ) && par )
        sar = par->sample_aspect_ratio;
    if( is_valid_rational( sar ) )
    {
        out_props->sar_num = sar.num;
        out_props->sar_den = sar.den;
    }

    if( par )
    {
        out_props->color_range     = (int32_t)par->color_range;
        out_props->colorspace      = (int32_t)par->color_space;
        out_props->color_primaries = (int32_t)par->color_primaries;
        out_props->color_trc       = (int32_t)par->color_trc;
        out_props->chroma_location = (int32_t)par->chroma_location;
        out_props->field_order     = (int32_t)par->field_order;
    }
}

static lsmas_rational32_t to_rational32( AVRational r )
{
    lsmas_rational32_t o;
    o.num = (int32_t)r.num;
    o.den = (int32_t)r.den;
    return o;
}

static float rational_to_float( AVRational r )
{
    return r.den ? (float)r.num / (float)r.den : 0.0f;
}

#if LSMAS_LSW_VARIANT_HOE
static int dovi_range_contains( size_t total, size_t offset, size_t size )
{
    return offset <= total && total - offset >= size;
}

static void fill_dovi_reshape_component(
    lsmas_dovi_reshape_component_t *dst,
    const AVDOVIReshapingCurve *src,
    const AVDOVIRpuDataHeader *header )
{
    if( !dst || !src || !header )
        return;

    memset( dst, 0, sizeof(*dst) );

    uint8_t num_pivots = src->num_pivots;
    if( num_pivots > 9 )
        num_pivots = 9;
    dst->num_pivots = num_pivots;

    int bl_bit_depth = header->bl_bit_depth > 0 ? header->bl_bit_depth : 12;
    if( bl_bit_depth > 30 )
        bl_bit_depth = 12;
    float pivot_scale = 1.0f / (float)((1u << bl_bit_depth) - 1u);
    for( int i = 0; i < num_pivots; i++ )
        dst->pivots[i] = pivot_scale * (float)src->pivots[i];

    int coef_log2_denom = header->coef_log2_denom;
    if( coef_log2_denom < 0 || coef_log2_denom > 30 )
        coef_log2_denom = 0;
    float coeff_scale = 1.0f / (float)(1u << coef_log2_denom);
    int pieces = num_pivots > 0 ? num_pivots - 1 : 0;
    if( pieces > 8 )
        pieces = 8;

    for( int i = 0; i < pieces; i++ )
    {
        dst->method[i] = (uint8_t)src->mapping_idc[i];
        switch( src->mapping_idc[i] )
        {
            case AV_DOVI_MAPPING_POLYNOMIAL:
                for( int k = 0; k < 3; k++ )
                    dst->poly_coeffs[i][k] = k <= src->poly_order[i]
                        ? coeff_scale * (float)src->poly_coef[i][k]
                        : 0.0f;
                break;
            case AV_DOVI_MAPPING_MMR:
            {
                uint8_t mmr_order = src->mmr_order[i];
                if( mmr_order > 3 )
                    mmr_order = 3;
                dst->mmr_order[i] = mmr_order;
                dst->mmr_constant[i] = coeff_scale * (float)src->mmr_constant[i];
                for( int j = 0; j < mmr_order; j++ )
                {
                    for( int k = 0; k < 7; k++ )
                        dst->mmr_coeffs[i][j][k] = coeff_scale * (float)src->mmr_coef[i][j][k];
                }
                break;
            }
            default:
                break;
        }
    }
}
#endif

static int div_round_up_i32( int value, int divisor )
{
    if( divisor <= 1 )
        return value;
    if( value <= 0 )
        return 0;
    return (value + divisor - 1) / divisor;
}

static int side_data_type_to_avframe_type( lsmas_video_frame_side_data_type_t type, enum AVFrameSideDataType *out_type )
{
    if( !out_type )
        return 0;

    switch( type )
    {
        case LSMAS_FRAME_SIDE_DATA_MASTERING_DISPLAY_METADATA:
            *out_type = AV_FRAME_DATA_MASTERING_DISPLAY_METADATA;
            return 1;
        case LSMAS_FRAME_SIDE_DATA_CONTENT_LIGHT_METADATA:
            *out_type = AV_FRAME_DATA_CONTENT_LIGHT_LEVEL;
            return 1;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(56, 25, 100)
        case LSMAS_FRAME_SIDE_DATA_DYNAMIC_HDR_PLUS:
            *out_type = AV_FRAME_DATA_DYNAMIC_HDR_PLUS;
            return 1;
#endif
#if LSMAS_LSW_VARIANT_HOE
        case LSMAS_FRAME_SIDE_DATA_DOVI_METADATA:
            *out_type = AV_FRAME_DATA_DOVI_METADATA;
            return 1;
        case LSMAS_FRAME_SIDE_DATA_DOVI_RPU:
            *out_type = AV_FRAME_DATA_DOVI_RPU_BUFFER;
            return 1;
#endif
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(56, 61, 100)
        case LSMAS_FRAME_SIDE_DATA_FILM_GRAIN_PARAMS:
            *out_type = AV_FRAME_DATA_FILM_GRAIN_PARAMS;
            return 1;
#endif
        case LSMAS_FRAME_SIDE_DATA_DISPLAYMATRIX:
            *out_type = AV_FRAME_DATA_DISPLAYMATRIX;
            return 1;
        default:
            return 0;
    }
}

static int frame_has_side_data( const AVFrame *frame, lsmas_video_frame_side_data_type_t type )
{
    enum AVFrameSideDataType av_type;
    if( !frame || !side_data_type_to_avframe_type( type, &av_type ) )
        return 0;
    return av_frame_get_side_data( frame, av_type ) != NULL;
}

static void fill_frame_display_matrix_props( const AVFrame *frame, lsmas_video_frame_props_t *out_props )
{
    if( !frame || !out_props )
        return;

    AVFrameSideData *sd = av_frame_get_side_data( frame, AV_FRAME_DATA_DISPLAYMATRIX );
    if( !sd || !sd->data || sd->size < 9 * (int)sizeof(int32_t) )
        return;

    out_props->has_displaymatrix = 1;
    double rotation = av_display_rotation_get( (const int32_t *)sd->data );
    if( rotation > -360000.0 && rotation < 360000.0 )
    {
        int rounded = (int)(rotation >= 0.0 ? rotation + 0.5 : rotation - 0.5);
        out_props->display_rotation_degrees = rounded;
    }
}

static int fill_video_format_info_from_pix_fmt( enum AVPixelFormat pix_fmt, int width, int height, lsmas_video_format_info_t *out_info )
{
    if( !out_info )
        return 0;
    memset( out_info, 0, sizeof(*out_info) );

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get( pix_fmt );
    if( !desc )
        return 0;

    int plane_count = av_pix_fmt_count_planes( pix_fmt );
    if( plane_count <= 0 || plane_count > 4 )
        return 0;

    out_info->plane_count = plane_count;
    out_info->color_family = (desc->flags & AV_PIX_FMT_FLAG_RGB)
        ? LSMAS_COLOR_FAMILY_RGB
        : LSMAS_COLOR_FAMILY_YCBCR;

    for( int p = 0; p < plane_count; p++ )
    {
        lsmas_video_plane_format_t *plane = &out_info->planes[p];
        memset( plane, 0, sizeof(*plane) );
        plane->width_divisor = 1;
        plane->height_divisor = 1;
        plane->bytes_per_sample = 0;
        plane->bits_per_component = 0;

        int components = 0;
        int max_component_step = 0;
        for( int c = 0; c < desc->nb_components && c < 4; c++ )
        {
            const AVComponentDescriptor *comp = &desc->comp[c];
            if( comp->plane != p )
                continue;
            if( components < 4 )
                plane->component_shift[components] = comp->shift + comp->offset * 8;
            components++;
            if( comp->step > max_component_step )
                max_component_step = comp->step;
            if( comp->depth > plane->bits_per_component )
                plane->bits_per_component = comp->depth;
        }

        if( components <= 0 )
            components = 1;
        plane->components_per_sample = components;
        plane->bytes_per_sample = max_component_step > 0 ? max_component_step : 1;
        if( plane->bits_per_component <= 0 )
            plane->bits_per_component = 8;
    }

    if( plane_count == 1 )
        return 1;

    for( int p = 1; p < plane_count; p++ )
    {
        int plane_w = AV_CEIL_RSHIFT( width, desc->log2_chroma_w );
        int plane_h = AV_CEIL_RSHIFT( height, desc->log2_chroma_h );
        out_info->planes[p].width_divisor = plane_w > 0 ? div_round_up_i32( width, plane_w ) : 1;
        out_info->planes[p].height_divisor = plane_h > 0 ? div_round_up_i32( height, plane_h ) : 1;
    }

    return 1;
}

static void fill_frame_props_from_avframe( const AVFrame *frame, lsmas_video_frame_props_t *out_props )
{
    memset( out_props, 0, sizeof(*out_props) );
    out_props->sar_den = 1;
    out_props->pix_fmt = AV_PIX_FMT_NONE;
    out_props->interlaced_frame = -1;
    out_props->top_field_first = -1;

    if( !frame )
        return;

    out_props->width = frame->width;
    out_props->height = frame->height;
    out_props->pix_fmt = frame->format;
    out_props->plane_count = av_pix_fmt_count_planes( (enum AVPixelFormat)frame->format );
    if( out_props->plane_count < 0 )
        out_props->plane_count = 0;
    if( out_props->plane_count > 4 )
        out_props->plane_count = 4;

    AVRational sar = frame->sample_aspect_ratio;
    if( is_valid_rational( sar ) )
    {
        out_props->sar_num = sar.num;
        out_props->sar_den = sar.den;
    }

    out_props->color_range     = (int32_t)frame->color_range;
    out_props->colorspace      = (int32_t)frame->colorspace;
    out_props->color_primaries = (int32_t)frame->color_primaries;
    out_props->color_trc       = (int32_t)frame->color_trc;
    out_props->chroma_location = (int32_t)frame->chroma_location;
    out_props->field_order     = -1;
#if LSMAS_LSW_VARIANT_HOE
    out_props->interlaced_frame = (int32_t)((frame->flags & AV_FRAME_FLAG_INTERLACED) != 0);
    out_props->top_field_first  = (int32_t)((frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0);
#else
    out_props->interlaced_frame = (int32_t)frame->interlaced_frame;
    out_props->top_field_first  = (int32_t)frame->top_field_first;
#endif
    out_props->repeat_pict = frame->repeat_pict;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(56, 60, 100)
    out_props->crop_left   = (int32_t)MIN( frame->crop_left,   (size_t)INT_MAX );
    out_props->crop_top    = (int32_t)MIN( frame->crop_top,    (size_t)INT_MAX );
    out_props->crop_right  = (int32_t)MIN( frame->crop_right,  (size_t)INT_MAX );
    out_props->crop_bottom = (int32_t)MIN( frame->crop_bottom, (size_t)INT_MAX );
#endif

    fill_frame_display_matrix_props( frame, out_props );

    out_props->has_mastering_display_metadata = frame_has_side_data( frame, LSMAS_FRAME_SIDE_DATA_MASTERING_DISPLAY_METADATA );
    out_props->has_content_light_metadata     = frame_has_side_data( frame, LSMAS_FRAME_SIDE_DATA_CONTENT_LIGHT_METADATA );
    out_props->has_dynamic_hdr_plus           = frame_has_side_data( frame, LSMAS_FRAME_SIDE_DATA_DYNAMIC_HDR_PLUS );
    out_props->has_dovi_metadata              = frame_has_side_data( frame, LSMAS_FRAME_SIDE_DATA_DOVI_METADATA );
    out_props->has_dovi_rpu                   = frame_has_side_data( frame, LSMAS_FRAME_SIDE_DATA_DOVI_RPU );
    out_props->has_film_grain_params          = frame_has_side_data( frame, LSMAS_FRAME_SIDE_DATA_FILM_GRAIN_PARAMS );
}

static void init_rgba_scaler_if_needed( lsmas_handle_t *h )
{
    if( !h || h->rgba_scaler_inited )
        return;

    memset( &h->rgba_scaler, 0, sizeof(h->rgba_scaler) );
    h->rgba_scaler.scaler_flags
        = SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND | SWS_BICUBIC;
    h->rgba_scaler.frame_prop_change_flags = 0;
    h->rgba_scaler.input_width = 0;
    h->rgba_scaler.input_height = 0;
    h->rgba_scaler.input_pixel_format = AV_PIX_FMT_NONE;
    h->rgba_scaler.output_pixel_format = AV_PIX_FMT_RGBA;
    h->rgba_scaler.input_colorspace = AVCOL_SPC_UNSPECIFIED;
    h->rgba_scaler.input_yuv_range = AVCOL_RANGE_UNSPECIFIED;
    h->rgba_scaler.sws_ctx = NULL;

    h->rgba_scaler_inited = 1;
}

static void init_gray8_scaler_if_needed( lsmas_handle_t *h )
{
    if( !h || h->gray8_scaler_inited )
        return;

    memset( &h->gray8_scaler, 0, sizeof(h->gray8_scaler) );
    h->gray8_scaler.scaler_flags
        = SWS_ACCURATE_RND | SWS_BICUBIC;
    h->gray8_scaler.frame_prop_change_flags = 0;
    h->gray8_scaler.input_width = 0;
    h->gray8_scaler.input_height = 0;
    h->gray8_scaler.input_pixel_format = AV_PIX_FMT_NONE;
    h->gray8_scaler.output_pixel_format = AV_PIX_FMT_GRAY8;
    h->gray8_scaler.input_colorspace = AVCOL_SPC_UNSPECIFIED;
    h->gray8_scaler.input_yuv_range = AVCOL_RANGE_UNSPECIFIED;
    h->gray8_scaler.sws_ctx = NULL;

    h->gray8_scaler_inited = 1;
}

static void init_yuv420p8_scaler_if_needed( lsmas_handle_t *h )
{
    if( !h || h->yuv420p8_scaler_inited )
        return;

    memset( &h->yuv420p8_scaler, 0, sizeof(h->yuv420p8_scaler) );
    h->yuv420p8_scaler.scaler_flags = SWS_ACCURATE_RND | SWS_BICUBIC;
    h->yuv420p8_scaler.frame_prop_change_flags = 0;
    h->yuv420p8_scaler.input_width = 0;
    h->yuv420p8_scaler.input_height = 0;
    h->yuv420p8_scaler.input_pixel_format = AV_PIX_FMT_NONE;
    h->yuv420p8_scaler.output_pixel_format = AV_PIX_FMT_YUV420P;
    h->yuv420p8_scaler.input_colorspace = AVCOL_SPC_UNSPECIFIED;
    h->yuv420p8_scaler.input_yuv_range = AVCOL_RANGE_UNSPECIFIED;
    h->yuv420p8_scaler.sws_ctx = NULL;

    h->yuv420p8_scaler_inited = 1;
}

static void apply_ff_loglevel( int32_t ff_loglevel )
{
    /* Match VapourSynth/LWLibavSource behavior: 0..7 maps to a log level. */
    if( ff_loglevel <= 0 )
        av_log_set_level( AV_LOG_QUIET );
    else if( ff_loglevel == 1 )
        av_log_set_level( AV_LOG_PANIC );
    else if( ff_loglevel == 2 )
        av_log_set_level( AV_LOG_FATAL );
    else if( ff_loglevel == 3 )
        av_log_set_level( AV_LOG_ERROR );
    else if( ff_loglevel == 4 )
        av_log_set_level( AV_LOG_WARNING );
    else if( ff_loglevel == 5 )
        av_log_set_level( AV_LOG_INFO );
    else if( ff_loglevel == 6 )
        av_log_set_level( AV_LOG_VERBOSE );
    else if( ff_loglevel == 7 )
        av_log_set_level( AV_LOG_DEBUG );
    else
        av_log_set_level( AV_LOG_TRACE );
}

static int has_repeat_duplicates( const lwlibav_video_output_handler_t *vohp )
{
    if( !vohp || !vohp->repeat_control || !vohp->frame_order_list || vohp->frame_count < 2 )
        return 0;

    uint32_t limit = vohp->frame_count;
    if( limit > 256 )
        limit = 256;

    for( uint32_t i = 2; i <= limit; i++ )
    {
        const lw_video_frame_order_t *a = &vohp->frame_order_list[i - 1];
        const lw_video_frame_order_t *b = &vohp->frame_order_list[i];
        if( a->top == b->top && a->bottom == b->bottom )
            return 1;
    }

    return 0;
}

static int64_t calc_cfr_delta_pts( AVRational time_base, int64_t fps_num, int64_t fps_den )
{
    if( time_base.num == 0 || time_base.den == 0 || fps_num <= 0 || fps_den <= 0 )
        return 1;

    if( fps_num > INT_MAX )
        fps_num = INT_MAX;
    if( fps_den > INT_MAX )
        fps_den = INT_MAX;

    AVRational frame_duration = { (int)fps_den, (int)fps_num }; /* seconds per frame */
    int64_t delta = av_rescale_q_rnd( 1, frame_duration, time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX );
    if( delta <= 0 )
        return 1;
    return delta;
}

static int64_t get_video_output_pts( const lsmas_handle_t *h, uint32_t output_frame_number )
{
    /* output_frame_number is 1-based. */
    if( !h || !h->vdhp || !h->vohp )
        return INT64_MIN;

    lwlibav_video_decode_handler_t *vdhp = h->vdhp;
    lwlibav_video_output_handler_t *vohp = h->vohp;

    if( vohp->vfr2cfr )
    {
        int64_t base_ts = (vdhp->min_ts != AV_NOPTS_VALUE) ? vdhp->min_ts : 0;
        int64_t delta = calc_cfr_delta_pts( vdhp->time_base, (int64_t)vohp->cfr_num, (int64_t)vohp->cfr_den );
        return base_ts + (int64_t)(output_frame_number - 1) * delta;
    }

    if( vohp->repeat_control && has_repeat_duplicates( vohp ) )
    {
        /* Repeat control duplicates pictures/fields; output timeline is effectively CFR. */
        int64_t base_ts = (vdhp->min_ts != AV_NOPTS_VALUE) ? vdhp->min_ts
                      : (vdhp->frame_count >= 1 ? vdhp->frame_list[1].pts : 0);
        int64_t delta = calc_cfr_delta_pts( vdhp->time_base, (int64_t)h->info.fps_num, (int64_t)h->info.fps_den );
        return base_ts + (int64_t)(output_frame_number - 1) * delta;
    }

    if( vohp->repeat_control && vohp->frame_order_list )
    {
        uint32_t src = vohp->frame_order_list[ output_frame_number ].top;
        if( src == 0 || src > vdhp->frame_count )
            src = vohp->frame_order_list[ output_frame_number ].bottom;
        if( src == 0 || src > vdhp->frame_count )
            src = 1;
        return vdhp->frame_list[ src ].pts;
    }

    if( output_frame_number > vdhp->frame_count )
        return vdhp->frame_list[ vdhp->frame_count ].pts;

    return vdhp->frame_list[ output_frame_number ].pts;
}

static enum AVSampleFormat map_audio_sample_format( lsmas_audio_sample_format_t fmt )
{
    switch( fmt )
    {
        case LSMAS_AUDIO_S16:
            return AV_SAMPLE_FMT_S16;
        case LSMAS_AUDIO_S32:
            return AV_SAMPLE_FMT_S32;
        case LSMAS_AUDIO_F32:
        default:
            return AV_SAMPLE_FMT_FLT;
    }
}

static int setup_audio_rendering( lsmas_handle_t *h, const lsmas_audio_open_options_t *opt_in )
{
    if( !h || !h->adhp || !h->aohp )
        return -1;

    lwlibav_audio_decode_handler_t *adhp = h->adhp;
    lwlibav_audio_output_handler_t *aohp = h->aohp;
    AVCodecContext *ctx = lwlibav_audio_get_codec_context( adhp );
    if( !ctx )
    {
        set_last_error( h, "Invalid internal state (no audio codec context)." );
        return -1;
    }

#if LSMAS_LSW_VARIANT_HOE
    if( ctx->ch_layout.nb_channels == 0 )
    {
        /* Be conservative: assume mono if layout is missing. */
        av_channel_layout_default( &ctx->ch_layout, 1 );
    }

    if( aohp->output_channel_layout.nb_channels == 0 )
        av_channel_layout_copy( &aohp->output_channel_layout, &ctx->ch_layout );

    if( opt_in && opt_in->channel_layout != 0 )
    {
        av_channel_layout_uninit( &aohp->output_channel_layout );
        av_channel_layout_from_mask( &aohp->output_channel_layout, (uint64_t)opt_in->channel_layout );
    }

    if( aohp->output_channel_layout.nb_channels == 0 )
        av_channel_layout_default( &aohp->output_channel_layout, ctx->ch_layout.nb_channels > 0 ? ctx->ch_layout.nb_channels : 1 );
#else
    if( ctx->channel_layout == 0 )
        ctx->channel_layout = av_get_default_channel_layout( ctx->channels );

    if( aohp->output_channel_layout == 0 )
        aohp->output_channel_layout = ctx->channel_layout;
    if( opt_in && opt_in->channel_layout != 0 )
        aohp->output_channel_layout = opt_in->channel_layout;
    if( aohp->output_channel_layout == 0 )
        aohp->output_channel_layout = av_get_default_channel_layout( ctx->channels > 0 ? ctx->channels : 1 );
#endif

    if( aohp->output_sample_rate <= 0 )
        aohp->output_sample_rate = ctx->sample_rate;
    if( opt_in && opt_in->sample_rate > 0 )
        aohp->output_sample_rate = opt_in->sample_rate;
    if( aohp->output_sample_rate <= 0 )
        aohp->output_sample_rate = 48000;

    /* Output format (interleaved). Default to float32 for .NET consumers. */
    enum AVSampleFormat out_fmt = AV_SAMPLE_FMT_FLT;
    if( opt_in )
        out_fmt = map_audio_sample_format( opt_in->sample_format );
    aohp->output_sample_format = out_fmt;
    aohp->s24_output = 0;
    aohp->output_bits_per_sample = av_get_bytes_per_sample( out_fmt ) * 8;

#if LSMAS_LSW_VARIANT_HOE
    int output_channels = aohp->output_channel_layout.nb_channels;
    if( output_channels <= 0 )
        output_channels = ctx->ch_layout.nb_channels > 0 ? ctx->ch_layout.nb_channels : 1;
#else
    int output_channels = av_get_channel_layout_nb_channels( aohp->output_channel_layout );
    if( output_channels <= 0 )
        output_channels = ctx->channels > 0 ? ctx->channels : 1;
#endif
    aohp->output_block_align = output_channels * av_get_bytes_per_sample( out_fmt );

    /* Set up resampler. */
    if( aohp->swr_ctx )
        swr_free( &aohp->swr_ctx );
    aohp->swr_ctx = swr_alloc();
    if( !aohp->swr_ctx )
    {
        set_last_error( h, "Failed to allocate audio resampler (swr_alloc)." );
        return -1;
    }

#if LSMAS_LSW_VARIANT_HOE
    if( update_resampler_configuration( aohp->swr_ctx,
                                        &aohp->output_channel_layout, aohp->output_sample_rate, aohp->output_sample_format,
                                        &ctx->ch_layout,             ctx->sample_rate,         ctx->sample_fmt,
                                        &aohp->input_planes, &aohp->input_block_align ) < 0 )
#else
    if( update_resampler_configuration( aohp->swr_ctx,
                                        aohp->output_channel_layout, aohp->output_sample_rate, aohp->output_sample_format,
                                        ctx->channel_layout,           ctx->sample_rate,           ctx->sample_fmt,
                                        &aohp->input_planes, &aohp->input_block_align ) < 0 )
#endif
    {
        set_last_error( h, "Failed to initialize audio resampler (swr_init)." );
        return -1;
    }

#if LSMAS_LSW_VARIANT_HOE
    av_channel_layout_uninit( &aohp->input_channel_layout );
    av_channel_layout_copy( &aohp->input_channel_layout, &ctx->ch_layout );
#else
    aohp->input_channel_layout = ctx->channel_layout;
#endif
    aohp->input_sample_rate    = ctx->sample_rate;
    aohp->input_sample_format  = ctx->sample_fmt;
    aohp->output_sample_offset = 0;

    return 0;
}

static int init_audio_provider_on_handle( lsmas_handle_t *h, const lsmas_audio_open_options_t *opt_in, char **error_message )
{
    if( !h || !h->adhp || !h->aohp )
    {
        set_error_message( error_message, "Invalid handle state (audio handlers missing)." );
        return -1;
    }

    TRACE( "lsmasnative: get_desired_track (audio) start" );
    open_lock_enter();
    int track_ret = lwlibav_audio_get_desired_track( h->lwh.file_path, h->adhp, h->lwh.threads );
    open_lock_leave();
    if( track_ret < 0 )
    {
        set_error_message( error_message, h->last_error ? h->last_error : "Failed to get desired audio track." );
        return -1;
    }

    TRACE( "lsmasnative: import_av_index_entry (audio) start" );
    if( lwlibav_import_av_index_entry( (lwlibav_decode_handler_t *)h->adhp ) < 0 )
    {
        set_error_message( error_message, h->last_error ? h->last_error : "Failed to import AVIndexEntry for audio." );
        return -1;
    }

    if( setup_audio_rendering( h, opt_in ) < 0 )
    {
        set_error_message( error_message, h->last_error ? h->last_error : "Failed to set up audio output." );
        return -1;
    }

    AVCodecContext *ctx = lwlibav_audio_get_codec_context( h->adhp );
    uint64_t decoded_samples = lwlibav_audio_count_overall_pcm_samples( h->adhp, h->aohp->output_sample_rate );
    if( decoded_samples == 0 )
    {
        set_error_message( error_message, "No valid audio frame." );
        return -1;
    }

    /* Rescale av_gap to output sample rate if needed (match LWLibavAudioSource). */
    if( h->lwh.av_gap && ctx && ctx->sample_rate > 0 && h->aohp->output_sample_rate != ctx->sample_rate )
        h->lwh.av_gap = ((int64_t)h->lwh.av_gap * h->aohp->output_sample_rate - 1) / ctx->sample_rate + 1;


#if LSMAS_LSW_VARIANT_HOE
    int channels = h->aohp->output_channel_layout.nb_channels;
    if( channels <= 0 )
        channels = (ctx && ctx->ch_layout.nb_channels > 0) ? ctx->ch_layout.nb_channels : 1;
    uint64_t channel_layout_mask = 0;
    if( h->aohp->output_channel_layout.order == AV_CHANNEL_ORDER_NATIVE )
        channel_layout_mask = (uint64_t)h->aohp->output_channel_layout.u.mask;
#else
    int channels = av_get_channel_layout_nb_channels( h->aohp->output_channel_layout );
    if( channels <= 0 )
        channels = (ctx && ctx->channels > 0) ? ctx->channels : 1;
    uint64_t channel_layout_mask = (uint64_t)h->aohp->output_channel_layout;
#endif

    h->audio_info_valid = 1;
    memset( &h->audio_info, 0, sizeof(h->audio_info) );
    h->audio_info.stream_index     = h->adhp->stream_index;
    h->audio_info.sample_rate      = h->aohp->output_sample_rate;
    h->audio_info.channels         = channels;
    h->audio_info.channel_layout   = channel_layout_mask;
    h->audio_info.sample_format    = (int32_t)h->aohp->output_sample_format;
    h->audio_info.bits_per_sample  = h->aohp->output_bits_per_sample;
    h->audio_info.bytes_per_sample = av_get_bytes_per_sample( h->aohp->output_sample_format );
    h->audio_info.block_align      = h->aohp->output_block_align;
    h->audio_info.decoded_samples  = (int64_t)decoded_samples;
    h->audio_info.delay_samples    = h->lwh.av_gap;
    {
        int64_t total = (int64_t)decoded_samples + h->lwh.av_gap;
        if( total < 0 )
            total = 0;
        h->audio_info.total_samples = total;
    }

    lwlibav_audio_force_seek( h->adhp );

    return 0;
}

LSMAS_NATIVE_API lsmas_handle_t *lsmas_video_open_utf8(
    const char *file_path_utf8,
    const lsmas_video_open_options_t *options,
    char **error_message
)
{
    ensure_ffmpeg_threadsafe();

    if( !file_path_utf8 || !file_path_utf8[0] )
    {
        set_error_message( error_message, "file_path_utf8 is null/empty." );
        return NULL;
    }

    lsmas_video_open_options_t opt_in = { 0 };
    if( options )
        opt_in = *options;
    else
    {
        opt_in.stream_index   = -1;
        opt_in.threads        = 0;
        opt_in.seek_mode      = LSMAS_SEEK_NORMAL;
        opt_in.seek_threshold = 10;
        opt_in.fpsnum         = 0;
        opt_in.fpsden         = 1;
        opt_in.prefer_hw      = LSMAS_HW_NONE;
        opt_in.cache_index    = 1;
        opt_in.soft_reset     = 1;
        opt_in.repeat         = 1;
        opt_in.dominance      = LSMAS_DOMINANCE_OBEY;
    }

    apply_ff_loglevel( opt_in.ff_loglevel );

    lsmas_handle_t *h = (lsmas_handle_t *)lw_malloc_zero( sizeof(lsmas_handle_t) );
    if( !h )
    {
        set_error_message( error_message, "Out of memory." );
        return NULL;
    }

#ifdef _WIN32
    InitializeCriticalSection( &h->error_lock );
    h->error_lock_inited = 1;
#endif

    h->vdhp = lwlibav_video_alloc_decode_handler();
    h->vohp = lwlibav_video_alloc_output_handler();
    h->adhp = lwlibav_audio_alloc_decode_handler();
    h->aohp = lwlibav_audio_alloc_output_handler();
    if( !h->vdhp || !h->vohp || !h->adhp || !h->aohp )
    {
        set_error_message( error_message, "Failed to allocate handlers." );
        lsmas_video_close( h );
        return NULL;
    }

    lw_log_handler_t lh = { 0 };
    lh.level    = LW_LOG_ERROR;
    lh.priv     = h;
    lh.show_log = on_log;

    memset( h->preferred_decoder_names_buf, 0, sizeof(h->preferred_decoder_names_buf) );
    if( opt_in.decoder )
    {
        size_t n = strlen( opt_in.decoder );
        size_t c = MIN( sizeof(h->preferred_decoder_names_buf) - 1, n );
        memcpy( h->preferred_decoder_names_buf, opt_in.decoder, c );
    }
    h->preferred_decoder_names = lw_tokenize_string( h->preferred_decoder_names_buf, ',', NULL );
    lwlibav_video_set_preferred_decoder_names( h->vdhp, h->preferred_decoder_names );
#if LSMAS_LSW_VARIANT_HOE
    h->prefer_hw_decoder = (int)CLIP_VALUE( opt_in.prefer_hw, 0, 3 );
    lwlibav_video_set_prefer_hw_decoder( h->vdhp, &h->prefer_hw_decoder );
#else
    lwlibav_video_set_prefer_hw_decoder( h->vdhp, (int)CLIP_VALUE( opt_in.prefer_hw, 0, 3 ) );
#endif
    lwlibav_video_set_seek_mode( h->vdhp, (int)CLIP_VALUE( opt_in.seek_mode, 0, 2 ) );
    lwlibav_video_set_forward_seek_threshold( h->vdhp, (uint32_t)CLIP_VALUE( opt_in.seek_threshold, 1, 999 ) );
#if !LSMAS_LSW_VARIANT_HOE
    lwlibav_video_set_soft_reset( h->vdhp, (int)CLIP_VALUE( opt_in.soft_reset, 0, 1 ) );
#endif
    lwlibav_video_set_log_handler( h->vdhp, &lh );
    lwlibav_audio_set_log_handler( h->adhp, &lh );

    lwlibav_option_t opt = { 0 };
    opt.file_path         = file_path_utf8;
    opt.cache_dir         = opt_in.cachedir;
    opt.threads           = opt_in.threads >= 0 ? opt_in.threads : 0;
    opt.av_sync           = 0;
    opt.no_create_index   = !opt_in.cache_index;
    opt.index_file_path   = opt_in.cachefile;
    opt.force_video       = (opt_in.stream_index >= 0);
    opt.force_video_index = (opt_in.stream_index >= 0) ? opt_in.stream_index : -1;
    opt.force_audio       = 0;
    opt.force_audio_index = -2;
    opt.apply_repeat_flag = opt_in.repeat ? 1 : 0;
    opt.field_dominance   = (int)CLIP_VALUE( opt_in.dominance, 0, 2 );
    opt.vfr2cfr.active    = (opt_in.fpsnum > 0 && opt_in.fpsden > 0) ? 1 : 0;
    opt.vfr2cfr.fps_num   = (uint32_t)opt_in.fpsnum;
    opt.vfr2cfr.fps_den   = (uint32_t)opt_in.fpsden;

    progress_indicator_t indicator = { 0 };

    lsmas_progress_handler_t php = { 0 };
    progress_handler_t *php_ptr = NULL;
    init_progress_indicator( &indicator, &php_ptr, &php, g_tls_progress_cb, g_tls_progress_userdata );

    TRACE( "lsmasnative: construct_index start" );
    /* FFmpeg in this branch can initialize some shared VLC tables lazily and is not robust to
       concurrent codec opens. Serialize index construction/codec open to avoid av_assert0 aborts. */
    open_lock_enter();
    int ret = lwlibav_construct_index( &h->lwh, h->vdhp, h->vohp, h->adhp, h->aohp, &lh, &opt, &indicator, php_ptr );
    open_lock_leave();
    TRACE( "lsmasnative: construct_index ret=%d", ret );
    lwlibav_audio_free_decode_handler_ptr( &h->adhp );
    lwlibav_audio_free_output_handler_ptr( &h->aohp );
    if( ret < 0 )
    {
        set_error_message( error_message, h->last_error ? h->last_error : "Failed to construct index." );
        lsmas_video_close( h );
        return NULL;
    }

    /* Eliminate silent failure: if repeat=1, then fail if repeat is requested but not applied.
       This matches VapourSynth/LWLibavSource behavior. */
    if( opt.apply_repeat_flag == 1 )
    {
        if( h->vohp->repeat_requested && !h->vohp->repeat_control )
        {
            set_error_message( error_message, "repeat requested by input video, but unable to obey (try repeat=0 to get a VFR clip)." );
            lsmas_video_close( h );
            return NULL;
        }
    }

    lwlibav_video_set_log_handler( h->vdhp, &lh );
    TRACE( "lsmasnative: get_desired_track start" );
    open_lock_enter();
    int track_ret = lwlibav_video_get_desired_track( h->lwh.file_path, h->vdhp, h->lwh.threads );
    open_lock_leave();
    if( track_ret < 0 )
    {
        char buf[256] = { 0 };
        (void)snprintf(
            buf,
            sizeof(buf),
            "Failed to get desired video track. stream_index=%d frame_count=%u",
            h->vdhp ? h->vdhp->stream_index : -999,
            h->vdhp ? (unsigned)h->vdhp->frame_count : 0U
        );
        set_error_message( error_message, h->last_error ? h->last_error : buf );
        lsmas_video_close( h );
        return NULL;
    }

    TRACE( "lsmasnative: import_av_index_entry start" );
    if( lwlibav_import_av_index_entry( (lwlibav_decode_handler_t *)h->vdhp ) < 0 )
    {
        set_error_message( error_message, h->last_error ? h->last_error : "Failed to import AVIndexEntry." );
        lsmas_video_close( h );
        return NULL;
    }
    TRACE( "lsmasnative: import_av_index_entry ok" );

    TRACE( "lsmasnative: set_initial_input_format start" );
    lwlibav_video_set_initial_input_format( h->vdhp );
    TRACE( "lsmasnative: set_initial_input_format ok" );
    AVCodecContext *ctx = lwlibav_video_get_codec_context( h->vdhp );

    TRACE( "lsmasnative: get_max_width/height start" );
    int max_width  = lwlibav_video_get_max_width( h->vdhp );
    int max_height = lwlibav_video_get_max_height( h->vdhp );
    TRACE( "lsmasnative: get_max_width/height w=%d h=%d", max_width, max_height );
    if( max_width <= 0 || max_height <= 0 )
    {
        set_error_message( error_message, "Invalid frame size detected (invalid/corrupted input file?)." );
        lsmas_video_close( h );
        return NULL;
    }

    TRACE( "lsmasnative: setup_video_rendering start" );
    setup_video_rendering(
        h->vohp,
        SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND | SWS_BICUBIC,
        max_width,
        max_height,
        AV_PIX_FMT_BGRA,
        ctx,
        NULL
    );
    TRACE( "lsmasnative: setup_video_rendering ok" );

    TRACE( "lsmasnative: set_get_buffer_func start" );
    lwlibav_video_set_get_buffer_func( h->vdhp );
    TRACE( "lsmasnative: set_get_buffer_func ok" );

    TRACE( "lsmasnative: find_first_valid_frame start" );
    if( lwlibav_video_find_first_valid_frame( h->vdhp ) < 0 )
    {
        set_error_message( error_message, h->last_error ? h->last_error : "Failed to find the first valid video frame." );
        lsmas_video_close( h );
        return NULL;
    }
    TRACE( "lsmasnative: find_first_valid_frame ok" );

    TRACE( "lsmasnative: video_force_seek start" );
    lwlibav_video_force_seek( h->vdhp );
    TRACE( "lsmasnative: video_force_seek ok" );
    TRACE( "lsmasnative: update_info start" );
    update_info( h, &opt_in );
    TRACE( "lsmasnative: update_info ok" );
    init_rgba_scaler_if_needed( h );
    init_gray8_scaler_if_needed( h );

    return h;
}

LSMAS_NATIVE_API lsmas_handle_t *lsmas_video_open_with_progress_utf8(
    const char *file_path_utf8,
    const lsmas_video_open_options_t *options,
    lsmas_progress_callback_t progress_cb,
    void *progress_userdata,
    char **error_message
)
{
    g_tls_progress_cb = progress_cb;
    g_tls_progress_userdata = progress_userdata;
    lsmas_handle_t *h = lsmas_video_open_utf8( file_path_utf8, options, error_message );
    g_tls_progress_cb = NULL;
    g_tls_progress_userdata = NULL;
    return h;
}

LSMAS_NATIVE_API lsmas_handle_t *lsmas_av_open_utf8(
    const char *file_path_utf8,
    const lsmas_video_open_options_t *video_options,
    const lsmas_audio_open_options_t *audio_options,
    char **error_message
)
{
    ensure_ffmpeg_threadsafe();

    if( !file_path_utf8 || !file_path_utf8[0] )
    {
        set_error_message( error_message, "file_path_utf8 is null/empty." );
        return NULL;
    }

    lsmas_video_open_options_t vopt_in = { 0 };
    if( video_options )
        vopt_in = *video_options;
    else
    {
        vopt_in.stream_index   = -1;
        vopt_in.threads        = 0;
        vopt_in.seek_mode      = LSMAS_SEEK_NORMAL;
        vopt_in.seek_threshold = 10;
        vopt_in.fpsnum         = 0;
        vopt_in.fpsden         = 1;
        vopt_in.prefer_hw      = LSMAS_HW_NONE;
        vopt_in.cache_index    = 1;
        vopt_in.soft_reset     = 1;
        vopt_in.repeat         = 1;
        vopt_in.dominance      = LSMAS_DOMINANCE_OBEY;
    }

    lsmas_audio_open_options_t aopt_in = { 0 };
    if( audio_options )
        aopt_in = *audio_options;
    else
    {
        aopt_in.stream_index   = -1;
        aopt_in.threads        = 0;
        aopt_in.av_sync        = 1;
        aopt_in.ff_loglevel    = 0;
        aopt_in.decoder        = NULL;
        aopt_in.cache_index    = vopt_in.cache_index;
        aopt_in.cachefile      = vopt_in.cachefile;
        aopt_in.cachedir       = vopt_in.cachedir;
        aopt_in.channel_layout = 0;
        aopt_in.sample_rate    = 0;
        aopt_in.sample_format  = LSMAS_AUDIO_F32;
    }

    /* Prefer explicit audio loglevel when provided. */
    apply_ff_loglevel( audio_options ? aopt_in.ff_loglevel : vopt_in.ff_loglevel );

    lsmas_handle_t *h = (lsmas_handle_t *)lw_malloc_zero( sizeof(lsmas_handle_t) );
    if( !h )
    {
        set_error_message( error_message, "Out of memory." );
        return NULL;
    }

#ifdef _WIN32
    InitializeCriticalSection( &h->error_lock );
    h->error_lock_inited = 1;
#endif

    h->vdhp = lwlibav_video_alloc_decode_handler();
    h->vohp = lwlibav_video_alloc_output_handler();
    h->adhp = lwlibav_audio_alloc_decode_handler();
    h->aohp = lwlibav_audio_alloc_output_handler();
    if( !h->vdhp || !h->vohp || !h->adhp || !h->aohp )
    {
        set_error_message( error_message, "Failed to allocate handlers." );
        lsmas_video_close( h );
        return NULL;
    }

    lw_log_handler_t lh = { 0 };
    lh.level    = LW_LOG_ERROR;
    lh.priv     = h;
    lh.show_log = on_log;

    memset( h->preferred_decoder_names_buf, 0, sizeof(h->preferred_decoder_names_buf) );
    if( vopt_in.decoder )
    {
        size_t n = strlen( vopt_in.decoder );
        size_t c = MIN( sizeof(h->preferred_decoder_names_buf) - 1, n );
        memcpy( h->preferred_decoder_names_buf, vopt_in.decoder, c );
    }
    h->preferred_decoder_names = lw_tokenize_string( h->preferred_decoder_names_buf, ',', NULL );
    lwlibav_video_set_preferred_decoder_names( h->vdhp, h->preferred_decoder_names );

    memset( h->preferred_audio_decoder_names_buf, 0, sizeof(h->preferred_audio_decoder_names_buf) );
    const char *audio_decoder_names = aopt_in.decoder && aopt_in.decoder[0] ? aopt_in.decoder : vopt_in.decoder;
    if( audio_decoder_names )
    {
        size_t n = strlen( audio_decoder_names );
        size_t c = MIN( sizeof(h->preferred_audio_decoder_names_buf) - 1, n );
        memcpy( h->preferred_audio_decoder_names_buf, audio_decoder_names, c );
    }
    h->preferred_audio_decoder_names = lw_tokenize_string( h->preferred_audio_decoder_names_buf, ',', NULL );
    lwlibav_audio_set_preferred_decoder_names( h->adhp, h->preferred_audio_decoder_names );

#if LSMAS_LSW_VARIANT_HOE
    h->prefer_hw_decoder = (int)CLIP_VALUE( vopt_in.prefer_hw, 0, 3 );
    lwlibav_video_set_prefer_hw_decoder( h->vdhp, &h->prefer_hw_decoder );
#else
    lwlibav_video_set_prefer_hw_decoder( h->vdhp, (int)CLIP_VALUE( vopt_in.prefer_hw, 0, 3 ) );
#endif
    lwlibav_video_set_seek_mode( h->vdhp, (int)CLIP_VALUE( vopt_in.seek_mode, 0, 2 ) );
    lwlibav_video_set_forward_seek_threshold( h->vdhp, (uint32_t)CLIP_VALUE( vopt_in.seek_threshold, 1, 999 ) );
#if !LSMAS_LSW_VARIANT_HOE
    lwlibav_video_set_soft_reset( h->vdhp, (int)CLIP_VALUE( vopt_in.soft_reset, 0, 1 ) );
#endif
    lwlibav_video_set_log_handler( h->vdhp, &lh );
    lwlibav_audio_set_log_handler( h->adhp, &lh );

    lwlibav_option_t opt = { 0 };
    opt.file_path         = file_path_utf8;
    opt.cache_dir         = vopt_in.cachedir;
    opt.threads           = vopt_in.threads >= 0 ? vopt_in.threads : 0;
    opt.av_sync           = aopt_in.av_sync ? 1 : 0;
    opt.no_create_index   = !vopt_in.cache_index;
    opt.index_file_path   = vopt_in.cachefile;
    opt.force_video       = (vopt_in.stream_index >= 0);
    opt.force_video_index = (vopt_in.stream_index >= 0) ? vopt_in.stream_index : -1;
    opt.force_audio       = (aopt_in.stream_index >= 0);
    opt.force_audio_index = (aopt_in.stream_index >= 0) ? aopt_in.stream_index : -1;
    opt.apply_repeat_flag = vopt_in.repeat ? 1 : 0;
    opt.field_dominance   = (int)CLIP_VALUE( vopt_in.dominance, 0, 2 );
    opt.vfr2cfr.active    = (vopt_in.fpsnum > 0 && vopt_in.fpsden > 0) ? 1 : 0;
    opt.vfr2cfr.fps_num   = (uint32_t)vopt_in.fpsnum;
    opt.vfr2cfr.fps_den   = (uint32_t)vopt_in.fpsden;

#if LSMAS_LSW_VARIANT_HOE
    /* HOE: AV-open with (stream_index=-1) can leave the parsed index in a state with frame_count=0.
       For robustness (and to match typical "auto" selection), preselect likely video/audio streams. */
    if( vopt_in.stream_index < 0 && opt.force_video == 0 )
    {
        int best_v = -1;
        if( choose_best_stream_index_utf8( file_path_utf8, AVMEDIA_TYPE_VIDEO, &best_v ) == 0 )
        {
            opt.force_video = 1;
            opt.force_video_index = best_v;
        }
    }
    if( aopt_in.stream_index < 0 && opt.force_audio == 0 )
    {
        int best_a = -1;
        if( choose_best_stream_index_utf8( file_path_utf8, AVMEDIA_TYPE_AUDIO, &best_a ) == 0 )
        {
            opt.force_audio = 1;
            opt.force_audio_index = best_a;
        }
    }
#endif

    progress_indicator_t indicator = { 0 };
    lsmas_progress_handler_t php = { 0 };
    progress_handler_t *php_ptr = NULL;
    init_progress_indicator( &indicator, &php_ptr, &php, g_tls_progress_cb, g_tls_progress_userdata );

    TRACE( "lsmasnative: construct_index (av) start" );
    open_lock_enter();
    int ret = lwlibav_construct_index( &h->lwh, h->vdhp, h->vohp, h->adhp, h->aohp, &lh, &opt, &indicator, php_ptr );
    open_lock_leave();
    TRACE( "lsmasnative: construct_index (av) ret=%d", ret );
    if( ret < 0 )
    {
        set_error_message( error_message, h->last_error ? h->last_error : "Failed to construct index." );
        lsmas_video_close( h );
        return NULL;
    }

    if( opt.apply_repeat_flag == 1 )
    {
        if( h->vohp->repeat_requested && !h->vohp->repeat_control )
        {
            set_error_message( error_message, "repeat requested by input video, but unable to obey (try repeat=0 to get a VFR clip)." );
            lsmas_video_close( h );
            return NULL;
        }
    }

    /* Video provider init */
    lwlibav_video_set_log_handler( h->vdhp, &lh );

#if LSMAS_LSW_VARIANT_HOE
    /* HOE: Best-effort auto stream selection fallback for AV-open.
       In some cases, .lwi parsing can leave stream_index unset when used in this combined path. */
    if( h->vdhp && h->vohp && h->vdhp->frame_count == 0 && h->vohp->frame_count > 0 )
        h->vdhp->frame_count = h->vohp->frame_count;
    if( vopt_in.stream_index < 0 && h->vdhp->stream_index < 0 )
    {
        int best_v = -1;
        if( choose_best_stream_index_utf8( file_path_utf8, AVMEDIA_TYPE_VIDEO, &best_v ) == 0 )
            h->vdhp->stream_index = best_v;
    }
    if( aopt_in.stream_index < 0 && h->adhp->stream_index < 0 )
    {
        int best_a = -1;
        if( choose_best_stream_index_utf8( file_path_utf8, AVMEDIA_TYPE_AUDIO, &best_a ) == 0 )
            h->adhp->stream_index = best_a;
    }
#endif

    TRACE( "lsmasnative: get_desired_track (video) start" );
    open_lock_enter();
    int track_ret = lwlibav_video_get_desired_track( h->lwh.file_path, h->vdhp, h->lwh.threads );
    open_lock_leave();
    if( track_ret < 0 )
    {
        const char *base = h->last_error ? h->last_error : "Failed to get desired video track.";
        char buf[256] = { 0 };
        (void)snprintf(
            buf,
            sizeof(buf),
            "%s (stream_index=%d frame_count=%u)",
            base,
            h->vdhp ? h->vdhp->stream_index : -999,
            h->vdhp ? (unsigned)h->vdhp->frame_count : 0U
        );
        set_error_message( error_message, buf );
        lsmas_video_close( h );
        return NULL;
    }

    TRACE( "lsmasnative: import_av_index_entry (video) start" );
    if( lwlibav_import_av_index_entry( (lwlibav_decode_handler_t *)h->vdhp ) < 0 )
    {
        set_error_message( error_message, h->last_error ? h->last_error : "Failed to import AVIndexEntry (video)." );
        lsmas_video_close( h );
        return NULL;
    }

    lwlibav_video_set_initial_input_format( h->vdhp );
    AVCodecContext *ctx = lwlibav_video_get_codec_context( h->vdhp );

    int max_width  = lwlibav_video_get_max_width( h->vdhp );
    int max_height = lwlibav_video_get_max_height( h->vdhp );
    if( max_width <= 0 || max_height <= 0 )
    {
        set_error_message( error_message, "Invalid frame size detected (invalid/corrupted input file?)." );
        lsmas_video_close( h );
        return NULL;
    }

    setup_video_rendering(
        h->vohp,
        SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND | SWS_BICUBIC,
        max_width,
        max_height,
        AV_PIX_FMT_BGRA,
        ctx,
        NULL
    );

    lwlibav_video_set_get_buffer_func( h->vdhp );

    if( lwlibav_video_find_first_valid_frame( h->vdhp ) < 0 )
    {
        set_error_message( error_message, h->last_error ? h->last_error : "Failed to find the first valid video frame." );
        lsmas_video_close( h );
        return NULL;
    }

    lwlibav_video_force_seek( h->vdhp );
    update_info( h, &vopt_in );
    init_rgba_scaler_if_needed( h );
    init_gray8_scaler_if_needed( h );

    /* Audio provider init (optional: skip if not indexed) */
    if( h->adhp->stream_index >= 0 && h->adhp->frame_count > 0 )
    {
        if( init_audio_provider_on_handle( h, &aopt_in, error_message ) < 0 )
        {
            lsmas_video_close( h );
            return NULL;
        }
    }
    else
    {
        /* Keep as "no audio" handle. */
        h->audio_info_valid = 0;
    }

    return h;
}

LSMAS_NATIVE_API lsmas_handle_t *lsmas_audio_open_utf8(
    const char *file_path_utf8,
    const lsmas_audio_open_options_t *options,
    char **error_message
)
{
    ensure_ffmpeg_threadsafe();

    if( !file_path_utf8 || !file_path_utf8[0] )
    {
        set_error_message( error_message, "file_path_utf8 is null/empty." );
        return NULL;
    }

    lsmas_audio_open_options_t opt_in = { 0 };
    if( options )
        opt_in = *options;
    else
    {
        opt_in.stream_index   = -1;
        opt_in.threads        = 0;
        opt_in.av_sync        = 0;
        opt_in.ff_loglevel    = 0;
        opt_in.decoder        = NULL;
        opt_in.cache_index    = 1;
        opt_in.cachefile      = NULL;
        opt_in.cachedir       = NULL;
        opt_in.channel_layout = 0;
        opt_in.sample_rate    = 0;
        opt_in.sample_format  = LSMAS_AUDIO_F32;
    }

    apply_ff_loglevel( opt_in.ff_loglevel );

    lsmas_handle_t *h = (lsmas_handle_t *)lw_malloc_zero( sizeof(lsmas_handle_t) );
    if( !h )
    {
        set_error_message( error_message, "Out of memory." );
        return NULL;
    }

#ifdef _WIN32
    InitializeCriticalSection( &h->error_lock );
    h->error_lock_inited = 1;
#endif

    h->vdhp = lwlibav_video_alloc_decode_handler();
    h->vohp = lwlibav_video_alloc_output_handler();
    h->adhp = lwlibav_audio_alloc_decode_handler();
    h->aohp = lwlibav_audio_alloc_output_handler();
    if( !h->vdhp || !h->vohp || !h->adhp || !h->aohp )
    {
        set_error_message( error_message, "Failed to allocate handlers." );
        lsmas_video_close( h );
        return NULL;
    }

    lw_log_handler_t lh = { 0 };
    lh.level    = LW_LOG_ERROR;
    lh.priv     = h;
    lh.show_log = on_log;

    memset( h->preferred_decoder_names_buf, 0, sizeof(h->preferred_decoder_names_buf) );
    if( opt_in.decoder )
    {
        size_t n = strlen( opt_in.decoder );
        size_t c = MIN( sizeof(h->preferred_decoder_names_buf) - 1, n );
        memcpy( h->preferred_decoder_names_buf, opt_in.decoder, c );
    }
    h->preferred_decoder_names = lw_tokenize_string( h->preferred_decoder_names_buf, ',', NULL );
    lwlibav_audio_set_preferred_decoder_names( h->adhp, h->preferred_decoder_names );

    lwlibav_video_set_log_handler( h->vdhp, &lh );
    lwlibav_audio_set_log_handler( h->adhp, &lh );

    lwlibav_option_t opt = { 0 };
    opt.file_path         = file_path_utf8;
    opt.cache_dir         = opt_in.cachedir;
    opt.threads           = opt_in.threads >= 0 ? opt_in.threads : 0;
    opt.av_sync           = opt_in.av_sync ? 1 : 0;
    opt.no_create_index   = !opt_in.cache_index;
    opt.index_file_path   = opt_in.cachefile;
    opt.force_video       = 0;
    opt.force_video_index = -1;
    opt.force_audio       = (opt_in.stream_index >= 0);
    opt.force_audio_index = (opt_in.stream_index >= 0) ? opt_in.stream_index : -1;
    opt.apply_repeat_flag = 0;
    opt.field_dominance   = 0;
    opt.vfr2cfr.active    = 0;
    opt.vfr2cfr.fps_num   = 0;
    opt.vfr2cfr.fps_den   = 0;

    progress_indicator_t indicator = { 0 };
    lsmas_progress_handler_t php = { 0 };
    progress_handler_t *php_ptr = NULL;
    init_progress_indicator( &indicator, &php_ptr, &php, g_tls_progress_cb, g_tls_progress_userdata );

    TRACE( "lsmasnative: construct_index (audio) start" );
    open_lock_enter();
    int ret = lwlibav_construct_index( &h->lwh, h->vdhp, h->vohp, h->adhp, h->aohp, &lh, &opt, &indicator, php_ptr );
    open_lock_leave();
    TRACE( "lsmasnative: construct_index (audio) ret=%d", ret );
    if( ret < 0 )
    {
        set_error_message( error_message, h->last_error ? h->last_error : "Failed to construct index." );
        lsmas_video_close( h );
        return NULL;
    }

    /* Audio-only handle: release video handlers after av_gap/index construction. */
    lwlibav_video_free_decode_handler_ptr( &h->vdhp );
    lwlibav_video_free_output_handler_ptr( &h->vohp );

    TRACE( "lsmasnative: get_desired_track (audio) start" );
    open_lock_enter();
    int track_ret = lwlibav_audio_get_desired_track( h->lwh.file_path, h->adhp, h->lwh.threads );
    open_lock_leave();
    if( track_ret < 0 )
    {
        set_error_message( error_message, h->last_error ? h->last_error : "Failed to get desired audio track." );
        lsmas_video_close( h );
        return NULL;
    }

    TRACE( "lsmasnative: import_av_index_entry (audio) start" );
    if( lwlibav_import_av_index_entry( (lwlibav_decode_handler_t *)h->adhp ) < 0 )
    {
        set_error_message( error_message, h->last_error ? h->last_error : "Failed to import AVIndexEntry for audio." );
        lsmas_video_close( h );
        return NULL;
    }

    if( setup_audio_rendering( h, &opt_in ) < 0 )
    {
        set_error_message( error_message, h->last_error ? h->last_error : "Failed to set up audio output." );
        lsmas_video_close( h );
        return NULL;
    }

    AVCodecContext *ctx = lwlibav_audio_get_codec_context( h->adhp );
    uint64_t decoded_samples = lwlibav_audio_count_overall_pcm_samples( h->adhp, h->aohp->output_sample_rate );
    if( decoded_samples == 0 )
    {
        set_error_message( error_message, "No valid audio frame." );
        lsmas_video_close( h );
        return NULL;
    }

    /* Rescale av_gap to output sample rate if needed (match LWLibavAudioSource). */
    if( h->lwh.av_gap && ctx && ctx->sample_rate > 0 && h->aohp->output_sample_rate != ctx->sample_rate )
        h->lwh.av_gap = ((int64_t)h->lwh.av_gap * h->aohp->output_sample_rate - 1) / ctx->sample_rate + 1;


#if LSMAS_LSW_VARIANT_HOE
    int channels = h->aohp->output_channel_layout.nb_channels;
    if( channels <= 0 )
        channels = (ctx && ctx->ch_layout.nb_channels > 0) ? ctx->ch_layout.nb_channels : 1;
    uint64_t channel_layout_mask = 0;
    if( h->aohp->output_channel_layout.order == AV_CHANNEL_ORDER_NATIVE )
        channel_layout_mask = (uint64_t)h->aohp->output_channel_layout.u.mask;
#else
    int channels = av_get_channel_layout_nb_channels( h->aohp->output_channel_layout );
    if( channels <= 0 )
        channels = (ctx && ctx->channels > 0) ? ctx->channels : 1;
    uint64_t channel_layout_mask = (uint64_t)h->aohp->output_channel_layout;
#endif

    h->audio_info_valid = 1;
    memset( &h->audio_info, 0, sizeof(h->audio_info) );
    h->audio_info.stream_index     = h->adhp->stream_index;
    h->audio_info.sample_rate      = h->aohp->output_sample_rate;
    h->audio_info.channels         = channels;
    h->audio_info.channel_layout   = channel_layout_mask;
    h->audio_info.sample_format    = (int32_t)h->aohp->output_sample_format;
    h->audio_info.bits_per_sample  = h->aohp->output_bits_per_sample;
    h->audio_info.bytes_per_sample = av_get_bytes_per_sample( h->aohp->output_sample_format );
    h->audio_info.block_align      = h->aohp->output_block_align;
    h->audio_info.decoded_samples  = (int64_t)decoded_samples;
    h->audio_info.delay_samples    = h->lwh.av_gap;
    {
        int64_t total = (int64_t)decoded_samples + h->lwh.av_gap;
        if( total < 0 )
            total = 0;
        h->audio_info.total_samples = total;
    }

    lwlibav_audio_force_seek( h->adhp );

    return h;
}

LSMAS_NATIVE_API lsmas_handle_t *lsmas_audio_open_with_progress_utf8(
    const char *file_path_utf8,
    const lsmas_audio_open_options_t *options,
    lsmas_progress_callback_t progress_cb,
    void *progress_userdata,
    char **error_message
)
{
    g_tls_progress_cb = progress_cb;
    g_tls_progress_userdata = progress_userdata;
    lsmas_handle_t *h = lsmas_audio_open_utf8( file_path_utf8, options, error_message );
    g_tls_progress_cb = NULL;
    g_tls_progress_userdata = NULL;
    return h;
}

LSMAS_NATIVE_API lsmas_handle_t *lsmas_av_open_with_progress_utf8(
    const char *file_path_utf8,
    const lsmas_video_open_options_t *video_options,
    const lsmas_audio_open_options_t *audio_options,
    lsmas_progress_callback_t progress_cb,
    void *progress_userdata,
    char **error_message
)
{
    g_tls_progress_cb = progress_cb;
    g_tls_progress_userdata = progress_userdata;
    lsmas_handle_t *h = lsmas_av_open_utf8( file_path_utf8, video_options, audio_options, error_message );
    g_tls_progress_cb = NULL;
    g_tls_progress_userdata = NULL;
    return h;
}

LSMAS_NATIVE_API void lsmas_audio_close( lsmas_handle_t *handle )
{
    lsmas_video_close( handle );
}

LSMAS_NATIVE_API void lsmas_av_close( lsmas_handle_t *handle )
{
    lsmas_video_close( handle );
}

LSMAS_NATIVE_API int lsmas_audio_get_info( lsmas_handle_t *handle, lsmas_audio_info_t *out_info, char **error_message )
{
    if( !handle || !out_info )
    {
        set_error_message( error_message, "handle/out_info is NULL." );
        return -1;
    }
    if( !handle->audio_info_valid )
    {
        set_error_message( error_message, "Invalid internal state (not an audio handle)." );
        return -1;
    }
    *out_info = handle->audio_info;
    return 0;
}

LSMAS_NATIVE_API int64_t lsmas_audio_get_samples(
    lsmas_handle_t *handle,
    void *dst,
    int64_t start,
    int64_t wanted_length,
    char **error_message
)
{
    if( !handle || !handle->audio_info_valid || !handle->adhp || !handle->aohp )
    {
        set_error_message( error_message, "Invalid handle (audio not opened)." );
        return -1;
    }
    if( !dst )
    {
        set_error_message( error_message, "dst is NULL." );
        return -1;
    }
    if( wanted_length <= 0 )
        return 0;

    uint8_t silence = handle->aohp->output_bits_per_sample == 8 ? 0x80 : 0x00;
    int64_t total = handle->audio_info.total_samples;
    if( total < 0 )
        total = 0;

    uint8_t *outp = (uint8_t *)dst;
    int64_t remaining = wanted_length;

    /* Leading silence for negative start. */
    if( start < 0 )
    {
        int64_t lead = -start;
        if( lead > remaining )
            lead = remaining;
        memset( outp, silence, (size_t)(lead * handle->aohp->output_block_align) );
        outp += lead * handle->aohp->output_block_align;
        start = 0;
        remaining -= lead;
        if( remaining <= 0 )
            return wanted_length;
    }

    /* Trailing silence when requesting beyond total_samples. */
    if( start >= total )
    {
        memset( outp, silence, (size_t)(remaining * handle->aohp->output_block_align) );
        lwlibav_audio_force_seek( handle->adhp );
        return wanted_length;
    }

    int64_t readable = total - start;
    if( readable < 0 )
        readable = 0;
    if( readable > remaining )
        readable = remaining;

    int64_t audio_delay = handle->lwh.av_gap;
    int64_t end = start + readable;
    if( readable > 0 && start < audio_delay && end <= audio_delay )
    {
        memset( outp, silence, (size_t)(readable * handle->aohp->output_block_align) );
        outp += readable * handle->aohp->output_block_align;
        lwlibav_audio_force_seek( handle->adhp );
    }
    else if( readable > 0 )
    {
        int64_t decode_start = start - audio_delay;
        int64_t written = (int64_t)lwlibav_audio_get_pcm_samples( handle->adhp, handle->aohp, outp, decode_start, readable );
        if( written < 0 )
            written = 0;
        if( written < readable )
        {
            /* Defensive: fill remaining with silence. */
            memset( outp + written * handle->aohp->output_block_align,
                    silence,
                    (size_t)((readable - written) * handle->aohp->output_block_align) );
        }
        outp += readable * handle->aohp->output_block_align;
    }

    remaining -= readable;
    if( remaining > 0 )
        memset( outp, silence, (size_t)(remaining * handle->aohp->output_block_align) );

    return wanted_length;
}

LSMAS_NATIVE_API void lsmas_video_close( lsmas_handle_t *handle )
{
    if( !handle )
        return;

    if( handle->preferred_decoder_names )
        lw_free( handle->preferred_decoder_names );
    handle->preferred_decoder_names = NULL;

    if( handle->preferred_audio_decoder_names )
        lw_free( handle->preferred_audio_decoder_names );
    handle->preferred_audio_decoder_names = NULL;

    lwlibav_video_free_decode_handler_ptr( &handle->vdhp );
    lwlibav_video_free_output_handler_ptr( &handle->vohp );

    if( handle->rgba_scaler_inited && handle->rgba_scaler.sws_ctx )
        sws_freeContext( handle->rgba_scaler.sws_ctx );
    handle->rgba_scaler.sws_ctx = NULL;
    handle->rgba_scaler_inited = 0;

    if( handle->gray8_scaler_inited && handle->gray8_scaler.sws_ctx )
        sws_freeContext( handle->gray8_scaler.sws_ctx );
    handle->gray8_scaler.sws_ctx = NULL;
    handle->gray8_scaler_inited = 0;

    if( handle->yuv420p8_scaler_inited && handle->yuv420p8_scaler.sws_ctx )
        sws_freeContext( handle->yuv420p8_scaler.sws_ctx );
    handle->yuv420p8_scaler.sws_ctx = NULL;
    handle->yuv420p8_scaler_inited = 0;

    lwlibav_audio_free_decode_handler_ptr( &handle->adhp );
    lwlibav_audio_free_output_handler_ptr( &handle->aohp );
    lw_freep( &handle->lwh.file_path );

    if( handle->last_error )
        free( handle->last_error );
    handle->last_error = NULL;

#ifdef _WIN32
    if( handle->error_lock_inited )
    {
        DeleteCriticalSection( &handle->error_lock );
        handle->error_lock_inited = 0;
    }
#endif

    lw_free( handle );
}

LSMAS_NATIVE_API int lsmas_video_get_info( lsmas_handle_t *handle, lsmas_video_info_t *out_info, char **error_message )
{
    if( !handle || !out_info )
    {
        set_error_message( error_message, "handle/out_info is NULL." );
        return -1;
    }
    *out_info = handle->info;
    return 0;
}

LSMAS_NATIVE_API int lsmas_video_get_time_base( lsmas_handle_t *handle, int32_t *out_num, int32_t *out_den, char **error_message )
{
    if( !handle || !out_num || !out_den )
    {
        set_error_message( error_message, "handle/out_num/out_den is NULL." );
        return -1;
    }
    if( !handle->vdhp || !handle->vdhp->format || handle->vdhp->stream_index < 0 )
    {
        set_error_message( error_message, "Invalid internal state (no stream selected)." );
        return -1;
    }
    AVStream *st = handle->vdhp->format->streams[ handle->vdhp->stream_index ];
    if( !st || st->time_base.den == 0 )
    {
        set_error_message( error_message, "Invalid stream time base." );
        return -1;
    }
    *out_num = (int32_t)st->time_base.num;
    *out_den = (int32_t)st->time_base.den;
    return 0;
}

LSMAS_NATIVE_API int lsmas_video_get_stream_props( lsmas_handle_t *handle, lsmas_video_props_t *out_props, char **error_message )
{
    if( !handle || !out_props )
    {
        set_error_message( error_message, "handle/out_props is NULL." );
        return -1;
    }

    AVStream *st = get_selected_stream( handle, error_message );
    if( !st )
        return -1;

    fill_stream_props( out_props, st );
    return 0;
}

static uint8_t *get_stream_side_data_compat( const AVStream *st, enum AVPacketSideDataType type, int *size )
{
    if( !st || !size )
        return NULL;

#if LSMAS_LSW_VARIANT_HOE
    if( !st->codecpar )
    {
        *size = 0;
        return NULL;
    }
    const AVPacketSideData *sd = av_packet_side_data_get( st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data, type );
    if( !sd )
    {
        *size = 0;
        return NULL;
    }
    if( sd->size > (size_t)INT_MAX )
    {
        *size = 0;
        return NULL;
    }
    *size = (int)sd->size;
    return sd->data;
#else
    return av_stream_get_side_data( st, type, size );
#endif
}

LSMAS_NATIVE_API int lsmas_video_get_stream_mastering_display_metadata( lsmas_handle_t *handle, lsmas_mastering_display_metadata_t *out_metadata, char **error_message )
{
    if( !handle || !out_metadata )
    {
        set_error_message( error_message, "handle/out_metadata is NULL." );
        return -1;
    }

    memset( out_metadata, 0, sizeof(*out_metadata) );

    AVStream *st = get_selected_stream( handle, error_message );
    if( !st )
        return -1;

    int size = 0;
    uint8_t *sd = get_stream_side_data_compat( st, AV_PKT_DATA_MASTERING_DISPLAY_METADATA, &size );
    if( !sd || size < (int)sizeof(AVMasteringDisplayMetadata) )
        return 1;

    const AVMasteringDisplayMetadata *md = (const AVMasteringDisplayMetadata *)sd;
    out_metadata->has_primaries = (int32_t)md->has_primaries;
    out_metadata->has_luminance = (int32_t)md->has_luminance;

    out_metadata->primary_r_x = to_rational32( md->display_primaries[0][0] );
    out_metadata->primary_r_y = to_rational32( md->display_primaries[0][1] );
    out_metadata->primary_g_x = to_rational32( md->display_primaries[1][0] );
    out_metadata->primary_g_y = to_rational32( md->display_primaries[1][1] );
    out_metadata->primary_b_x = to_rational32( md->display_primaries[2][0] );
    out_metadata->primary_b_y = to_rational32( md->display_primaries[2][1] );

    out_metadata->white_x = to_rational32( md->white_point[0] );
    out_metadata->white_y = to_rational32( md->white_point[1] );

    out_metadata->max_luminance = to_rational32( md->max_luminance );
    out_metadata->min_luminance = to_rational32( md->min_luminance );

    return 0;
}

LSMAS_NATIVE_API int lsmas_video_get_stream_content_light_metadata( lsmas_handle_t *handle, lsmas_content_light_metadata_t *out_metadata, char **error_message )
{
    if( !handle || !out_metadata )
    {
        set_error_message( error_message, "handle/out_metadata is NULL." );
        return -1;
    }

    memset( out_metadata, 0, sizeof(*out_metadata) );

    AVStream *st = get_selected_stream( handle, error_message );
    if( !st )
        return -1;

    int size = 0;
    uint8_t *sd = get_stream_side_data_compat( st, AV_PKT_DATA_CONTENT_LIGHT_LEVEL, &size );
    if( !sd || size < (int)sizeof(AVContentLightMetadata) )
        return 1;

    const AVContentLightMetadata *cl = (const AVContentLightMetadata *)sd;
    out_metadata->max_cll  = (int32_t)cl->MaxCLL;
    out_metadata->max_fall = (int32_t)cl->MaxFALL;
    return 0;
}

LSMAS_NATIVE_API int lsmas_video_get_stream_dovi_conf( lsmas_handle_t *handle, lsmas_dovi_conf_t *out_conf, char **error_message )
{
    if( !handle || !out_conf )
    {
        set_error_message( error_message, "handle/out_conf is NULL." );
        return -1;
    }

    memset( out_conf, 0, sizeof(*out_conf) );

    AVStream *st = get_selected_stream( handle, error_message );
    if( !st )
        return -1;

    int size = 0;
    uint8_t *sd = get_stream_side_data_compat( st, AV_PKT_DATA_DOVI_CONF, &size );
    if( !sd || size < (int)sizeof(AVDOVIDecoderConfigurationRecord) )
        return 1;

    const AVDOVIDecoderConfigurationRecord *dc = (const AVDOVIDecoderConfigurationRecord *)sd;
    out_conf->dv_version_major = (int32_t)dc->dv_version_major;
    out_conf->dv_version_minor = (int32_t)dc->dv_version_minor;
    out_conf->dv_profile       = (int32_t)dc->dv_profile;
    out_conf->dv_level         = (int32_t)dc->dv_level;
    out_conf->rpu_present_flag = (int32_t)dc->rpu_present_flag;
    out_conf->el_present_flag  = (int32_t)dc->el_present_flag;
    out_conf->bl_present_flag  = (int32_t)dc->bl_present_flag;
    out_conf->dv_bl_signal_compatibility_id = (int32_t)dc->dv_bl_signal_compatibility_id;
    return 0;
}

LSMAS_NATIVE_API int lsmas_video_seek_frame( lsmas_handle_t *handle, int32_t frame_index, char **error_message )
{
    if( !handle )
    {
        set_error_message( error_message, "handle is NULL." );
        return -1;
    }
    if( frame_index < 0 )
    {
        set_error_message( error_message, "frame_index is negative." );
        return -1;
    }
    if( frame_index >= handle->info.num_frames )
    {
        set_error_message( error_message, "frame_index out of range." );
        return -1;
    }

    uint32_t frame_number = (uint32_t)frame_index + 1; /* lwlibav is 1-based */
    if( lwlibav_video_get_frame( handle->vdhp, handle->vohp, frame_number ) < 0 )
    {
        set_error_message( error_message, handle->last_error ? handle->last_error : "Failed to seek/decode frame." );
        return -1;
    }
    return 0;
}

LSMAS_NATIVE_API int lsmas_video_flush( lsmas_handle_t *handle, char **error_message )
{
    (void)error_message;
    if( !handle || !handle->vdhp )
    {
        set_error_message( error_message, "handle is NULL or not initialized." );
        return -1;
    }

    lwlibav_video_force_seek( handle->vdhp );
    if( handle->vdhp->ctx )
        avcodec_flush_buffers( handle->vdhp->ctx );
    return 0;
}

LSMAS_NATIVE_API int lsmas_video_get_frame_props( lsmas_handle_t *handle, int32_t frame_index, lsmas_video_props_t *out_props, char **error_message )
{
    if( !handle || !out_props )
    {
        set_error_message( error_message, "handle/out_props is NULL." );
        return -1;
    }
    if( frame_index < 0 )
    {
        set_error_message( error_message, "frame_index is negative." );
        return -1;
    }
    if( frame_index >= handle->info.num_frames )
    {
        set_error_message( error_message, "frame_index out of range." );
        return -1;
    }

    AVStream *st = get_selected_stream( handle, error_message );
    if( !st )
        return -1;

    fill_stream_props( out_props, st );

    uint32_t frame_number = (uint32_t)frame_index + 1; /* lwlibav is 1-based */
    if( lwlibav_video_get_frame( handle->vdhp, handle->vohp, frame_number ) < 0 )
    {
        set_error_message( error_message, handle->last_error ? handle->last_error : "Failed to decode frame." );
        return -1;
    }

    AVFrame *av_frame = lwlibav_video_get_frame_buffer( handle->vdhp );
    if( !av_frame )
    {
        set_error_message( error_message, "Failed to get frame buffer." );
        return -1;
    }

#if LSMAS_LSW_VARIANT_HOE
    out_props->interlaced_frame = (int32_t)((av_frame->flags & AV_FRAME_FLAG_INTERLACED) != 0);
    out_props->top_field_first  = (int32_t)((av_frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0);
#else
    out_props->interlaced_frame = (int32_t)av_frame->interlaced_frame;
    out_props->top_field_first  = (int32_t)av_frame->top_field_first;
#endif

    AVRational sar = av_frame->sample_aspect_ratio;
    if( is_valid_rational( sar ) )
    {
        out_props->sar_num = sar.num;
        out_props->sar_den = sar.den;
    }

    out_props->color_range     = (int32_t)av_frame->color_range;
    out_props->colorspace      = (int32_t)av_frame->colorspace;
    out_props->color_primaries = (int32_t)av_frame->color_primaries;
    out_props->color_trc       = (int32_t)av_frame->color_trc;
    out_props->chroma_location = (int32_t)av_frame->chroma_location;

    return 0;
}

LSMAS_NATIVE_API int64_t lsmas_video_get_frame_pts( lsmas_handle_t *handle, int32_t frame_index, char **error_message )
{
    if( !handle || !handle->vdhp || !handle->vdhp->frame_list )
    {
        set_error_message( error_message, "handle is NULL or not initialized." );
        return -1;
    }
    if( frame_index < 0 )
    {
        set_error_message( error_message, "frame_index is negative." );
        return -1;
    }
    if( frame_index >= handle->info.num_frames )
    {
        set_error_message( error_message, "frame_index out of range." );
        return -1;
    }
    uint32_t frame_number = (uint32_t)frame_index + 1; /* frame_list is 1-based */
    return get_video_output_pts( handle, frame_number );
}

LSMAS_NATIVE_API int32_t lsmas_video_get_pts_list( lsmas_handle_t *handle, int64_t *out_pts, int32_t out_count, char **error_message )
{
    if( !handle || !handle->vdhp || !handle->vdhp->frame_list )
    {
        set_error_message( error_message, "handle is NULL or not initialized." );
        return -1;
    }

    int32_t required = handle->info.num_frames;
    if( !out_pts || out_count <= 0 )
        return required;

    int32_t n = out_count < required ? out_count : required;
    for( int32_t i = 0; i < n; i++ )
        out_pts[i] = get_video_output_pts( handle, (uint32_t)i + 1 );
    return n;
}

LSMAS_NATIVE_API int32_t lsmas_video_get_source_frame_count( lsmas_handle_t *handle, int32_t *out_count, char **error_message )
{
    if( !handle || !out_count || !handle->vdhp || !handle->vdhp->frame_list )
    {
        set_error_message( error_message, "handle/out_count is NULL or not initialized." );
        return -1;
    }

    *out_count = (int32_t)handle->vdhp->frame_count;
    return 0;
}

LSMAS_NATIVE_API int32_t lsmas_video_get_source_pict_type_list( lsmas_handle_t *handle, int8_t *out_types, int32_t out_count, char **error_message )
{
    if( !handle || !handle->vdhp || !handle->vdhp->frame_list )
    {
        set_error_message( error_message, "handle is NULL or not initialized." );
        return -1;
    }

    int32_t required = (int32_t)handle->vdhp->frame_count;
    if( !out_types || out_count <= 0 )
        return required;

    int32_t n = out_count < required ? out_count : required;
    const video_frame_info_t *info = &handle->vdhp->frame_list[1]; /* 1-based index */
    for( int32_t i = 0; i < n; i++ )
        out_types[i] = (int8_t)info[i].pict_type;
    return n;
}

LSMAS_NATIVE_API int32_t lsmas_video_get_source_keyframe_flags( lsmas_handle_t *handle, uint8_t *out_flags, int32_t out_count, char **error_message )
{
    if( !handle || !handle->vdhp || !handle->vdhp->frame_list )
    {
        set_error_message( error_message, "handle is NULL or not initialized." );
        return -1;
    }

    int32_t required = (int32_t)handle->vdhp->frame_count;
    if( !out_flags || out_count <= 0 )
        return required;

    int32_t n = out_count < required ? out_count : required;
    const video_frame_info_t *info = &handle->vdhp->frame_list[1]; /* 1-based index */
    for( int32_t i = 0; i < n; i++ )
        out_flags[i] = (uint8_t)((info[i].flags & LW_VFRAME_FLAG_KEY) ? 1 : 0);
    return n;
}

static int64_t calc_gray8_required( int width, int height, int stride )
{
    return (int64_t)stride * (int64_t)height;
}

static int64_t calc_gray8_padded16_required( int width, int height, int stride )
{
    int rounded_height = (height + 15) & ~15;
    return (int64_t)stride * (int64_t)rounded_height;
}

static int64_t calc_yuv420p8_required( int width, int height, int y_stride )
{
    if( width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0 || y_stride < width || (y_stride & 1) != 0 )
        return -1;
    const int uv_stride = y_stride / 2;
    return (int64_t)y_stride * (int64_t)height
         + 2 * (int64_t)uv_stride * (int64_t)(height / 2);
}

static void copy_plane_rows(
    uint8_t *dst,
    int dst_stride,
    const uint8_t *src,
    int src_stride,
    int row_bytes,
    int height
)
{
    if( dst_stride == row_bytes && src_stride == row_bytes )
    {
        memcpy( dst, src, (size_t)row_bytes * (size_t)height );
        return;
    }

    for( int y = 0; y < height; y++ )
        memcpy( dst + (int64_t)y * (int64_t)dst_stride,
                src + (int64_t)y * (int64_t)src_stride,
                (size_t)row_bytes );
}

static int is_yuv420p8_direct_copy_format( enum AVPixelFormat fmt )
{
    switch( fmt )
    {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUVA420P:
            return 1;
        default:
            return 0;
    }
}

static int get_yuv420p16le_shift( enum AVPixelFormat fmt, int *out_shift )
{
    switch( fmt )
    {
        case AV_PIX_FMT_YUV420P9LE:
        case AV_PIX_FMT_YUVA420P9LE:
            *out_shift = 1;
            return 1;
        case AV_PIX_FMT_YUV420P10LE:
        case AV_PIX_FMT_YUVA420P10LE:
            *out_shift = 2;
            return 1;
        case AV_PIX_FMT_YUV420P12LE:
            *out_shift = 4;
            return 1;
        case AV_PIX_FMT_YUV420P14LE:
            *out_shift = 6;
            return 1;
        case AV_PIX_FMT_YUV420P16LE:
        case AV_PIX_FMT_YUVA420P16LE:
            *out_shift = 8;
            return 1;
        default:
            return 0;
    }
}

static int get_yuv420sp8_order( enum AVPixelFormat fmt, int *out_v_first )
{
    switch( fmt )
    {
        case AV_PIX_FMT_NV12:
            *out_v_first = 0;
            return 1;
        case AV_PIX_FMT_NV21:
            *out_v_first = 1;
            return 1;
        default:
            return 0;
    }
}

static int is_yuv420sp16le_msb_format( enum AVPixelFormat fmt )
{
    switch( fmt )
    {
        case AV_PIX_FMT_P010LE:
#if LIBAVUTIL_VERSION_MAJOR >= 58
        case AV_PIX_FMT_P012LE:
#endif
        case AV_PIX_FMT_P016LE:
            return 1;
        default:
            return 0;
    }
}

static void convert_plane16le_to8(
    uint8_t *restrict dst,
    int dst_stride,
    const uint8_t *restrict src,
    int src_stride,
    int width,
    int height,
    int shift
)
{
    for( int y = 0; y < height; y++ )
    {
        uint8_t *restrict out_row = dst + (int64_t)y * (int64_t)dst_stride;
        const uint16_t *restrict in_row = (const uint16_t *)(src + (int64_t)y * (int64_t)src_stride);
        for( int x = 0; x < width; x++ )
            out_row[x] = (uint8_t)(in_row[x] >> shift);
    }
}

static void deinterleave_yuv420sp8(
    uint8_t *restrict dst_u,
    uint8_t *restrict dst_v,
    int dst_stride,
    const uint8_t *restrict src_uv,
    int src_stride,
    int chroma_width,
    int chroma_height,
    int v_first
)
{
    for( int y = 0; y < chroma_height; y++ )
    {
        uint8_t *restrict out_u = dst_u + (int64_t)y * (int64_t)dst_stride;
        uint8_t *restrict out_v = dst_v + (int64_t)y * (int64_t)dst_stride;
        const uint8_t *restrict in_uv = src_uv + (int64_t)y * (int64_t)src_stride;
        for( int x = 0; x < chroma_width; x++ )
        {
            out_u[x] = in_uv[2 * x + v_first];
            out_v[x] = in_uv[2 * x + (1 - v_first)];
        }
    }
}

static void deinterleave_yuv420sp16le_msb_to8(
    uint8_t *restrict dst_u,
    uint8_t *restrict dst_v,
    int dst_stride,
    const uint8_t *restrict src_uv,
    int src_stride,
    int chroma_width,
    int chroma_height
)
{
    for( int y = 0; y < chroma_height; y++ )
    {
        uint8_t *restrict out_u = dst_u + (int64_t)y * (int64_t)dst_stride;
        uint8_t *restrict out_v = dst_v + (int64_t)y * (int64_t)dst_stride;
        const uint16_t *restrict in_uv = (const uint16_t *)(src_uv + (int64_t)y * (int64_t)src_stride);
        for( int x = 0; x < chroma_width; x++ )
        {
            out_u[x] = (uint8_t)(in_uv[2 * x] >> 8);
            out_v[x] = (uint8_t)(in_uv[2 * x + 1] >> 8);
        }
    }
}

static int is_gray8_direct_copy_format( enum AVPixelFormat fmt )
{
    switch( fmt )
    {
        case AV_PIX_FMT_GRAY8:
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21:
        case AV_PIX_FMT_YUV410P:
        case AV_PIX_FMT_YUV411P:
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV440P:
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUVJ422P:
        case AV_PIX_FMT_YUVJ440P:
        case AV_PIX_FMT_YUVJ444P:
            return 1;
        default:
            return 0;
    }
}

static int is_gray8_direct_convert16le_format( enum AVPixelFormat fmt, int *out_shift, int *out_msb_aligned )
{
    /* For these formats we can derive an 8-bit luma plane by downshifting 16-bit samples,
     * without going through swscale. This is a performance optimization for 10/12/14/16-bit sources.
     *
     * NOTE: Behavior matches the existing 8-bit fast path: we take the raw decoded luma plane
     * (no range expansion) and do a simple bit-depth downshift with rounding. */
    switch( fmt )
    {
        /* Planar luma is LSB-aligned in 16-bit words. */
        case AV_PIX_FMT_YUV420P10LE:
        case AV_PIX_FMT_YUV422P10LE:
        case AV_PIX_FMT_YUV444P10LE:
            *out_shift = 2;
            *out_msb_aligned = 0;
            return 1;

        /* Semi-planar formats: 16-bit words with samples in the MSBs (P010/P016). */
        case AV_PIX_FMT_P010LE:
        case AV_PIX_FMT_P016LE:
            *out_shift = 0;
            *out_msb_aligned = 1;
            return 1;

        default:
            return 0;
    }
}

static void gray8_convert16le_downshift_and_pad(
    uint8_t *dst,
    int dst_stride,
    const uint8_t *src_plane0,
    int src_linesize0,
    int width,
    int height,
    int rounded_width,
    int rounded_height,
    int shift,
    int msb_aligned
)
{
    /* Convert visible rows + right pad. */
    if( msb_aligned )
    {
        for( int y = 0; y < height; y++ )
        {
            uint8_t *out_row = dst + (int64_t)y * (int64_t)dst_stride;
            const uint16_t *in_row = (const uint16_t *)(src_plane0 + (int64_t)y * (int64_t)src_linesize0);
            for( int x = 0; x < width; x++ )
            {
                /* MSB-aligned 16-bit (e.g. P010/P016): take the top 8 bits. */
                out_row[x] = (uint8_t)(in_row[x] >> 8);
            }

            if( rounded_width > width )
                memset( out_row + width, out_row[width - 1], (size_t)(rounded_width - width) );
        }
    }
    else
    {
        for( int y = 0; y < height; y++ )
        {
            uint8_t *out_row = dst + (int64_t)y * (int64_t)dst_stride;
            const uint16_t *in_row = (const uint16_t *)(src_plane0 + (int64_t)y * (int64_t)src_linesize0);
            for( int x = 0; x < width; x++ )
            {
                /* LSB-aligned 10-bit planar: downshift into 8-bit. */
                out_row[x] = (uint8_t)(in_row[x] >> (uint32_t)shift);
            }

            if( rounded_width > width )
                memset( out_row + width, out_row[width - 1], (size_t)(rounded_width - width) );
        }
    }

    /* Bottom pad (repeat last row including right padding). */
    if( rounded_height > height )
    {
        const uint8_t *last = dst + (int64_t)(height - 1) * (int64_t)dst_stride;
        for( int y = height; y < rounded_height; y++ )
            memcpy( dst + (int64_t)y * (int64_t)dst_stride, last, (size_t)rounded_width );
    }
}

static int64_t copy_frame_gray8(
    lsmas_handle_t *handle,
    const AVFrame *av_frame,
    uint8_t *dst,
    int32_t dst_stride,
    char **error_message
)
{
    if( !handle || !av_frame )
    {
        set_error_message( error_message, "handle/frame is NULL." );
        return -1;
    }

    const int width  = av_frame->width;
    const int height = av_frame->height;
    if( width <= 0 || height <= 0 )
    {
        set_error_message( error_message, "Decoded frame has invalid dimensions." );
        return -1;
    }

    int stride = dst_stride > 0 ? dst_stride : width;
    const int64_t required = calc_gray8_required( width, height, stride );
    if( !dst )
        return required;

    if( stride < width )
    {
        set_error_message( error_message, "dst_stride is too small for GRAY8." );
        return -1;
    }

    /* Fast path: when decoded frame exposes an 8-bit luma plane, just copy Y.
     * This avoids swscale and is significantly faster for typical YUV420P/NV12 decode outputs. */
    if( is_gray8_direct_copy_format( (enum AVPixelFormat)av_frame->format )
        && av_frame->data[0]
        && av_frame->linesize[0] >= width )
    {
        if( stride == width && av_frame->linesize[0] == width )
        {
            memcpy( dst, av_frame->data[0], (size_t)width * (size_t)height );
        }
        else
        {
            for( int y = 0; y < height; y++ )
                memcpy( dst + (int64_t)y * (int64_t)stride,
                        av_frame->data[0] + (int64_t)y * (int64_t)av_frame->linesize[0],
                        (size_t)width );
        }
        return required;
    }

    /* Fast path: for common 10/12/14/16-bit YUV/P010 sources, downshift luma directly (no swscale). */
    {
        int shift = 0;
        int msb_aligned = 0;
        if( is_gray8_direct_convert16le_format( (enum AVPixelFormat)av_frame->format, &shift, &msb_aligned )
            && av_frame->data[0]
            && av_frame->linesize[0] >= width * 2 )
        {
            gray8_convert16le_downshift_and_pad( dst, stride,
                                                 av_frame->data[0], av_frame->linesize[0],
                                                 width, height, width, height,
                                                 shift, msb_aligned );
            return required;
        }
    }

    init_gray8_scaler_if_needed( handle );

    lw_log_handler_t *lhp = lwlibav_video_get_log_handler( handle->vdhp );
    if( !lhp )
    {
        set_error_message( error_message, "Invalid internal state (no log handler)." );
        return -1;
    }

    if( update_scaler_configuration_if_needed( &handle->gray8_scaler, lhp, av_frame ) < 0 || !handle->gray8_scaler.sws_ctx )
    {
        set_error_message( error_message, handle->last_error ? handle->last_error : "Failed to initialize GRAY8 scaler." );
        return -1;
    }

    uint8_t *dst_data[4] = { dst, NULL, NULL, NULL };
    int dst_linesize[4] = { stride, 0, 0, 0 };
    sws_scale( handle->gray8_scaler.sws_ctx,
               (const uint8_t *const *)av_frame->data,
               av_frame->linesize,
               0,
               height,
               dst_data,
               dst_linesize );

    return required;
}

static int64_t copy_frame_yuv420p8(
    lsmas_handle_t *handle,
    const AVFrame *av_frame,
    uint8_t *dst,
    int32_t dst_y_stride,
    char **error_message
)
{
    if( !handle || !av_frame )
    {
        set_error_message( error_message, "handle/frame is NULL." );
        return -1;
    }

    const int width = av_frame->width;
    const int height = av_frame->height;
    if( width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0 )
    {
        set_error_message( error_message, "YUV420P8 output requires positive even frame dimensions." );
        return -1;
    }

    const int y_stride = dst_y_stride > 0 ? dst_y_stride : width;
    const int64_t required = calc_yuv420p8_required( width, height, y_stride );
    if( required < 0 )
    {
        set_error_message( error_message, "YUV420P8 dst_stride must be even and at least the visible width." );
        return -1;
    }
    if( !dst )
        return required;

    const int uv_stride = y_stride / 2;
    uint8_t *dst_data[4] = {
        dst,
        dst + (int64_t)y_stride * (int64_t)height,
        dst + (int64_t)y_stride * (int64_t)height + (int64_t)uv_stride * (int64_t)(height / 2),
        NULL
    };
    int dst_linesize[4] = { y_stride, uv_stride, uv_stride, 0 };

    if( is_yuv420p8_direct_copy_format( (enum AVPixelFormat)av_frame->format )
        && av_frame->data[0] && av_frame->data[1] && av_frame->data[2]
        && av_frame->linesize[0] >= width
        && av_frame->linesize[1] >= width / 2
        && av_frame->linesize[2] >= width / 2 )
    {
        copy_plane_rows( dst_data[0], y_stride,
                         av_frame->data[0], av_frame->linesize[0],
                         width, height );
        copy_plane_rows( dst_data[1], uv_stride,
                         av_frame->data[1], av_frame->linesize[1],
                         width / 2, height / 2 );
        copy_plane_rows( dst_data[2], uv_stride,
                         av_frame->data[2], av_frame->linesize[2],
                         width / 2, height / 2 );
        return required;
    }

    /* NV12/NV21 already has the target sampling and depth. Copy Y and split UV/VU
     * directly instead of paying for a general swscale conversion. */
    {
        int v_first = 0;
        if( get_yuv420sp8_order( (enum AVPixelFormat)av_frame->format, &v_first )
            && av_frame->data[0] && av_frame->data[1]
            && av_frame->linesize[0] >= width
            && av_frame->linesize[1] >= width )
        {
            copy_plane_rows( dst_data[0], y_stride,
                             av_frame->data[0], av_frame->linesize[0],
                             width, height );
            deinterleave_yuv420sp8( dst_data[1], dst_data[2], uv_stride,
                                    av_frame->data[1], av_frame->linesize[1],
                                    width / 2, height / 2, v_first );
            return required;
        }
    }

    /* Planar high-bit-depth 4:2:0 stores samples LSB-aligned in 16-bit words.
     * Truncating the low bits matches the existing GRAY8 fast-path semantics. */
    {
        int shift = 0;
        if( get_yuv420p16le_shift( (enum AVPixelFormat)av_frame->format, &shift )
            && av_frame->data[0] && av_frame->data[1] && av_frame->data[2]
            && av_frame->linesize[0] >= width * 2
            && av_frame->linesize[1] >= width
            && av_frame->linesize[2] >= width )
        {
            convert_plane16le_to8( dst_data[0], y_stride,
                                   av_frame->data[0], av_frame->linesize[0],
                                   width, height, shift );
            convert_plane16le_to8( dst_data[1], uv_stride,
                                   av_frame->data[1], av_frame->linesize[1],
                                   width / 2, height / 2, shift );
            convert_plane16le_to8( dst_data[2], uv_stride,
                                   av_frame->data[2], av_frame->linesize[2],
                                   width / 2, height / 2, shift );
            return required;
        }
    }

    /* P010/P012/P016 stores 16-bit Y and interleaved UV with meaningful bits in
     * the MSBs. Taking the top byte produces 8-bit I420 without resampling. */
    if( is_yuv420sp16le_msb_format( (enum AVPixelFormat)av_frame->format )
        && av_frame->data[0] && av_frame->data[1]
        && av_frame->linesize[0] >= width * 2
        && av_frame->linesize[1] >= width * 2 )
    {
        convert_plane16le_to8( dst_data[0], y_stride,
                               av_frame->data[0], av_frame->linesize[0],
                               width, height, 8 );
        deinterleave_yuv420sp16le_msb_to8( dst_data[1], dst_data[2], uv_stride,
                                           av_frame->data[1], av_frame->linesize[1],
                                           width / 2, height / 2 );
        return required;
    }

    init_yuv420p8_scaler_if_needed( handle );
    lw_log_handler_t *lhp = lwlibav_video_get_log_handler( handle->vdhp );
    if( !lhp )
    {
        set_error_message( error_message, "Invalid internal state (no log handler)." );
        return -1;
    }
    if( update_scaler_configuration_if_needed( &handle->yuv420p8_scaler, lhp, av_frame ) < 0
        || !handle->yuv420p8_scaler.sws_ctx )
    {
        set_error_message( error_message, handle->last_error ? handle->last_error : "Failed to initialize YUV420P8 scaler." );
        return -1;
    }

    if( sws_scale( handle->yuv420p8_scaler.sws_ctx,
                   (const uint8_t *const *)av_frame->data,
                   av_frame->linesize,
                   0,
                   height,
                   dst_data,
                   dst_linesize ) != height )
    {
        set_error_message( error_message, "Failed to convert frame to YUV420P8." );
        return -1;
    }
    return required;
}

static int64_t copy_frame_gray8_padded16(
    lsmas_handle_t *handle,
    const AVFrame *av_frame,
    uint8_t *dst,
    int32_t dst_stride,
    char **error_message
)
{
    if( !handle || !av_frame )
    {
        set_error_message( error_message, "handle/frame is NULL." );
        return -1;
    }

    const int width  = av_frame->width;
    const int height = av_frame->height;
    if( width <= 0 || height <= 0 )
    {
        set_error_message( error_message, "Decoded frame has invalid dimensions." );
        return -1;
    }

    const int rounded_width  = (width  + 15) & ~15;
    const int rounded_height = (height + 15) & ~15;

    int stride = dst_stride > 0 ? dst_stride : rounded_width;
    const int64_t required = calc_gray8_padded16_required( width, height, stride );
    if( !dst )
        return required;

    if( stride < rounded_width )
    {
        set_error_message( error_message, "dst_stride is too small for GRAY8 padded16." );
        return -1;
    }

    /* Fast path: direct copy of Y plane, then do right/bottom replication padding. */
    if( is_gray8_direct_copy_format( (enum AVPixelFormat)av_frame->format )
        && av_frame->data[0]
        && av_frame->linesize[0] >= width )
    {
        if( width == rounded_width && height == rounded_height
            && stride == width && av_frame->linesize[0] == width )
        {
            memcpy( dst, av_frame->data[0], (size_t)width * (size_t)height );
            return required;
        }

        /* Copy visible rows + right pad */
        for( int y = 0; y < height; y++ )
        {
            uint8_t *out_row = dst + (int64_t)y * (int64_t)stride;
            const uint8_t *in_row = av_frame->data[0] + (int64_t)y * (int64_t)av_frame->linesize[0];
            memcpy( out_row, in_row, (size_t)width );

            if( rounded_width > width )
                memset( out_row + width, out_row[width - 1], (size_t)(rounded_width - width) );
        }

        /* Bottom pad (repeat last row including right padding) */
        if( rounded_height > height )
        {
            const uint8_t *last = dst + (int64_t)(height - 1) * (int64_t)stride;
            for( int y = height; y < rounded_height; y++ )
                memcpy( dst + (int64_t)y * (int64_t)stride, last, (size_t)rounded_width );
        }

        return required;
    }

    /* Fast path: for common 10/12/14/16-bit YUV/P010 sources, downshift luma directly (no swscale),
     * then do right/bottom replication padding. */
    {
        int shift = 0;
        int msb_aligned = 0;
        if( is_gray8_direct_convert16le_format( (enum AVPixelFormat)av_frame->format, &shift, &msb_aligned )
            && av_frame->data[0]
            && av_frame->linesize[0] >= width * 2 )
        {
            gray8_convert16le_downshift_and_pad( dst, stride,
                                                 av_frame->data[0], av_frame->linesize[0],
                                                 width, height, rounded_width, rounded_height,
                                                 shift, msb_aligned );
            return required;
        }
    }

    /* Fallback: swscale -> GRAY8 for visible height, then pad. */
    init_gray8_scaler_if_needed( handle );

    lw_log_handler_t *lhp = lwlibav_video_get_log_handler( handle->vdhp );
    if( !lhp )
    {
        set_error_message( error_message, "Invalid internal state (no log handler)." );
        return -1;
    }

    if( update_scaler_configuration_if_needed( &handle->gray8_scaler, lhp, av_frame ) < 0 || !handle->gray8_scaler.sws_ctx )
    {
        set_error_message( error_message, handle->last_error ? handle->last_error : "Failed to initialize GRAY8 scaler." );
        return -1;
    }

    uint8_t *dst_data[4] = { dst, NULL, NULL, NULL };
    int dst_linesize[4] = { stride, 0, 0, 0 };
    sws_scale( handle->gray8_scaler.sws_ctx,
               (const uint8_t *const *)av_frame->data,
               av_frame->linesize,
               0,
               height,
               dst_data,
               dst_linesize );

    /* Right pad after swscale */
    if( rounded_width > width )
    {
        for( int y = 0; y < height; y++ )
        {
            uint8_t *out_row = dst + (int64_t)y * (int64_t)stride;
            memset( out_row + width, out_row[width - 1], (size_t)(rounded_width - width) );
        }
    }

    /* Bottom pad */
    if( rounded_height > height )
    {
        const uint8_t *last = dst + (int64_t)(height - 1) * (int64_t)stride;
        for( int y = height; y < rounded_height; y++ )
            memcpy( dst + (int64_t)y * (int64_t)stride, last, (size_t)rounded_width );
    }

    return required;
}

LSMAS_NATIVE_API int lsmas_video_acquire_avframe(
    lsmas_handle_t *handle,
    int32_t frame_index,
    lsmas_video_frame_t **out_frame,
    char **error_message
)
{
    if( !handle )
    {
        set_error_message( error_message, "handle is NULL." );
        return -1;
    }
    if( !out_frame )
    {
        set_error_message( error_message, "out_frame is NULL." );
        return -1;
    }
    *out_frame = NULL;

    if( frame_index < 0 )
    {
        set_error_message( error_message, "frame_index is negative." );
        return -1;
    }
    if( frame_index >= handle->info.num_frames )
    {
        set_error_message( error_message, "frame_index out of range." );
        return -1;
    }

    uint32_t frame_number = (uint32_t)frame_index + 1; /* lwlibav is 1-based */
    if( lwlibav_video_get_frame( handle->vdhp, handle->vohp, frame_number ) < 0 )
    {
        set_error_message( error_message, handle->last_error ? handle->last_error : "Failed to decode frame." );
        return -1;
    }

    AVFrame *src = lwlibav_video_get_frame_buffer( handle->vdhp );
    if( !src )
    {
        set_error_message( error_message, "Failed to get frame buffer." );
        return -1;
    }

    lsmas_video_frame_t *h = (lsmas_video_frame_t *)malloc( sizeof(lsmas_video_frame_t) );
    if( !h )
    {
        set_error_message( error_message, "Out of memory." );
        return -1;
    }
    memset( h, 0, sizeof(*h) );

    h->frame = av_frame_alloc();
    if( !h->frame )
    {
        free( h );
        set_error_message( error_message, "av_frame_alloc failed." );
        return -1;
    }

    if( av_frame_ref( h->frame, src ) < 0 )
    {
        av_frame_free( &h->frame );
        free( h );
        set_error_message( error_message, "av_frame_ref failed (frame may not be refcounted/CPU-accessible)." );
        return -1;
    }

    *out_frame = h;
    return 0;
}

LSMAS_NATIVE_API const void *lsmas_video_frame_get_avframe( const lsmas_video_frame_t *frame )
{
    return frame ? (const void *)frame->frame : NULL;
}

LSMAS_NATIVE_API const char *lsmas_video_frame_get_pix_fmt_name( const lsmas_video_frame_t *frame )
{
    if( !frame || !frame->frame )
        return NULL;
    return av_get_pix_fmt_name( (enum AVPixelFormat)frame->frame->format );
}

LSMAS_NATIVE_API int lsmas_video_frame_get_props(
    const lsmas_video_frame_t *frame,
    lsmas_video_frame_props_t *out_props,
    char **error_message
)
{
    if( !frame || !frame->frame || !out_props )
    {
        set_error_message( error_message, "frame/out_props is NULL." );
        return -1;
    }

    fill_frame_props_from_avframe( frame->frame, out_props );
    return 0;
}

LSMAS_NATIVE_API int lsmas_video_frame_get_format_info(
    const lsmas_video_frame_t *frame,
    lsmas_video_format_info_t *out_info,
    char **error_message
)
{
    if( !frame || !frame->frame || !out_info )
    {
        set_error_message( error_message, "frame/out_info is NULL." );
        return -1;
    }

    if( !fill_video_format_info_from_pix_fmt( (enum AVPixelFormat)frame->frame->format, frame->frame->width, frame->frame->height, out_info ) )
    {
        set_error_message( error_message, "Unsupported or unknown AVFrame pixel format." );
        return -1;
    }

    return 0;
}

LSMAS_NATIVE_API int lsmas_video_frame_get_plane(
    const lsmas_video_frame_t *frame,
    int32_t plane_index,
    const uint8_t **out_data,
    int32_t *out_stride,
    int32_t *out_width,
    int32_t *out_height,
    char **error_message
)
{
    if( !frame || !frame->frame )
    {
        set_error_message( error_message, "frame is NULL." );
        return -1;
    }
    if( plane_index < 0 || plane_index >= 4 )
    {
        set_error_message( error_message, "plane_index out of range." );
        return -1;
    }

    lsmas_video_format_info_t format_info;
    if( !fill_video_format_info_from_pix_fmt( (enum AVPixelFormat)frame->frame->format, frame->frame->width, frame->frame->height, &format_info ) )
    {
        set_error_message( error_message, "Unsupported or unknown AVFrame pixel format." );
        return -1;
    }
    if( plane_index >= format_info.plane_count )
    {
        set_error_message( error_message, "plane_index is not present in this pixel format." );
        return -1;
    }

    const uint8_t *data = (const uint8_t *)frame->frame->data[plane_index];
    if( !data )
    {
        set_error_message( error_message, "requested AVFrame plane is NULL." );
        return -1;
    }

    const lsmas_video_plane_format_t *plane = &format_info.planes[plane_index];
    int plane_w = div_round_up_i32( frame->frame->width, plane->width_divisor );
    int plane_h = div_round_up_i32( frame->frame->height, plane->height_divisor );

    if( out_data )
        *out_data = data;
    if( out_stride )
        *out_stride = (int32_t)frame->frame->linesize[plane_index];
    if( out_width )
        *out_width = plane_w;
    if( out_height )
        *out_height = plane_h;

    return 0;
}

LSMAS_NATIVE_API int lsmas_video_frame_get_side_data(
    const lsmas_video_frame_t *frame,
    lsmas_video_frame_side_data_type_t type,
    const uint8_t **out_data,
    int32_t *out_size,
    char **error_message
)
{
    if( out_data )
        *out_data = NULL;
    if( out_size )
        *out_size = 0;

    if( !frame || !frame->frame )
    {
        set_error_message( error_message, "frame is NULL." );
        return -1;
    }

    enum AVFrameSideDataType av_type;
    if( !side_data_type_to_avframe_type( type, &av_type ) )
    {
        set_error_message( error_message, "unsupported side data type." );
        return -1;
    }

    AVFrameSideData *sd = av_frame_get_side_data( frame->frame, av_type );
    if( !sd )
        return 0;
    if( sd->size > (size_t)INT_MAX )
    {
        set_error_message( error_message, "side data is too large." );
        return -1;
    }

    if( out_data )
        *out_data = sd->data;
    if( out_size )
        *out_size = (int32_t)sd->size;
    return 1;
}

LSMAS_NATIVE_API int lsmas_video_frame_get_dovi_metadata(
    const lsmas_video_frame_t *frame,
    lsmas_dovi_metadata_t *out_metadata,
    char **error_message
)
{
    if( out_metadata )
        memset( out_metadata, 0, sizeof(*out_metadata) );

    if( !frame || !frame->frame || !out_metadata )
    {
        set_error_message( error_message, "frame/out_metadata is NULL." );
        return -1;
    }

#if !LSMAS_LSW_VARIANT_HOE
    (void)frame;
    return 1;
#else
    AVFrameSideData *sd = av_frame_get_side_data( frame->frame, AV_FRAME_DATA_DOVI_METADATA );
    if( !sd || !sd->data )
        return 1;
    if( sd->size < sizeof(AVDOVIMetadata) )
    {
        set_error_message( error_message, "Dolby Vision metadata side data is too small." );
        return -1;
    }

    const AVDOVIMetadata *metadata = (const AVDOVIMetadata *)sd->data;
    size_t total = sd->size;
    if( !dovi_range_contains( total, metadata->header_offset, sizeof(AVDOVIRpuDataHeader) )
        || !dovi_range_contains( total, metadata->mapping_offset, sizeof(AVDOVIDataMapping) )
        || !dovi_range_contains( total, metadata->color_offset, sizeof(AVDOVIColorMetadata) ) )
    {
        set_error_message( error_message, "Dolby Vision metadata side data has invalid offsets." );
        return -1;
    }

    const AVDOVIRpuDataHeader *header = av_dovi_get_header( metadata );
    const AVDOVIDataMapping *mapping = av_dovi_get_mapping( metadata );
    const AVDOVIColorMetadata *color = av_dovi_get_color( metadata );
    if( !header || !mapping || !color )
    {
        set_error_message( error_message, "Dolby Vision metadata is incomplete." );
        return -1;
    }

    out_metadata->disable_residual_flag = header->disable_residual_flag != 0;
    out_metadata->bl_bit_depth = header->bl_bit_depth;
    out_metadata->coefficient_log2_denom = header->coef_log2_denom;
    if( !header->disable_residual_flag )
        return 0;

    out_metadata->valid = 1;
    for( int i = 0; i < 3; i++ )
        out_metadata->nonlinear_offset[i] = rational_to_float( color->ycc_to_rgb_offset[i] );
    for( int i = 0; i < 9; i++ )
    {
        out_metadata->nonlinear[i] = rational_to_float( color->ycc_to_rgb_matrix[i] );
        out_metadata->linear[i] = rational_to_float( color->rgb_to_lms_matrix[i] );
    }
    for( int c = 0; c < 3; c++ )
        fill_dovi_reshape_component( &out_metadata->comp[c], &mapping->curves[c], header );

    out_metadata->source_min_pq = (float)color->source_min_pq / 4095.0f;
    out_metadata->source_max_pq = (float)color->source_max_pq / 4095.0f;

    AVDOVIDmData *l1 = av_dovi_find_level( metadata, 1 );
    if( l1 )
    {
        out_metadata->has_l1 = 1;
        out_metadata->max_pq_y = (float)l1->l1.max_pq / 4095.0f;
        out_metadata->avg_pq_y = (float)l1->l1.avg_pq / 4095.0f;
    }

    return 0;
#endif
}

LSMAS_NATIVE_API void lsmas_video_release_frame( lsmas_video_frame_t *frame )
{
    if( !frame )
        return;
    if( frame->frame )
        av_frame_free( &frame->frame );
    free( frame );
}

static int64_t copy_frame_bgra(
    lsmas_handle_t *handle,
    const AVFrame *av_frame,
    uint8_t *dst,
    int32_t dst_stride,
    char **error_message
)
{
    if( !handle || !av_frame )
    {
        set_error_message( error_message, "handle/frame is NULL." );
        return -1;
    }

    const int width  = av_frame->width;
    const int height = av_frame->height;
    if( width <= 0 || height <= 0 )
    {
        set_error_message( error_message, "Decoded frame has invalid dimensions." );
        return -1;
    }

    int stride = dst_stride > 0 ? dst_stride : width * 4;
    const int64_t required = (int64_t)stride * (int64_t)(height - 1) + (int64_t)width * 4;
    if( !dst )
        return required;

    if( stride < width * 4 )
    {
        set_error_message( error_message, "dst_stride is too small for BGRA." );
        return -1;
    }

    uint8_t *dst_data[4] = { dst, NULL, NULL, NULL };
    int dst_linesize[4] = { stride, 0, 0, 0 };
    sws_scale( handle->vohp->scaler.sws_ctx,
               (const uint8_t *const *)av_frame->data,
               av_frame->linesize,
               0,
               height,
               dst_data,
               dst_linesize );

    return required;
}

static int64_t copy_frame_rgba(
    lsmas_handle_t *handle,
    const AVFrame *av_frame,
    uint8_t *dst,
    int32_t dst_stride,
    char **error_message
)
{
    if( !handle || !av_frame )
    {
        set_error_message( error_message, "handle/frame is NULL." );
        return -1;
    }

    const int width  = av_frame->width;
    const int height = av_frame->height;
    if( width <= 0 || height <= 0 )
    {
        set_error_message( error_message, "Decoded frame has invalid dimensions." );
        return -1;
    }

    int stride = dst_stride > 0 ? dst_stride : width * 4;
    const int64_t required = (int64_t)stride * (int64_t)(height - 1) + (int64_t)width * 4;
    if( !dst )
        return required;

    if( stride < width * 4 )
    {
        set_error_message( error_message, "dst_stride is too small for RGBA." );
        return -1;
    }

    init_rgba_scaler_if_needed( handle );

    lw_log_handler_t *lhp = lwlibav_video_get_log_handler( handle->vdhp );
    if( !lhp )
    {
        set_error_message( error_message, "Invalid internal state (no log handler)." );
        return -1;
    }

    if( update_scaler_configuration_if_needed( &handle->rgba_scaler, lhp, av_frame ) < 0 || !handle->rgba_scaler.sws_ctx )
    {
        set_error_message( error_message, handle->last_error ? handle->last_error : "Failed to initialize RGBA scaler." );
        return -1;
    }

    uint8_t *dst_data[4] = { dst, NULL, NULL, NULL };
    int dst_linesize[4] = { stride, 0, 0, 0 };
    sws_scale( handle->rgba_scaler.sws_ctx,
               (const uint8_t *const *)av_frame->data,
               av_frame->linesize,
               0,
               height,
               dst_data,
               dst_linesize );

    return required;
}

static int decode_frame_for_output( lsmas_handle_t *handle, int32_t frame_index, AVFrame **out_frame, char **error_message )
{
    if( out_frame )
        *out_frame = NULL;

    if( !handle )
    {
        set_error_message( error_message, "handle is NULL." );
        return -1;
    }
    if( !out_frame )
    {
        set_error_message( error_message, "out_frame is NULL." );
        return -1;
    }
    if( frame_index < 0 )
    {
        set_error_message( error_message, "frame_index is negative." );
        return -1;
    }
    if( frame_index >= handle->info.num_frames )
    {
        set_error_message( error_message, "frame_index out of range." );
        return -1;
    }

    uint32_t frame_number = (uint32_t)frame_index + 1; /* lwlibav is 1-based */
    if( lwlibav_video_get_frame( handle->vdhp, handle->vohp, frame_number ) < 0 )
    {
        set_error_message( error_message, handle->last_error ? handle->last_error : "Failed to decode frame." );
        return -1;
    }

    AVFrame *av_frame = lwlibav_video_get_frame_buffer( handle->vdhp );
    if( !av_frame )
    {
        set_error_message( error_message, "Failed to get frame buffer." );
        return -1;
    }
    if( av_frame->width <= 0 || av_frame->height <= 0 )
    {
        set_error_message( error_message, "Decoded frame has invalid dimensions." );
        return -1;
    }

    *out_frame = av_frame;
    return 0;
}

static int get_decoded_frame_after_output( lsmas_handle_t *handle, AVFrame **out_frame, char **error_message )
{
    if( out_frame )
        *out_frame = NULL;
    if( !handle || !out_frame )
    {
        set_error_message( error_message, "handle/out_frame is NULL." );
        return -1;
    }

    AVFrame *av_frame = lwlibav_video_get_frame_buffer( handle->vdhp );
    if( !av_frame )
    {
        set_error_message( error_message, "Failed to get frame buffer." );
        return -1;
    }
    if( av_frame->width <= 0 || av_frame->height <= 0 )
    {
        set_error_message( error_message, "Decoded frame has invalid dimensions." );
        return -1;
    }

    *out_frame = av_frame;
    return 0;
}

static int64_t calc_packed_stride_required( int width, int height, int stride, int bytes_per_pixel )
{
    if( height <= 0 )
        return -1;
    int64_t row_bytes = (int64_t)width * (int64_t)bytes_per_pixel;
    return (int64_t)stride * (int64_t)(height - 1) + row_bytes;
}

static void clear_frame_buffer_layout( lsmas_video_frame_buffer_layout_t *layout )
{
    if( layout )
        memset( layout, 0, sizeof(*layout) );
}

static int fill_native_frame_buffer_layout(
    const AVFrame *frame,
    lsmas_video_frame_buffer_layout_t *out_layout,
    int64_t *out_required,
    char **error_message
)
{
    if( out_required )
        *out_required = -1;
    clear_frame_buffer_layout( out_layout );

    if( !frame )
    {
        set_error_message( error_message, "frame is NULL." );
        return -1;
    }
    if( frame->width <= 0 || frame->height <= 0 )
    {
        set_error_message( error_message, "Decoded frame has invalid dimensions." );
        return -1;
    }

    enum AVPixelFormat pix_fmt = (enum AVPixelFormat)frame->format;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get( pix_fmt );
    if( !desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) )
    {
        set_error_message( error_message, "Decoded frame pixel format is not a CPU-copyable native image." );
        return -1;
    }

    int linesizes[4] = { 0, 0, 0, 0 };
    if( av_image_fill_linesizes( linesizes, pix_fmt, frame->width ) < 0 )
    {
        set_error_message( error_message, "Failed to compute native frame linesizes." );
        return -1;
    }

    int required = av_image_get_buffer_size( pix_fmt, frame->width, frame->height, 1 );
    if( required < 0 )
    {
        set_error_message( error_message, "Failed to compute native frame buffer size." );
        return -1;
    }

    int plane_count = av_pix_fmt_count_planes( pix_fmt );
    if( plane_count <= 0 || plane_count > 4 )
    {
        set_error_message( error_message, "Unsupported native frame plane count." );
        return -1;
    }

    if( out_required )
        *out_required = (int64_t)required;

    if( !out_layout )
        return 0;

    out_layout->output_format = LSMAS_VIDEO_FRAME_OUTPUT_NATIVE;
    out_layout->width = frame->width;
    out_layout->height = frame->height;
    out_layout->pix_fmt = frame->format;
    out_layout->plane_count = plane_count;
    out_layout->required_bytes = (int64_t)required;

    int64_t offset = 0;
    if( desc->flags & AV_PIX_FMT_FLAG_PAL )
    {
        out_layout->plane_width[0] = frame->width;
        out_layout->plane_height[0] = frame->height;
        out_layout->plane_stride[0] = linesizes[0];
        out_layout->plane_offset[0] = 0;

        if( plane_count > 1 )
        {
            offset = (int64_t)linesizes[0] * (int64_t)frame->height;
            out_layout->plane_width[1] = 256;
            out_layout->plane_height[1] = 1;
            out_layout->plane_stride[1] = 256 * 4;
            out_layout->plane_offset[1] = offset;
        }
        return 0;
    }

    for( int p = 0; p < plane_count; p++ )
    {
        int plane_width = frame->width;
        int plane_height = frame->height;
        if( p == 1 || p == 2 )
        {
            plane_width = AV_CEIL_RSHIFT( frame->width, desc->log2_chroma_w );
            plane_height = AV_CEIL_RSHIFT( frame->height, desc->log2_chroma_h );
        }

        out_layout->plane_width[p] = plane_width;
        out_layout->plane_height[p] = plane_height;
        out_layout->plane_stride[p] = linesizes[p];
        out_layout->plane_offset[p] = offset;

        offset += (int64_t)linesizes[p] * (int64_t)plane_height;
    }

    return 0;
}

static int fill_converted_frame_buffer_layout(
    int width,
    int height,
    lsmas_video_frame_output_format_t output_format,
    int32_t dst_stride,
    lsmas_video_frame_buffer_layout_t *out_layout,
    int64_t *out_required,
    char **error_message
)
{
    if( out_required )
        *out_required = -1;
    clear_frame_buffer_layout( out_layout );

    if( width <= 0 || height <= 0 )
    {
        set_error_message( error_message, "Decoded frame has invalid dimensions." );
        return -1;
    }

    int plane_count = 1;
    int pix_fmt = AV_PIX_FMT_NONE;
    int plane_width[4] = { 0, 0, 0, 0 };
    int plane_height[4] = { 0, 0, 0, 0 };
    int plane_stride[4] = { 0, 0, 0, 0 };
    int64_t plane_offset[4] = { 0, 0, 0, 0 };
    int64_t required = -1;

    switch( output_format )
    {
        case LSMAS_VIDEO_FRAME_OUTPUT_GRAY8:
        {
            int stride = dst_stride > 0 ? dst_stride : width;
            if( stride < width )
            {
                set_error_message( error_message, "dst_stride is too small for GRAY8." );
                return -1;
            }
            pix_fmt = AV_PIX_FMT_GRAY8;
            plane_width[0] = width;
            plane_height[0] = height;
            plane_stride[0] = stride;
            required = calc_gray8_required( width, height, stride );
            break;
        }
        case LSMAS_VIDEO_FRAME_OUTPUT_GRAY8_PADDED16:
        {
            const int rounded_width = (width + 15) & ~15;
            const int rounded_height = (height + 15) & ~15;
            int stride = dst_stride > 0 ? dst_stride : rounded_width;
            if( stride < rounded_width )
            {
                set_error_message( error_message, "dst_stride is too small for GRAY8 padded16." );
                return -1;
            }
            pix_fmt = AV_PIX_FMT_GRAY8;
            plane_width[0] = rounded_width;
            plane_height[0] = rounded_height;
            plane_stride[0] = stride;
            required = calc_gray8_padded16_required( width, height, stride );
            break;
        }
        case LSMAS_VIDEO_FRAME_OUTPUT_YUV420P8:
        {
            int y_stride = dst_stride > 0 ? dst_stride : width;
            if( width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0
                || y_stride < width || (y_stride & 1) != 0 )
            {
                set_error_message( error_message, "YUV420P8 requires positive even dimensions and an even dst_stride >= width." );
                return -1;
            }
            const int uv_stride = y_stride / 2;
            pix_fmt = AV_PIX_FMT_YUV420P;
            plane_count = 3;
            plane_width[0] = width;
            plane_height[0] = height;
            plane_stride[0] = y_stride;
            plane_offset[0] = 0;
            plane_width[1] = width / 2;
            plane_height[1] = height / 2;
            plane_stride[1] = uv_stride;
            plane_offset[1] = (int64_t)y_stride * (int64_t)height;
            plane_width[2] = width / 2;
            plane_height[2] = height / 2;
            plane_stride[2] = uv_stride;
            plane_offset[2] = plane_offset[1] + (int64_t)uv_stride * (int64_t)(height / 2);
            required = calc_yuv420p8_required( width, height, y_stride );
            break;
        }
        case LSMAS_VIDEO_FRAME_OUTPUT_BGRA:
        case LSMAS_VIDEO_FRAME_OUTPUT_RGBA:
        {
            int row_bytes = width * 4;
            int stride = dst_stride > 0 ? dst_stride : row_bytes;
            if( stride < row_bytes )
            {
                set_error_message( error_message, output_format == LSMAS_VIDEO_FRAME_OUTPUT_BGRA
                    ? "dst_stride is too small for BGRA."
                    : "dst_stride is too small for RGBA." );
                return -1;
            }
            pix_fmt = output_format == LSMAS_VIDEO_FRAME_OUTPUT_BGRA ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA;
            plane_width[0] = width;
            plane_height[0] = height;
            plane_stride[0] = stride;
            required = calc_packed_stride_required( width, height, stride, 4 );
            break;
        }
        default:
            set_error_message( error_message, "Unsupported frame output format." );
            return -1;
    }

    if( required < 0 )
    {
        set_error_message( error_message, "Failed to compute frame output layout." );
        return -1;
    }

    if( out_required )
        *out_required = required;

    if( !out_layout )
        return 0;

    out_layout->output_format = output_format;
    out_layout->width = width;
    out_layout->height = height;
    out_layout->pix_fmt = pix_fmt;
    out_layout->plane_count = plane_count;
    out_layout->required_bytes = required;
    for( int p = 0; p < plane_count; p++ )
    {
        out_layout->plane_width[p] = plane_width[p];
        out_layout->plane_height[p] = plane_height[p];
        out_layout->plane_stride[p] = plane_stride[p];
        out_layout->plane_offset[p] = plane_offset[p];
    }

    return 0;
}

LSMAS_NATIVE_API int64_t lsmas_video_get_frame(
    lsmas_handle_t *handle,
    int32_t frame_index,
    lsmas_video_frame_output_format_t output_format,
    uint8_t *dst,
    int32_t dst_stride,
    lsmas_video_frame_buffer_layout_t *out_layout,
    char **error_message
)
{
    clear_frame_buffer_layout( out_layout );

    if( output_format == LSMAS_VIDEO_FRAME_OUTPUT_NATIVE )
    {
        AVFrame *av_frame = NULL;
        if( decode_frame_for_output( handle, frame_index, &av_frame, error_message ) < 0 )
            return -1;

        int64_t required = -1;
        if( fill_native_frame_buffer_layout( av_frame, out_layout, &required, error_message ) < 0 )
            return -1;

        if( !dst )
            return required;

        if( av_image_copy_to_buffer( dst,
                                     (int)required,
                                     (const uint8_t * const *)av_frame->data,
                                     av_frame->linesize,
                                     (enum AVPixelFormat)av_frame->format,
                                     av_frame->width,
                                     av_frame->height,
                                     1 ) < 0 )
        {
            set_error_message( error_message, "Failed to copy native frame pixels." );
            return -1;
        }

        return required;
    }

    if( !handle )
    {
        set_error_message( error_message, "handle is NULL." );
        return -1;
    }
    if( frame_index < 0 )
    {
        set_error_message( error_message, "frame_index is negative." );
        return -1;
    }
    if( frame_index >= handle->info.num_frames )
    {
        set_error_message( error_message, "frame_index out of range." );
        return -1;
    }

    if( !dst )
    {
        int64_t required = -1;
        if( fill_converted_frame_buffer_layout( handle->info.width,
                                                handle->info.height,
                                                output_format,
                                                dst_stride,
                                                out_layout,
                                                &required,
                                                error_message ) < 0 )
            return -1;
        return required;
    }

    AVFrame *av_frame = NULL;
    if( decode_frame_for_output( handle, frame_index, &av_frame, error_message ) < 0 )
        return -1;

    lsmas_video_frame_buffer_layout_t layout;
    int64_t required = -1;
    if( fill_converted_frame_buffer_layout( av_frame->width,
                                            av_frame->height,
                                            output_format,
                                            dst_stride,
                                            &layout,
                                            &required,
                                            error_message ) < 0 )
        return -1;

    int effective_dst_stride = layout.plane_stride[0];

    int64_t ret = -1;
    switch( output_format )
    {
        case LSMAS_VIDEO_FRAME_OUTPUT_GRAY8:
            ret = copy_frame_gray8( handle, av_frame, dst, effective_dst_stride, error_message );
            break;
        case LSMAS_VIDEO_FRAME_OUTPUT_BGRA:
            ret = copy_frame_bgra( handle, av_frame, dst, effective_dst_stride, error_message );
            break;
        case LSMAS_VIDEO_FRAME_OUTPUT_RGBA:
            ret = copy_frame_rgba( handle, av_frame, dst, effective_dst_stride, error_message );
            break;
        case LSMAS_VIDEO_FRAME_OUTPUT_GRAY8_PADDED16:
            ret = copy_frame_gray8_padded16( handle, av_frame, dst, effective_dst_stride, error_message );
            break;
        case LSMAS_VIDEO_FRAME_OUTPUT_YUV420P8:
            ret = copy_frame_yuv420p8( handle, av_frame, dst, effective_dst_stride, error_message );
            break;
        default:
            set_error_message( error_message, "Unsupported frame output format." );
            return -1;
    }
    if( ret < 0 )
        return -1;

    if( out_layout )
        *out_layout = layout;

    return required;
}

LSMAS_NATIVE_API void lsmas_free( void *p )
{
    free( p );
}

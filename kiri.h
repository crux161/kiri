/*
 * kiri.h — public C API for libkiri
 *
 * Kiri is a media-aware, zero-copy video splitter built on FFmpeg. The library
 * is intentionally I/O-free at the API boundary: it never writes to stdout/
 * stderr, never installs signal handlers, never prompts. All reporting is via
 * callbacks supplied by the caller, and cancellation is cooperative (return
 * non-zero from the progress callback).
 *
 * ABI stability: structures here are part of the ABI. Do not reorder members
 * across a minor-version bump. Add new members at the end and bump the minor
 * version. Binary-incompatible changes bump the major version.
 */
#ifndef KIRI_H
#define KIRI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Symbol visibility ------------------------------------------------ */
#if defined(_WIN32) || defined(__CYGWIN__)
  #if defined(KIRI_BUILDING_SHARED)
    #define KIRI_API __declspec(dllexport)
  #elif defined(KIRI_SHARED)
    #define KIRI_API __declspec(dllimport)
  #else
    #define KIRI_API
  #endif
#else
  #if defined(KIRI_BUILDING_SHARED) || defined(KIRI_SHARED)
    #define KIRI_API __attribute__((visibility("default")))
  #else
    #define KIRI_API
  #endif
#endif

/* --- Version ---------------------------------------------------------- */
#define KIRI_VERSION_MAJOR 2
#define KIRI_VERSION_MINOR 0
#define KIRI_VERSION_PATCH 0

/* --- Status codes ----------------------------------------------------- */
typedef enum {
    KIRI_OK               =   0,
    KIRI_ERR_INVALID_ARG  =  -1,
    KIRI_ERR_OPEN_INPUT   =  -2,
    KIRI_ERR_STREAM_INFO  =  -3,
    KIRI_ERR_NO_STREAMS   =  -4,
    KIRI_ERR_OPEN_OUTPUT  =  -5,
    KIRI_ERR_WRITE        =  -6,
    KIRI_ERR_ALLOC        =  -7,
    KIRI_ERR_MKDIR        =  -8,
    KIRI_ERR_DISK_SPACE   =  -9,
    KIRI_ERR_INTERRUPTED  = -10,
    KIRI_ERR_IO           = -11,
    KIRI_ERR_FFMPEG       = -100   /* generic FFmpeg error; see log callback */
} kiri_status;

/* --- Log levels ------------------------------------------------------- */
typedef enum {
    KIRI_LOG_ERROR   = 0,
    KIRI_LOG_WARN    = 1,
    KIRI_LOG_INFO    = 2,
    KIRI_LOG_VERBOSE = 3
} kiri_log_level;

/* --- Callbacks -------------------------------------------------------- */

/*
 * Progress callback. Invoked each time a new segment starts, and once more
 * when the last segment completes. Returning a non-zero value causes the
 * split to abort cleanly (current segment is finalized, remaining packets
 * are dropped). `bytes_total` may be 0 if the input length is unknown.
 */
typedef int (*kiri_progress_fn)(void   *user,
                                int     current_segment,
                                int     total_segments,
                                int64_t bytes_processed,
                                int64_t bytes_total);

/*
 * Log callback. `msg` is a NUL-terminated string valid only for the duration
 * of the call — copy it if you need to keep it.
 */
typedef void (*kiri_log_fn)(void          *user,
                            kiri_log_level level,
                            const char    *msg);

/* --- Input probe ------------------------------------------------------ */
typedef struct {
    int64_t size_bytes;
    int64_t duration_us;              /* microseconds; <0 if unknown */
    int     nb_streams;
    int     has_video;
    int     video_width;
    int     video_height;
    char    format_long_name[128];
    char    video_codec_name[32];
} kiri_input_info;

/* --- Split options ---------------------------------------------------- */
typedef struct {
    /* Required ------------------------------------------------------- */
    const char *input_path;
    const char *output_dir;           /* must exist; caller creates it */
    int64_t     max_bytes;            /* per-segment hard size limit */

    /* Optional ------------------------------------------------------- */
    const char *basename;             /* segment name stem; derived from input if NULL */
    const char *extension;            /* segment extension including dot; derived from input if NULL */
    int64_t     safety_margin_bytes;  /* search window before max_bytes; default 60 MiB if 0 */
    int64_t     preallocate_bytes;    /* 0 = none; <0 = max_bytes; >0 = explicit */
    int         make_playlist;        /* 0/1 — emit <basename>.m3u8 */

    /* Callbacks (all optional) --------------------------------------- */
    kiri_progress_fn on_progress;
    kiri_log_fn      on_log;
    void            *user_data;
} kiri_split_options;

/* --- API -------------------------------------------------------------- */

/* Library version, e.g. "2.0.0". Safe to use before any other call. */
KIRI_API const char *kiri_version_string(void);

/* Human-readable error string for a kiri_status value. */
KIRI_API const char *kiri_strerror(kiri_status status);

/* Probe a file and fill *info. Does not split or modify anything. */
KIRI_API kiri_status kiri_probe(const char      *input_path,
                                kiri_input_info *info);

/* Split a file. Blocks until done or aborted. Safe to call from any thread,
 * but not concurrently on the same options struct. */
KIRI_API kiri_status kiri_split(const kiri_split_options *options);

/* Utility: ceil(file_size / chunk_size), clamped to >= 1. */
KIRI_API int64_t kiri_chunk_count(int64_t file_size_bytes, int64_t chunk_size_bytes);

/* Convenience constant: (4 GiB - 1). */
KIRI_API int64_t kiri_fat32_max_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* KIRI_H */

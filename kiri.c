/*
 * kiri.c — libkiri implementation
 *
 * Pure library code. No stdio, no prompts, no signal handlers. All reporting
 * is funneled through the caller-supplied log and progress callbacks.
 * Cooperative cancellation via the progress callback's return value.
 */
#include "kiri.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>
#include <libavutil/opt.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#if defined(_WIN32)
  #include <windows.h>
  #ifndef PATH_MAX
    #define PATH_MAX MAX_PATH
  #endif
#else
  #include <sys/statvfs.h>
#endif

/* --- Tunables --------------------------------------------------------- */
#define KIRI_DEFAULT_SAFETY_MARGIN (60LL * 1024 * 1024)
static const int64_t KIRI_FAT32_MAX = (4LL * 1024 * 1024 * 1024) - 1;

/* --- Internal state --------------------------------------------------- */
typedef struct {
    int64_t pts_offset;
    int64_t dts_offset;
    int64_t last_dts;
    int     initialized;
} kiri_stream_state;

typedef struct {
    const kiri_split_options *opts;
    kiri_progress_fn progress;
    kiri_log_fn      log;
    void            *user;
} kiri_ctx;

/* --- Logging helpers -------------------------------------------------- */
static void kiri_logf(const kiri_ctx *ctx, kiri_log_level lvl, const char *fmt, ...) {
    if (!ctx->log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ctx->log(ctx->user, lvl, buf);
}

static void kiri_log_ffmpeg(const kiri_ctx *ctx, kiri_log_level lvl,
                             const char *prefix, int averr) {
    char ebuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(averr, ebuf, sizeof(ebuf));
    kiri_logf(ctx, lvl, "%s: %s", prefix, ebuf);
}

/* --- Portable filesystem helpers ------------------------------------- */

/*
 * Pre-allocation. On macOS reserves contiguous blocks without changing the
 * logical file size. On Linux uses fallocate+KEEP_SIZE. Best-effort: failure
 * is non-fatal.
 *
 * NOTE: must be called AFTER the file has been created (by avio_open) —
 * otherwise the subsequent open-with-truncate discards the reservation.
 */
static void kiri_preallocate(const char *path, int64_t size) {
    if (size <= 0) return;
    int fd = open(path, O_RDWR);
    if (fd == -1) return;

#ifdef __APPLE__
    fstore_t store = { F_ALLOCATECONTIG, F_PEOFPOSMODE, 0, size, 0 };
    if (fcntl(fd, F_PREALLOCATE, &store) == -1) {
        store.fst_flags = F_ALLOCATEALL;
        (void)fcntl(fd, F_PREALLOCATE, &store);
    }
#elif defined(__linux__)
    #ifdef FALLOC_FL_KEEP_SIZE
      (void)fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, size);
    #else
      (void)posix_fallocate(fd, 0, size);
    #endif
#elif !defined(_WIN32)
    (void)posix_fallocate(fd, 0, size);
#endif

    close(fd);
}

static int kiri_check_disk_space(const char *path, int64_t required_bytes) {
    /* 100MB cushion on top of the raw requirement */
    int64_t cushion = 100LL * 1024 * 1024;

#if defined(_WIN32)
    ULARGE_INTEGER avail;
    if (!GetDiskFreeSpaceExA(path, &avail, NULL, NULL)) return -1;
    if ((int64_t)avail.QuadPart < required_bytes + cushion) return -1;
    return 0;
#else
    struct statvfs st;
    if (statvfs(path, &st) != 0) return -1;
    int64_t available = (int64_t)st.f_bavail * (int64_t)st.f_frsize;
    if (available < required_bytes + cushion) return -1;
    return 0;
#endif
}

/* --- Path parsing ----------------------------------------------------- */
static void kiri_split_path(const char *input_file,
                             char *basename, size_t base_size,
                             char *extension, size_t ext_size) {
    const char *slash_fwd = strrchr(input_file, '/');
#ifdef _WIN32
    const char *slash_back = strrchr(input_file, '\\');
    const char *slash = slash_back > slash_fwd ? slash_back : slash_fwd;
#else
    const char *slash = slash_fwd;
#endif
    const char *filename = slash ? slash + 1 : input_file;
    const char *dot = strrchr(filename, '.');
    size_t base_len = dot ? (size_t)(dot - filename) : strlen(filename);
    if (base_len >= base_size) base_len = base_size - 1;
    memcpy(basename, filename, base_len);
    basename[base_len] = '\0';

    if (dot) {
        size_t ext_len = strlen(dot);
        if (ext_len >= ext_size) ext_len = ext_size - 1;
        memcpy(extension, dot, ext_len);
        extension[ext_len] = '\0';
    } else {
        extension[0] = '\0';
    }
}

static int kiri_digits_for_int(int value) {
    int digits = 1;
    while (value >= 10) { value /= 10; digits++; }
    return digits;
}

/* --- Output segment lifecycle ---------------------------------------- */

/*
 * Open a new output segment. Builds a stream mapping that drops streams the
 * output container can't mux (data, timecode, attachments). `stream_map[i]`
 * is set to the output stream index or -1 to skip. Preallocation runs AFTER
 * avio_open so the reservation isn't truncated away.
 */
static int kiri_open_segment(const kiri_ctx      *ctx,
                              AVFormatContext    *input_ctx,
                              AVFormatContext   **output_ctx,
                              const char         *output_path,
                              int64_t             prealloc_size,
                              int                *stream_map) {
    AVFormatContext *out_ctx = NULL;
    int ret = avformat_alloc_output_context2(&out_ctx, NULL, NULL, output_path);
    if (ret < 0 || !out_ctx) {
        kiri_log_ffmpeg(ctx, KIRI_LOG_ERROR, "avformat_alloc_output_context2", ret);
        return KIRI_ERR_FFMPEG;
    }

    int out_idx = 0;
    for (unsigned int i = 0; i < input_ctx->nb_streams; i++) {
        AVStream *in_stream = input_ctx->streams[i];
        enum AVMediaType t = in_stream->codecpar->codec_type;
        if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO && t != AVMEDIA_TYPE_SUBTITLE) {
            stream_map[i] = -1;
            continue;
        }
        AVStream *out_stream = avformat_new_stream(out_ctx, NULL);
        if (!out_stream) { avformat_free_context(out_ctx); return KIRI_ERR_ALLOC; }
        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) {
            kiri_log_ffmpeg(ctx, KIRI_LOG_ERROR, "avcodec_parameters_copy", ret);
            avformat_free_context(out_ctx);
            return KIRI_ERR_FFMPEG;
        }
        out_stream->time_base = in_stream->time_base;
        out_stream->codecpar->codec_tag = 0;
        stream_map[i] = out_idx++;
    }

    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out_ctx->pb, output_path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            kiri_log_ffmpeg(ctx, KIRI_LOG_ERROR, "avio_open", ret);
            avformat_free_context(out_ctx);
            return KIRI_ERR_OPEN_OUTPUT;
        }
    }

    if (prealloc_size > 0) {
        kiri_preallocate(output_path, prealloc_size);
    }

    ret = avformat_write_header(out_ctx, NULL);
    if (ret < 0) {
        kiri_log_ffmpeg(ctx, KIRI_LOG_ERROR, "avformat_write_header", ret);
        avio_closep(&out_ctx->pb);
        avformat_free_context(out_ctx);
        return KIRI_ERR_WRITE;
    }

    *output_ctx = out_ctx;
    return KIRI_OK;
}

static void kiri_close_segment(AVFormatContext **output_ctx) {
    if (!output_ctx || !*output_ctx) return;
    av_write_trailer(*output_ctx);
    if (!((*output_ctx)->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&(*output_ctx)->pb);
    }
    avformat_free_context(*output_ctx);
    *output_ctx = NULL;
}

/* --- Public API ------------------------------------------------------- */

KIRI_API const char *kiri_version_string(void) {
    static const char v[] = "2.0.0";
    return v;
}

KIRI_API int64_t kiri_fat32_max_bytes(void) {
    return KIRI_FAT32_MAX;
}

KIRI_API int64_t kiri_chunk_count(int64_t file_size_bytes, int64_t chunk_size_bytes) {
    if (file_size_bytes <= 0 || chunk_size_bytes <= 0) return 1;
    return (file_size_bytes + chunk_size_bytes - 1) / chunk_size_bytes;
}

KIRI_API const char *kiri_strerror(kiri_status s) {
    switch (s) {
        case KIRI_OK:              return "OK";
        case KIRI_ERR_INVALID_ARG: return "invalid argument";
        case KIRI_ERR_OPEN_INPUT:  return "failed to open input";
        case KIRI_ERR_STREAM_INFO: return "failed to read stream info";
        case KIRI_ERR_NO_STREAMS:  return "no supported streams found";
        case KIRI_ERR_OPEN_OUTPUT: return "failed to open output";
        case KIRI_ERR_WRITE:       return "write failed";
        case KIRI_ERR_ALLOC:       return "allocation failed";
        case KIRI_ERR_MKDIR:       return "mkdir failed";
        case KIRI_ERR_DISK_SPACE:  return "insufficient disk space";
        case KIRI_ERR_INTERRUPTED: return "interrupted by caller";
        case KIRI_ERR_IO:          return "I/O error";
        case KIRI_ERR_FFMPEG:      return "FFmpeg error (see log)";
    }
    return "unknown";
}

KIRI_API kiri_status kiri_probe(const char *input_path, kiri_input_info *info) {
    if (!input_path || !info) return KIRI_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    info->duration_us = -1;

    AVFormatContext *ctx = NULL;
    int r = avformat_open_input(&ctx, input_path, NULL, NULL);
    if (r != 0) return KIRI_ERR_OPEN_INPUT;
    if (avformat_find_stream_info(ctx, NULL) < 0) {
        avformat_close_input(&ctx);
        return KIRI_ERR_STREAM_INFO;
    }

    info->size_bytes  = avio_size(ctx->pb);
    info->duration_us = ctx->duration == AV_NOPTS_VALUE ? -1 : ctx->duration;
    info->nb_streams  = (int)ctx->nb_streams;
    snprintf(info->format_long_name, sizeof(info->format_long_name),
             "%s", ctx->iformat->long_name ? ctx->iformat->long_name : "");

    for (unsigned int i = 0; i < ctx->nb_streams; i++) {
        AVCodecParameters *cp = ctx->streams[i]->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
            info->has_video    = 1;
            info->video_width  = cp->width;
            info->video_height = cp->height;
            const char *name = avcodec_get_name(cp->codec_id);
            snprintf(info->video_codec_name, sizeof(info->video_codec_name),
                     "%s", name ? name : "");
            break;
        }
    }

    avformat_close_input(&ctx);
    return KIRI_OK;
}

KIRI_API kiri_status kiri_split(const kiri_split_options *opts) {
    if (!opts || !opts->input_path || !opts->output_dir || opts->max_bytes <= 0)
        return KIRI_ERR_INVALID_ARG;

    kiri_ctx ctx = {
        .opts = opts,
        .progress = opts->on_progress,
        .log = opts->on_log,
        .user = opts->user_data
    };

    /* Resolve derived options */
    char derived_base[PATH_MAX] = {0};
    char derived_ext[PATH_MAX]  = {0};
    kiri_split_path(opts->input_path, derived_base, sizeof(derived_base),
                     derived_ext, sizeof(derived_ext));
    const char *basename  = opts->basename  ? opts->basename  : derived_base;
    const char *extension = opts->extension ? opts->extension : derived_ext;

    int64_t safety_margin = opts->safety_margin_bytes > 0
                          ? opts->safety_margin_bytes
                          : KIRI_DEFAULT_SAFETY_MARGIN;
    if (safety_margin >= opts->max_bytes) {
        kiri_logf(&ctx, KIRI_LOG_ERROR,
                  "safety_margin (%lld) must be < max_bytes (%lld)",
                  (long long)safety_margin, (long long)opts->max_bytes);
        return KIRI_ERR_INVALID_ARG;
    }

    int64_t prealloc = opts->preallocate_bytes;
    if (prealloc < 0) prealloc = opts->max_bytes;

    /* Open input */
    AVFormatContext *input_ctx = NULL;
    int r = avformat_open_input(&input_ctx, opts->input_path, NULL, NULL);
    if (r != 0) {
        kiri_log_ffmpeg(&ctx, KIRI_LOG_ERROR, "avformat_open_input", r);
        return KIRI_ERR_OPEN_INPUT;
    }
    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        avformat_close_input(&input_ctx);
        return KIRI_ERR_STREAM_INFO;
    }

    int64_t input_size = avio_size(input_ctx->pb);
    int total_segments = (int)kiri_chunk_count(input_size, opts->max_bytes);
    int width = kiri_digits_for_int(total_segments);

    /* Disk space check — best-effort */
    if (kiri_check_disk_space(opts->output_dir, input_size) != 0) {
        kiri_logf(&ctx, KIRI_LOG_WARN, "low or unknown disk space on '%s'", opts->output_dir);
    }

    /* State allocations */
    kiri_stream_state *states = calloc(input_ctx->nb_streams, sizeof(*states));
    int *stream_map = calloc(input_ctx->nb_streams, sizeof(int));
    if (!states || !stream_map) {
        free(states); free(stream_map);
        avformat_close_input(&input_ctx);
        return KIRI_ERR_ALLOC;
    }
    for (unsigned int i = 0; i < input_ctx->nb_streams; i++)
        states[i].last_dts = AV_NOPTS_VALUE;

    int video_stream_index = -1;
    for (unsigned int i = 0; i < input_ctx->nb_streams; i++) {
        if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = (int)i;
            break;
        }
    }

    /* First output segment */
    char output_path[PATH_MAX];
    int current_segment = 1;
    AVFormatContext *output_ctx = NULL;
    snprintf(output_path, sizeof(output_path), "%s/%s_%0*d%s",
             opts->output_dir, basename, width, current_segment, extension);
    kiri_status s = kiri_open_segment(&ctx, input_ctx, &output_ctx,
                                      output_path, prealloc, stream_map);
    if (s != KIRI_OK) {
        free(states); free(stream_map);
        avformat_close_input(&input_ctx);
        return s;
    }

    /* Playlist */
    FILE *playlist = NULL;
    if (opts->make_playlist) {
        char pl_path[PATH_MAX];
        snprintf(pl_path, sizeof(pl_path), "%s/%s.m3u8", opts->output_dir, basename);
        playlist = fopen(pl_path, "w");
        if (playlist) fprintf(playlist, "#EXTM3U\n");
        else kiri_logf(&ctx, KIRI_LOG_WARN, "could not open playlist '%s'", pl_path);
    }

    /* Split loop state */
    int64_t split_threshold = opts->max_bytes - safety_margin;
    int64_t segment_start_video_dts = 0;
    int64_t last_video_dts          = 0;
    int64_t segment_ref_us          = 0;
    int     segment_ref_set         = 0;
    int     waiting_for_keyframe    = 0;
    int     aborted                 = 0;

    AVPacket packet;
    kiri_status final_status = KIRI_OK;

    while (av_read_frame(input_ctx, &packet) >= 0) {
        if (stream_map[packet.stream_index] < 0) {
            av_packet_unref(&packet);
            continue;
        }

        AVStream *in_stream  = input_ctx->streams[packet.stream_index];
        AVStream *out_stream = output_ctx->streams[stream_map[packet.stream_index]];
        kiri_stream_state *st = &states[packet.stream_index];

        /* Sanitize broken timestamps */
        if (packet.pts == AV_NOPTS_VALUE) packet.pts = packet.dts;
        if (packet.dts == AV_NOPTS_VALUE) packet.dts = packet.pts;
        if (packet.dts == AV_NOPTS_VALUE) { packet.dts = 0; packet.pts = 0; }

        /* Enforce DTS monotonicity on corrupted streams */
        if (st->last_dts != AV_NOPTS_VALUE && packet.dts < st->last_dts) {
            packet.dts = st->last_dts + 1;
            if (packet.pts < packet.dts) packet.pts = packet.dts;
        }
        st->last_dts = packet.dts;

        if (!segment_ref_set) {
            segment_ref_us = av_rescale_q(packet.dts, in_stream->time_base, AV_TIME_BASE_Q);
            segment_ref_set = 1;
        }

        int is_video = (packet.stream_index == video_stream_index);
        if (is_video) last_video_dts = packet.dts;

        int64_t current_size = avio_tell(output_ctx->pb);
        int is_keyframe = is_video && (packet.flags & AV_PKT_FLAG_KEY);

        int do_split = 0;
        if (current_size > split_threshold && is_keyframe) {
            do_split = 1;
        } else if (current_size >= opts->max_bytes) {
            do_split = 1;
            if (!is_keyframe) waiting_for_keyframe = 1;
        }

        if (do_split) {
            double seg_duration = 0.0;
            if (video_stream_index >= 0) {
                AVRational tb = input_ctx->streams[video_stream_index]->time_base;
                seg_duration = (double)(last_video_dts - segment_start_video_dts) * av_q2d(tb);
            }

            kiri_close_segment(&output_ctx);

            if (playlist) {
                fprintf(playlist, "#EXTINF:%.3f,\n%s_%0*d%s\n",
                        seg_duration > 0.0 ? seg_duration : 0.0,
                        basename, width, current_segment, extension);
            }

            current_segment++;
            segment_start_video_dts = last_video_dts;
            segment_ref_us = av_rescale_q(packet.dts, in_stream->time_base, AV_TIME_BASE_Q);

            if (ctx.progress) {
                int64_t pos = avio_tell(input_ctx->pb);
                int abort_req = ctx.progress(ctx.user,
                                             current_segment - 1,
                                             total_segments,
                                             pos, input_size);
                if (abort_req) { aborted = 1; av_packet_unref(&packet); break; }
            }

            snprintf(output_path, sizeof(output_path), "%s/%s_%0*d%s",
                     opts->output_dir, basename, width, current_segment, extension);
            kiri_status sopen = kiri_open_segment(&ctx, input_ctx, &output_ctx,
                                                  output_path, prealloc, stream_map);
            if (sopen != KIRI_OK) {
                final_status = sopen;
                av_packet_unref(&packet);
                break;
            }

            for (unsigned int i = 0; i < input_ctx->nb_streams; i++)
                states[i].initialized = 0;
        }

        /* After a forced (non-keyframe) split, drop video until the next keyframe
         * so each segment starts cleanly and is independently playable. */
        if (waiting_for_keyframe) {
            if (is_video) {
                if (is_keyframe) waiting_for_keyframe = 0;
                else { av_packet_unref(&packet); continue; }
            }
        }

        /* Zero-base against the unified segment reference */
        if (!st->initialized) {
            int64_t ref_in_tb = av_rescale_q(segment_ref_us, AV_TIME_BASE_Q,
                                             in_stream->time_base);
            st->pts_offset = ref_in_tb;
            st->dts_offset = ref_in_tb;
            st->initialized = 1;
        }

        av_packet_rescale_ts(&packet, in_stream->time_base, out_stream->time_base);
        int64_t pts_off = av_rescale_q(st->pts_offset, in_stream->time_base, out_stream->time_base);
        int64_t dts_off = av_rescale_q(st->dts_offset, in_stream->time_base, out_stream->time_base);

        packet.pts -= pts_off;
        packet.dts -= dts_off;
        if (packet.dts < 0) packet.dts = 0;
        if (packet.pts < 0) packet.pts = 0;
        if (packet.pts < packet.dts) packet.pts = packet.dts;

        packet.stream_index = stream_map[packet.stream_index];
        packet.pos = -1;

        int wret = av_interleaved_write_frame(output_ctx, &packet);
        av_packet_unref(&packet);
        if (wret < 0) {
            kiri_log_ffmpeg(&ctx, KIRI_LOG_WARN, "av_interleaved_write_frame", wret);
        }
    }

    /* Finalize last segment */
    kiri_close_segment(&output_ctx);

    if (playlist) {
        double seg_duration = 0.0;
        if (video_stream_index >= 0) {
            AVRational tb = input_ctx->streams[video_stream_index]->time_base;
            seg_duration = (double)(last_video_dts - segment_start_video_dts) * av_q2d(tb);
        }
        fprintf(playlist, "#EXTINF:%.3f,\n%s_%0*d%s\n",
                seg_duration > 0.0 ? seg_duration : 0.0,
                basename, width, current_segment, extension);
        fprintf(playlist, "#EXT-X-ENDLIST\n");
        fclose(playlist);
    }

    if (ctx.progress) {
        ctx.progress(ctx.user, current_segment, total_segments,
                     input_size, input_size);
    }

    free(stream_map);
    free(states);
    avformat_close_input(&input_ctx);

    if (aborted) return KIRI_ERR_INTERRUPTED;
    return final_status;
}

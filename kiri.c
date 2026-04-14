#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>
#include <libavutil/opt.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

// --- Configuration ---
#define SAFETY_MARGIN_BYTES (60LL * 1024 * 1024) // 60 MiB buffer before hard limit
#define MIN_CHUNK_MIB       100                   // Minimum chunk size in MiB
static const int64_t FAT32_MAX_BYTES = (4LL * 1024 * 1024 * 1024) - 1;

static const char *PROGRAM_NAME = "Kiri";
static const float PROGRAM_VERSION = 1.3;
static const char *AUTHOR = "crux161";

// --- Signal handling for clean shutdown ---
static volatile sig_atomic_t g_interrupted = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
}

// --- State Tracking ---
typedef struct {
    int64_t pts_offset;
    int64_t dts_offset;
    int64_t last_dts;      // To enforce monotonicity
    int initialized;
} StreamState;

// --- Prototypes ---
static void    humanReadable(int64_t bytes, char *buf, size_t buf_size);
static int     segmentCheck(int64_t bytes, int64_t max_bytes);
static void    banner(void);
static int     split_video(const char *input_file, const char *output_dir, const char *base_name, const char *ext,
                           int64_t max_bytes, int total_segments, int make_playlist);
static int     make_output_dir(const char *output_dir);
static void    split_path(const char *input_file, char *base_name, size_t base_size, char *ext, size_t ext_size);
static int     open_output_segment(AVFormatContext *input_ctx, AVFormatContext **output_ctx,
                                   const char *output_path, int64_t prealloc_size,
                                   int *stream_map, int *out_nb_streams);
static void    close_output_segment(AVFormatContext **output_ctx);
static int     digits_for_int(int value);
static int64_t ask_chunk_size(int64_t file_size, int64_t suggested_max);
static int64_t chunks_for_size(int64_t file_size, int64_t chunk_size);
static int     check_disk_space(const char *path, int64_t required_bytes);
static void    preallocate_file(const char *path, int64_t size);

// --- Main ---
int main(int argc, char *argv[]) {
    // Silence non-critical errors to keep UI clean, but allow criticals
    av_log_set_level(AV_LOG_ERROR);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input> [--playlist|-p]\n", argv[0]);
        return 1;
    }

    const char *input_file = argv[1];
    int make_playlist = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--playlist") == 0 || strcmp(argv[i], "-p") == 0) {
            make_playlist = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    AVFormatContext *formatCtx = avformat_alloc_context();
    if (avformat_open_input(&formatCtx, input_file, NULL, NULL) != 0) {
        fprintf(stderr, "Error: Could not open input file '%s'.\n", input_file);
        return 2;
    }

    if (avformat_find_stream_info(formatCtx, NULL) < 0) {
        fprintf(stderr, "Error: Could not find stream information.\n");
        avformat_close_input(&formatCtx);
        return 3;
    }

    banner();

    char hr_buf[64];
    printf(" File:\t\t'%s'\n", input_file);
    printf(" Format:\t'%s'\n", formatCtx->iformat->long_name);
    humanReadable(avio_size(formatCtx->pb), hr_buf, sizeof(hr_buf));
    printf(" Size:\t\t %s\n", hr_buf);

    // Correct Time Display (Handle NOPTS)
    if (formatCtx->duration != AV_NOPTS_VALUE) {
        int64_t duration = formatCtx->duration + (formatCtx->duration <= INT64_MAX - 5000 ? 5000 : 0);
        int64_t secs = duration / AV_TIME_BASE;
        printf(" Time:\t\t %02"PRId64":%02"PRId64":%02"PRId64"\n",
            secs / 3600, (secs % 3600) / 60, secs % 60);
    } else {
        printf(" Time:\t\t [Unknown - Stream might be damaged]\n");
    }

    printf(" Streams:\t %d\n", formatCtx->nb_streams);

    // Identify Video Stream
    int has_video = 0;
    for (unsigned int i = 0; i < formatCtx->nb_streams; i++) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            has_video = 1;
            printf(" Codec:\t\t'%s'\n", avcodec_get_name(formatCtx->streams[i]->codecpar->codec_id));
            printf(" Resolution:\t %d x %d\n\n", formatCtx->streams[i]->codecpar->width, formatCtx->streams[i]->codecpar->height);
            break;
        }
    }

    if (!has_video) {
        printf(" Warning: No video stream found. Keyframe-aware splitting unavailable.\n\n");
    }

    int64_t file_size = avio_size(formatCtx->pb);
    int chunks = segmentCheck(file_size, FAT32_MAX_BYTES);

    char base_name[PATH_MAX] = {0};
    char ext[PATH_MAX] = {0};
    split_path(input_file, base_name, sizeof(base_name), ext, sizeof(ext));

    int64_t chosen_chunk_size = 0;
    if (chunks >= 1) {
        chosen_chunk_size = ask_chunk_size(file_size, FAT32_MAX_BYTES);
        int64_t chosen_chunks = chunks_for_size(file_size, chosen_chunk_size);

        humanReadable(chosen_chunk_size, hr_buf, sizeof(hr_buf));
        printf(" Selected chunk size: %s\n", hr_buf);
        printf(" This will produce approximately %" PRId64 " chunk(s).\n", chosen_chunks);

        printf(" A directory named '%s' will be created.\n Proceed? (Y/N) ", base_name);
        fflush(stdout);

        char input_buf[64];
        int confirmed = 0;
        while (1) {
            if (!fgets(input_buf, sizeof(input_buf), stdin)) break;
            char choice = input_buf[0];
            if (choice == 'y' || choice == 'Y') { confirmed = 1; break; }
            if (choice == 'n' || choice == 'N') { printf(" Aborting...\n"); break; }
            printf(" Invalid input. (Y/N): ");
            fflush(stdout);
        }

        if (confirmed) {
            // 1. Check Space
            if (check_disk_space(".", file_size) != 0) {
                fprintf(stderr, " Error: Insufficient disk space.\n");
                avformat_close_input(&formatCtx);
                return 5;
            }

            // 2. Make Dir
            if (make_output_dir(base_name) != 0) {
                fprintf(stderr, " Error creating output directory '%s'.\n", base_name);
                avformat_close_input(&formatCtx);
                return 4;
            }

            // 3. Execute
            printf(" Proceeding with split...\n");
            int ret = split_video(input_file, base_name, base_name, ext,
                                  chosen_chunk_size, (int)chosen_chunks, make_playlist);

            if (ret != 0) {
                char err_buf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, err_buf, sizeof(err_buf));
                fprintf(stderr, "\n !!! SPLIT FAILED !!!\n Error Code: %d (%s)\n", ret, err_buf);
                avformat_close_input(&formatCtx);
                return ret;
            }

            if (g_interrupted) {
                printf("\n Interrupted. Partial output in '%s/'.\n", base_name);
            } else {
                printf("\n Success! Output is in directory '%s/'\n", base_name);
            }
        }
    }

    avformat_close_input(&formatCtx);
    return 0;
}

// --- Helper Functions ---

static void humanReadable(int64_t bytes, char *buf, size_t buf_size) {
    const char *sfx[] = {"B", "KB", "MB", "GB"};
    int len = sizeof(sfx) / sizeof(sfx[0]);
    int i = 0;
    double d_bytes = (double)bytes;
    if (bytes > 1024) {
        for (i = 0; (bytes / 1024) > 0 && i < (len - 1); i++, bytes /= 1024) {
            d_bytes = bytes / 1024.0;
        }
    }
    snprintf(buf, buf_size, "%.2f %s", d_bytes, sfx[i]);
}

// macOS/POSIX Disk Space Check
static int check_disk_space(const char *path, int64_t required_bytes) {
    struct statvfs st;
    if (statvfs(path, &st) != 0) {
        perror("statvfs");
        return -1;
    }
    int64_t available = (int64_t)st.f_bavail * st.f_frsize;
    if (available < (required_bytes + 100 * 1024 * 1024)) { // 100MB Buffer
        char need_buf[64], have_buf[64];
        humanReadable(required_bytes, need_buf, sizeof(need_buf));
        humanReadable(available, have_buf, sizeof(have_buf));
        printf(" Warning: Low disk space! Need: %s, Have: %s\n", need_buf, have_buf);
        return -1;
    }
    return 0;
}

// Pre-allocation helper (macOS / POSIX)
// Must be called AFTER the file is created by avio_open, so the allocation
// isn't immediately truncated away.
static void preallocate_file(const char *path, int64_t size) {
    int fd = open(path, O_RDWR);
    if (fd == -1) return;

#ifdef __APPLE__
    // macOS: F_PREALLOCATE reserves disk blocks without changing logical size
    fstore_t store = {F_ALLOCATECONTIG, F_PEOFPOSMODE, 0, size, 0};
    if (fcntl(fd, F_PREALLOCATE, &store) == -1) {
        store.fst_flags = F_ALLOCATEALL;
        fcntl(fd, F_PREALLOCATE, &store);
    }
    // Do NOT ftruncate — that would confuse FFmpeg's position tracking
#else
    // Generic POSIX fallback
    posix_fallocate(fd, 0, size);
#endif

    close(fd);
}

static int segmentCheck(int64_t bytes, int64_t max_bytes) {
    if (bytes <= 0) return 1;
    if (bytes > max_bytes) {
        return (int)((bytes + max_bytes - 1) / max_bytes);
    }
    return 1;
}

static void banner(void) {
    printf("\n=======================================\n");
    printf(" %s v%.2f\t FAT32 FFmpeg Splitter\n", PROGRAM_NAME, PROGRAM_VERSION);
    printf("---------------------------------------\n");
    printf(" %s\n", AUTHOR);
    printf("=======================================\n");
}

static int make_output_dir(const char *output_dir) {
    if (mkdir(output_dir, 0755) != 0) {
        if (errno == EEXIST) return 0;
        return -1;
    }
    return 0;
}

static void split_path(const char *input_file, char *base_name, size_t base_size, char *ext, size_t ext_size) {
    const char *slash = strrchr(input_file, '/');
    const char *filename = slash ? (slash + 1) : input_file;
    const char *dot = strrchr(filename, '.');
    size_t base_len = dot ? (size_t)(dot - filename) : strlen(filename);

    if (base_len >= base_size) base_len = base_size - 1;
    memcpy(base_name, filename, base_len);
    base_name[base_len] = '\0';

    if (dot != NULL) {
        size_t ext_len = strlen(dot);
        if (ext_len >= ext_size) ext_len = ext_size - 1;
        memcpy(ext, dot, ext_len);
        ext[ext_len] = '\0';
    } else ext[0] = '\0';
}

static int digits_for_int(int value) {
    int digits = 1;
    while (value >= 10) { value /= 10; digits++; }
    return digits;
}

static int64_t chunks_for_size(int64_t file_size, int64_t chunk_size) {
    if (file_size <= 0 || chunk_size <= 0) return 1;
    return (file_size + chunk_size - 1) / chunk_size;
}

static int64_t ask_chunk_size(int64_t file_size, int64_t suggested_max) {
    char buffer[64];
    int64_t max_mib = suggested_max / (1024LL * 1024);
    char hr_buf[64];

    humanReadable(suggested_max, hr_buf, sizeof(hr_buf));
    printf(" Suggested chunk size: %s\n", hr_buf);
    printf(" Estimated chunks: %" PRId64 "\n", chunks_for_size(file_size, suggested_max));

    while (1) {
        printf("\n Choose chunk size:\n");
        printf("  [1] Suggested max (%" PRId64 " MiB)\n", max_mib);
        printf("  [2] Custom size in MiB (%d - %" PRId64 ")\n", MIN_CHUNK_MIB, max_mib);
        printf(" Select option (1/2): ");
        fflush(stdout);

        if (!fgets(buffer, sizeof(buffer), stdin)) return suggested_max;
        int choice = (int)strtol(buffer, NULL, 10);

        if (choice == 1) return suggested_max;
        if (choice == 2) {
            printf(" Enter chunk size in MiB: ");
            fflush(stdout);
            if (!fgets(buffer, sizeof(buffer), stdin)) return suggested_max;
            int64_t custom_mib = strtoll(buffer, NULL, 10);
            if (custom_mib >= MIN_CHUNK_MIB && custom_mib <= max_mib) {
                return custom_mib * 1024LL * 1024;
            }
            printf(" Invalid size. Must be between %d and %" PRId64 " MiB.\n", MIN_CHUNK_MIB, max_mib);
            continue;
        }
    }
}

static int open_output_segment(AVFormatContext *input_ctx, AVFormatContext **output_ctx,
                               const char *output_path, int64_t prealloc_size,
                               int *stream_map, int *out_nb_streams) {
    AVFormatContext *out_ctx = NULL;
    int ret = avformat_alloc_output_context2(&out_ctx, NULL, NULL, output_path);
    if (ret < 0 || !out_ctx) return -1;

    int out_idx = 0;
    for (unsigned int i = 0; i < input_ctx->nb_streams; i++) {
        AVStream *in_stream = input_ctx->streams[i];
        enum AVMediaType type = in_stream->codecpar->codec_type;

        // Only copy streams the output container can handle
        if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_SUBTITLE) {
            stream_map[i] = -1;
            continue;
        }

        AVStream *out_stream = avformat_new_stream(out_ctx, NULL);
        if (!out_stream) { avformat_free_context(out_ctx); return -1; }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) { avformat_free_context(out_ctx); return ret; }

        out_stream->time_base = in_stream->time_base;
        out_stream->codecpar->codec_tag = 0;
        stream_map[i] = out_idx++;
    }
    if (out_nb_streams) *out_nb_streams = out_idx;

    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out_ctx->pb, output_path, AVIO_FLAG_WRITE);
        if (ret < 0) { avformat_free_context(out_ctx); return ret; }
    }

    // Pre-allocate AFTER avio_open so the allocation isn't truncated
    if (prealloc_size > 0) {
        preallocate_file(output_path, prealloc_size);
    }

    ret = avformat_write_header(out_ctx, NULL);
    if (ret < 0) {
        avio_closep(&out_ctx->pb);
        avformat_free_context(out_ctx);
        return ret;
    }

    *output_ctx = out_ctx;
    return 0;
}

static void close_output_segment(AVFormatContext **output_ctx) {
    if (output_ctx == NULL || *output_ctx == NULL) return;
    av_write_trailer(*output_ctx);
    if (!((*output_ctx)->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&(*output_ctx)->pb);
    }
    avformat_free_context(*output_ctx);
    *output_ctx = NULL;
}

// --- The Core Splitter ---
static int split_video(const char *input_file, const char *output_dir, const char *base_name, const char *ext,
                       int64_t max_bytes, int total_segments, int make_playlist) {
    AVFormatContext *input_ctx = NULL;
    AVFormatContext *output_ctx = NULL;
    AVPacket packet;
    int ret = 0;
    int current_segment = 1;
    int width = digits_for_int(total_segments);
    int video_stream_index = -1;
    char output_path[PATH_MAX];
    FILE *playlist = NULL;

    // State for every stream to handle corruption and offsets
    StreamState *states = NULL;

    // Install signal handlers for clean shutdown
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (avformat_open_input(&input_ctx, input_file, NULL, NULL) != 0) return 2;
    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        avformat_close_input(&input_ctx);
        return 3;
    }

    int64_t input_size = avio_size(input_ctx->pb);

    states = calloc(input_ctx->nb_streams, sizeof(StreamState));
    if (!states) {
        avformat_close_input(&input_ctx);
        return AVERROR(ENOMEM);
    }

    // Initialize last_dts to a safe negative
    for (unsigned int i = 0; i < input_ctx->nb_streams; i++)
        states[i].last_dts = AV_NOPTS_VALUE;

    for (unsigned int i = 0; i < input_ctx->nb_streams; i++) {
        if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = (int)i;
            break;
        }
    }

    // Stream mapping: input stream index -> output stream index (-1 = skip)
    int *stream_map = calloc(input_ctx->nb_streams, sizeof(int));
    if (!stream_map) {
        free(states);
        avformat_close_input(&input_ctx);
        return AVERROR(ENOMEM);
    }

    // Track segment timing for playlist durations
    int64_t segment_start_video_dts = 0;
    int64_t last_video_dts = 0;
    int     waiting_for_keyframe = 0;
    int     out_nb_streams = 0;

    snprintf(output_path, sizeof(output_path), "%s/%s_%0*d%s", output_dir, base_name, width, current_segment, ext);
    if (open_output_segment(input_ctx, &output_ctx, output_path, max_bytes, stream_map, &out_nb_streams) < 0) {
        free(stream_map);
        free(states);
        avformat_close_input(&input_ctx);
        return -1;
    }

    if (make_playlist) {
        char playlist_path[PATH_MAX];
        snprintf(playlist_path, sizeof(playlist_path), "%s/%s.m3u8", output_dir, base_name);
        playlist = fopen(playlist_path, "w");
        if (playlist) {
            fprintf(playlist, "#EXTM3U\n");
        }
    }

    int64_t split_threshold = max_bytes - SAFETY_MARGIN_BYTES;

    while (!g_interrupted && av_read_frame(input_ctx, &packet) >= 0) {
        // Skip streams not mapped to the output (data, timecode, etc.)
        if (stream_map[packet.stream_index] < 0) {
            av_packet_unref(&packet);
            continue;
        }

        AVStream *in_stream = input_ctx->streams[packet.stream_index];
        AVStream *out_stream = output_ctx->streams[stream_map[packet.stream_index]];
        StreamState *st = &states[packet.stream_index];

        // 1. SANITIZER: Fix broken input timestamps
        if (packet.pts == AV_NOPTS_VALUE) packet.pts = packet.dts;
        if (packet.dts == AV_NOPTS_VALUE) packet.dts = packet.pts;
        if (packet.dts == AV_NOPTS_VALUE) { packet.dts = 0; packet.pts = 0; }

        // 2. MONOTONICITY: Ensure time never goes backwards (fixes "clips" issue)
        if (st->last_dts != AV_NOPTS_VALUE && packet.dts <= st->last_dts) {
            packet.dts = st->last_dts + 1;
            if (packet.pts < packet.dts) packet.pts = packet.dts;
        }
        st->last_dts = packet.dts;

        // Track video DTS for playlist duration calculation
        int is_video = (packet.stream_index == video_stream_index);
        if (is_video) last_video_dts = packet.dts;

        // 3. SPLIT LOGIC
        int64_t current_size = avio_tell(output_ctx->pb);
        int is_keyframe = is_video && (packet.flags & AV_PKT_FLAG_KEY);

        int do_split = 0;
        if (current_size > split_threshold && is_keyframe) {
            // Ideal: split on video keyframe after passing soft threshold
            do_split = 1;
        } else if (current_size >= max_bytes) {
            // Hard limit: must split even without keyframe
            do_split = 1;
            if (!is_keyframe) {
                waiting_for_keyframe = 1;
            }
        }

        if (do_split) {
            // Calculate segment duration from video stream
            double seg_duration = 0.0;
            if (video_stream_index >= 0) {
                AVRational tb = input_ctx->streams[video_stream_index]->time_base;
                seg_duration = (double)(last_video_dts - segment_start_video_dts) * av_q2d(tb);
            }

            close_output_segment(&output_ctx);

            // Write completed segment to playlist
            if (playlist) {
                fprintf(playlist, "#EXTINF:%.3f,\n%s_%0*d%s\n",
                        seg_duration > 0.0 ? seg_duration : 0.0,
                        base_name, width, current_segment, ext);
            }

            current_segment++;
            segment_start_video_dts = last_video_dts;

            // Progress
            if (input_size > 0) {
                int64_t pos = avio_tell(input_ctx->pb);
                double pct = 100.0 * (double)pos / (double)input_size;
                if (pct > 100.0) pct = 100.0;
                printf("\r Segment %d/%d  [%.1f%%]", current_segment - 1, total_segments, pct);
                fflush(stdout);
            }

            snprintf(output_path, sizeof(output_path), "%s/%s_%0*d%s", output_dir, base_name, width, current_segment, ext);
            if (open_output_segment(input_ctx, &output_ctx, output_path, max_bytes, stream_map, NULL) < 0) {
                av_packet_unref(&packet);
                break;
            }

            // Reset offsets for new segment so it starts at 0
            for (unsigned int i = 0; i < input_ctx->nb_streams; i++) {
                states[i].initialized = 0;
                // Do NOT reset last_dts — we need monotonicity across the input stream
            }
        }

        // 4. After a forced (non-keyframe) split, drop video packets until the
        //    next keyframe so the new segment starts cleanly and is independently playable.
        //    Audio/subtitle packets pass through to maintain continuity.
        if (waiting_for_keyframe) {
            if (is_video) {
                if (is_keyframe) {
                    waiting_for_keyframe = 0;
                } else {
                    av_packet_unref(&packet);
                    continue;
                }
            }
        }

        // 5. OFFSET CALCULATION (Zero-base the new segment)
        if (!st->initialized) {
            st->pts_offset = packet.pts;
            st->dts_offset = packet.dts;
            st->initialized = 1;
        }

        av_packet_rescale_ts(&packet, in_stream->time_base, out_stream->time_base);

        int64_t pts_off = av_rescale_q(st->pts_offset, in_stream->time_base, out_stream->time_base);
        int64_t dts_off = av_rescale_q(st->dts_offset, in_stream->time_base, out_stream->time_base);

        packet.pts -= pts_off;
        packet.dts -= dts_off;

        if (packet.dts < 0) packet.dts = 0;
        if (packet.pts < 0) packet.pts = 0;
        // Guarantee PTS >= DTS (required by muxers)
        if (packet.pts < packet.dts) packet.pts = packet.dts;

        packet.stream_index = stream_map[packet.stream_index];
        packet.pos = -1;

        ret = av_interleaved_write_frame(output_ctx, &packet);
        av_packet_unref(&packet);
        if (ret < 0) {
            fprintf(stderr, "\n Warn: Mux error on packet (segment %d). Continuing...\n", current_segment);
        }
    }

    // Finalize last segment
    close_output_segment(&output_ctx);

    if (playlist) {
        // Write last segment entry
        double seg_duration = 0.0;
        if (video_stream_index >= 0) {
            AVRational tb = input_ctx->streams[video_stream_index]->time_base;
            seg_duration = (double)(last_video_dts - segment_start_video_dts) * av_q2d(tb);
        }
        fprintf(playlist, "#EXTINF:%.3f,\n%s_%0*d%s\n",
                seg_duration > 0.0 ? seg_duration : 0.0,
                base_name, width, current_segment, ext);
        fprintf(playlist, "#EXT-X-ENDLIST\n");
        fclose(playlist);
    }

    printf("\r Segment %d/%d  [100.0%%]\n", current_segment, total_segments);

    free(stream_map);
    free(states);
    avformat_close_input(&input_ctx);
    return 0;
}

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>
#include <libavutil/opt.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h> 

// --- Configuration ---
#define SAFETY_MARGIN_BYTES (60 * 1024 * 1024) // 60 MiB buffer (safe for 4GB limit)
static const int64_t FAT32_MAX_BYTES = (4LL * 1024 * 1024 * 1024) - 1;

static const char *PROGRAM_NAME = "Kiri";
static const float PROGRAM_VERSION = 1.2;
static const char *AUTHOR = "crux161";

// --- State Tracking ---
typedef struct {
    int64_t pts_offset;
    int64_t dts_offset;
    int64_t last_dts;      // To enforce monotonicity
    int initialized;
} StreamState;

// --- Prototypes ---
static const char *humanReadable(int64_t bytes);
static int segmentCheck(int64_t bytes, int64_t max_bytes);
static void banner();
static int split_video(const char *input_file, const char *output_dir, const char *base_name, const char *ext,
                       int64_t max_bytes, int total_segments, int make_playlist);
static int make_output_dir(const char *output_dir);
static void split_path(const char *input_file, char *base_name, size_t base_size, char *ext, size_t ext_size);
static int open_output_segment(AVFormatContext *input_ctx, AVFormatContext **output_ctx, const char *output_path);
static void close_output_segment(AVFormatContext **output_ctx);
static int digits_for_int(int value);
static int64_t ask_chunk_size(int64_t file_size, int64_t suggested_max);
static int64_t chunks_for_size(int64_t file_size, int64_t chunk_size);
static int check_disk_space(const char *path, int64_t required_bytes);
static void preallocate_file(const char *path, int64_t size);

// --- Main ---
int main(int argc, char *argv[]) {
    // Silence non-critical errors to keep UI clean, but allow criticals
    av_log_set_level(AV_LOG_ERROR);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input> [--playlist]\n", argv[0]);
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
    printf(" File:\t\t'%s'\n", input_file);
    printf(" Format:\t'%s'\n", formatCtx->iformat->long_name);
    printf(" Size:\t\t %s\n", humanReadable(avio_size(formatCtx->pb)));

    // Correct Time Display (Handle NOPTS)
    if (formatCtx->duration != AV_NOPTS_VALUE) {
        int64_t duration = formatCtx->duration + (formatCtx->duration <= INT64_MAX - 5000 ? 5000 : 0);
        int64_t secs = duration / AV_TIME_BASE;
        printf(" Time:\t\t %02"PRId64":%02"PRId64":%02"PRId64"\n",
            secs / 3600, (secs % 3600) / 60, secs % 60);
    } else {
        printf(" Time:\t\t [Unknown - Stream might be damaged]\n");
    }

    printf(" Streams:\t% 03d\n", formatCtx->nb_streams);

    // Identify Video Stream
    for (int i = 0; i < (int)formatCtx->nb_streams; i++) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            printf(" Codec:\t\t'%s'\n", avcodec_get_name(formatCtx->streams[i]->codecpar->codec_id));
            printf(" Resolution:\t %03d x %03d\n\n", formatCtx->streams[i]->codecpar->width, formatCtx->streams[i]->codecpar->height);
        }
    }

    int64_t file_size = avio_size(formatCtx->pb);
    int chunks = segmentCheck(file_size, FAT32_MAX_BYTES);
    char choice;
    int lock = 1;
    int confirm = 0;

    char base_name[PATH_MAX] = {0};
    char ext[PATH_MAX] = {0};
    split_path(input_file, base_name, sizeof(base_name), ext, sizeof(ext));

    int64_t chosen_chunk_size = 0;
    if (chunks >= 1) { 
        chosen_chunk_size = ask_chunk_size(file_size, FAT32_MAX_BYTES);
        int64_t chosen_chunks = chunks_for_size(file_size, chosen_chunk_size);
        printf(" Selected chunk size: %s\n", humanReadable(chosen_chunk_size));
        printf(" This will produce approximately %" PRId64 " chunk(s).\n", chosen_chunks);

        printf(" A directory named '%s' will be created.\n Do You want to proceed? (Y/N) ", base_name);
        scanf(" %c", &choice);
        while (lock) {
            if (choice == 'y' || choice == 'Y') {
                lock = 0; confirm = 1;
            } else if (choice == 'n' || choice == 'N') {
                printf(" Aborting...\n"); lock = 0;
            } else {
                printf(" Invalid input. (Y/N): ");
                scanf(" %c", &choice);
            }
        }
    }

    if (confirm) {
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
        int ret = split_video(input_file, base_name, base_name, ext, chosen_chunk_size, chunks_for_size(file_size, chosen_chunk_size), make_playlist);
        
        if (ret != 0) {
            fprintf(stderr, "\n !!! SPLIT FAILED !!!\n Error Code: %d (%s)\n", ret, av_err2str(ret));
            avformat_close_input(&formatCtx);
            return ret;
        } else {
            printf("\n Success! Output is in directory '%s/'\n", base_name);
        }
    }

    avformat_close_input(&formatCtx);
    return 0;
}

// --- Helper Functions ---

static const char *humanReadable(int64_t bytes) {
    static char output[200];
    const char *sfx[] = {"B", "KB", "MB", "GB"}; 
    int len = sizeof(sfx) / sizeof(sfx[0]);
    int i = 0;
    double d_bytes = bytes;
    if (bytes > 1024) {
        for (i = 0; (bytes / 1024) > 0 && i < (len - 1); i++, bytes /= 1024) {
            d_bytes = bytes / 1024.0;
        }
    }
    snprintf(output, sizeof(output), "%.2f %s", d_bytes, sfx[i]);
    return output;
}

// macOS/POSIX Disk Space Check
static int check_disk_space(const char *path, int64_t required_bytes) {
    struct statvfs stat;
    if (statvfs(path, &stat) != 0) {
        perror("statvfs");
        return -1;
    }
    int64_t available = (int64_t)stat.f_bavail * stat.f_frsize;
    if (available < (required_bytes + 100 * 1024 * 1024)) { // 100MB Buffer
        printf(" Warning: Low disk space! Need: %s, Have: %s\n", 
               humanReadable(required_bytes), humanReadable(available));
        return -1;
    }
    return 0;
}

// Pre-allocation helper (MacOS / POSIX Optimized)
static void preallocate_file(const char *path, int64_t size) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd == -1) return;

#ifdef __APPLE__
    // macOS specific pre-allocation (F_PREALLOCATE)
    fstore_t store = {F_ALLOCATECONTIG, F_PEOFPOSMODE, 0, size};
    // Try contiguous allocation first (Best for playback)
    if (fcntl(fd, F_PREALLOCATE, &store) == -1) {
        // Fallback to fragmented allocation if contiguous fails
        store.fst_flags = F_ALLOCATEALL;
        fcntl(fd, F_PREALLOCATE, &store);
    }
    // Actually set the logical file size
    ftruncate(fd, size); 
#else
    // Generic POSIX (Linux/Android)
    posix_fallocate(fd, 0, size);
#endif
    
    close(fd);
}

int segmentCheck(int64_t bytes, int64_t max_bytes) {
    if (bytes <= 0) return 1;
    if (bytes > max_bytes) {
        return (int)((bytes + max_bytes - 1) / max_bytes);
    }
    return 1;
}

void banner() {
    printf("\n=======================================\n");
    printf(" %s v.%.02f\t FAT32 FFmpeg Splitter\n", PROGRAM_NAME, PROGRAM_VERSION);
    printf("---------------------------------------\n");
    printf(" %s \n", AUTHOR);
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
    
    printf(" Suggested chunk size: %s\n", humanReadable(suggested_max));
    printf(" Estimated chunks: %" PRId64 "\n", chunks_for_size(file_size, suggested_max));

    while (1) {
        printf("\n Choose chunk size:\n");
        printf("  [1] Suggested max (%" PRId64 " MiB)\n", max_mib);
        printf("  [2] Custom size in MiB (1 - %" PRId64 ")\n", max_mib);
        printf(" Select option (1/2): ");

        if (!fgets(buffer, sizeof(buffer), stdin)) return suggested_max;
        int choice = (int)strtol(buffer, NULL, 10);

        if (choice == 1) return suggested_max;
        if (choice == 2) {
            printf(" Enter chunk size in MiB: ");
            if (!fgets(buffer, sizeof(buffer), stdin)) return suggested_max;
            int64_t custom_mib = strtoll(buffer, NULL, 10);
            if (custom_mib > 0 && custom_mib <= max_mib) {
                return custom_mib * 1024LL * 1024;
            }
            printf(" Invalid size.\n");
            continue;
        }
    }
}

static int open_output_segment(AVFormatContext *input_ctx, AVFormatContext **output_ctx, const char *output_path) {
    // 1. Pre-allocate file on disk to avoid fragmentation
    // We allocate 100MB initially, FFmpeg will grow it, but starting contiguous helps.
    preallocate_file(output_path, 100 * 1024 * 1024); 

    AVFormatContext *out_ctx = NULL;
    int ret = avformat_alloc_output_context2(&out_ctx, NULL, NULL, output_path);
    if (ret < 0 || !out_ctx) return -1;

    for (unsigned int i = 0; i < input_ctx->nb_streams; i++) {
        AVStream *in_stream = input_ctx->streams[i];
        AVStream *out_stream = avformat_new_stream(out_ctx, NULL);
        if (!out_stream) { avformat_free_context(out_ctx); return -1; }
        
        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) { avformat_free_context(out_ctx); return ret; }
        
        out_stream->time_base = in_stream->time_base;
        out_stream->codecpar->codec_tag = 0; 
    }

    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out_ctx->pb, output_path, AVIO_FLAG_WRITE);
        if (ret < 0) { avformat_free_context(out_ctx); return ret; }
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

// --- The Core Splitter (Robust Version) ---
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

    if (avformat_open_input(&input_ctx, input_file, NULL, NULL) != 0) return 2;
    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        avformat_close_input(&input_ctx);
        return 3;
    }

    states = calloc(input_ctx->nb_streams, sizeof(StreamState));
    // Initialize last_dts to a safe negative
    for(unsigned int i=0; i<input_ctx->nb_streams; i++) states[i].last_dts = AV_NOPTS_VALUE;

    for (unsigned int i = 0; i < input_ctx->nb_streams; i++) {
        if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = (int)i;
            break;
        }
    }

    snprintf(output_path, sizeof(output_path), "%s/%s_%0*d%s", output_dir, base_name, width, current_segment, ext);
    if (open_output_segment(input_ctx, &output_ctx, output_path) < 0) {
        free(states);
        avformat_close_input(&input_ctx);
        return -1;
    }

    if (make_playlist) {
        char playlist_path[PATH_MAX];
        snprintf(playlist_path, sizeof(playlist_path), "%s/%s.m3u8", output_dir, base_name);
        playlist = fopen(playlist_path, "w");
        if (playlist) {
            fprintf(playlist, "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:0\n");
            fprintf(playlist, "#EXTINF:-1,\n%s_%0*d%s\n", base_name, width, current_segment, ext);
        }
    }

    int64_t split_threshold = max_bytes - SAFETY_MARGIN_BYTES;

    while (av_read_frame(input_ctx, &packet) >= 0) {
        AVStream *in_stream = input_ctx->streams[packet.stream_index];
        AVStream *out_stream = output_ctx->streams[packet.stream_index];
        StreamState *st = &states[packet.stream_index];

        // 1. SANITIZER: Fix broken input timestamps
        if (packet.pts == AV_NOPTS_VALUE) packet.pts = packet.dts;
        if (packet.dts == AV_NOPTS_VALUE) packet.dts = packet.pts;
        if (packet.dts == AV_NOPTS_VALUE) packet.dts = 0; // Last resort

        // 2. MONOTONICITY: Ensure time never goes backwards (fixes "clips" issue)
        if (st->last_dts != AV_NOPTS_VALUE && packet.dts < st->last_dts) {
            // Corruption detected: Packet timestamp jumped back. Force it forward.
            packet.dts = st->last_dts + 1;
            if (packet.pts < packet.dts) packet.pts = packet.dts;
        }
        st->last_dts = packet.dts;

        // 3. SPLIT LOGIC
        int64_t current_size = avio_tell(output_ctx->pb);
        int is_keyframe = (packet.stream_index == video_stream_index) && (packet.flags & AV_PKT_FLAG_KEY);
        
        if ((current_size > split_threshold && is_keyframe) || (current_size >= max_bytes)) {
            close_output_segment(&output_ctx);
            current_segment++;
            
            snprintf(output_path, sizeof(output_path), "%s/%s_%0*d%s", output_dir, base_name, width, current_segment, ext);
            if (open_output_segment(input_ctx, &output_ctx, output_path) < 0) {
                av_packet_unref(&packet);
                break;
            }

            if (playlist) fprintf(playlist, "#EXTINF:-1,\n%s_%0*d%s\n", base_name, width, current_segment, ext);

            // Reset offsets for new segment so it starts at 0
            for(unsigned int i=0; i<input_ctx->nb_streams; i++) {
                states[i].initialized = 0; 
                // Do NOT reset last_dts, we need to track flow across segments
            }
        }

        // 4. OFFSET CALCULATION (Zero-base the new segment)
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

        if (packet.pts < 0) packet.pts = 0;
        if (packet.dts < 0) packet.dts = 0;

        packet.pos = -1;
        
        ret = av_interleaved_write_frame(output_ctx, &packet);
        av_packet_unref(&packet);
        if (ret < 0) {
            // If muxer fails on one packet, log it but TRY to continue
            fprintf(stderr, "Warn: Mux failed on packet. Continuing...\n");
            continue; 
        }
    }

    close_output_segment(&output_ctx);
    if (playlist) {
        fprintf(playlist, "#EXT-X-ENDLIST\n");
        fclose(playlist);
    }
    
    free(states);
    avformat_close_input(&input_ctx);
    return 0;
}

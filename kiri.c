#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// prototypes
static const char *humanReadable(int64_t bytes);
int segmentCheck(int64_t bytes, int64_t max_bytes);
void banner();
static int split_video(const char *input_file, const char *output_dir, const char *base_name, const char *ext,
                       int64_t max_bytes, int total_segments, int make_playlist);
static int make_output_dir(const char *output_dir);
static void split_path(const char *input_file, char *base_name, size_t base_size, char *ext, size_t ext_size);
static int open_output_segment(AVFormatContext *input_ctx, AVFormatContext **output_ctx, const char *output_path);
static void close_output_segment(AVFormatContext **output_ctx);
static int digits_for_int(int value);
static int64_t ask_chunk_size(int64_t file_size, int64_t suggested_max);
static int64_t chunks_for_size(int64_t file_size, int64_t chunk_size);

static const char *PROGRAM_NAME = "Kiri";
static const float PROGRAM_VERSION = 1.0;
static const char *AUTHOR = "crux161 <8zwsl79i@anonaddy.com>";
static const int64_t FAT32_MAX_BYTES = (4LL * 1024 * 1024 * 1024) - 1;

int main(int argc, char *argv[]) {

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
                fprintf(stderr, "Error opening input file...\n");
                return 2;
        }

        if (avformat_find_stream_info(formatCtx, NULL) < 0) {
                fprintf(stderr, "Error finding stream information...\n");
                avformat_close_input(&formatCtx);
                return 3;
        }

        // this is too noisy, but useful
        //av_dump_format(formatCtx, 0, input_file, 0);

        banner();
        // File Info to confirm before chopping!
        printf(" File:\t\t'%s'\n", input_file);
        printf(" Format:\t'%s'\n", formatCtx->iformat->long_name);
        printf(" Size:\t\t %s\n", humanReadable(avio_size(formatCtx->pb)));

        printf(" Time:\t\t %02d:%02d:%02d\n", (int)formatCtx->duration / AV_TIME_BASE / 3600, ((int)formatCtx->duration / AV_TIME_BASE / 60) % 60, (int)formatCtx->duration / AV_TIME_BASE % 60);
        printf(" Streams:\t% 03d\n", formatCtx->nb_streams);

        // Print video stream information
        for (int i = 0; i < (int) formatCtx->nb_streams; i++) {
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
        if (chunks > 1) {
                chosen_chunk_size = ask_chunk_size(file_size, FAT32_MAX_BYTES);
                int64_t chosen_chunks = chunks_for_size(file_size, chosen_chunk_size);
                printf(" Selected chunk size: %s\n", humanReadable(chosen_chunk_size));
                printf(" This will produce approximately %" PRId64 " chunk(s).\n", chosen_chunks);

                printf(" A directory named '%s' will be created with a copy of the movie in compatible segments.\n Do You want to proceed? (Y/N) ", base_name);
                scanf(" %c", &choice);
                while (lock) {
                    switch (choice) {
                        case 'y':
                        case 'Y':
                                printf(" Proceeding...\n");
                                lock = 0;
                                confirm = 1;
                                break;
                        case 'n':
                        case 'N':
                                printf(" Aborting...\n");
                                lock = 0;
                                break;
                        default:
                                printf(" Invalid input, enter 'y/Y' or 'n/N'...\n");
                                break;
                    }
                }
        }

        if (confirm) {
                if (make_output_dir(base_name) != 0) {
                        fprintf(stderr, " Error creating output directory '%s'.\n", base_name);
                        avformat_close_input(&formatCtx);
                        return 4;
                }

                int ret = split_video(input_file, base_name, base_name, ext, chosen_chunk_size, chunks_for_size(file_size, chosen_chunk_size), make_playlist);
                if (ret != 0) {
                        fprintf(stderr, " Error during splitting.\n");
                        avformat_close_input(&formatCtx);
                        return ret;
                }
        }

        avformat_close_input(&formatCtx);

        return 0;
}


// Thank you thank you: @dgoguerra [https://gist.github.com/dgoguerra/7194777]
static const char *humanReadable(int64_t bytes) {
        static char output[200];

        char *sfx[] = {"B", "KB", "MB", "GB", "TB"};
        char len    = sizeof(sfx) / sizeof(sfx[0]);
        int i = 0, b = 0;
        double LBytes = bytes;
        double BBytes = 0;
        if (bytes > 1024 ) {
                for (i = 0; (bytes / 1024) > 0 && i < (len - 1); i++, bytes /= 1024) {
                        LBytes = bytes / 1024.0;
                }
        }
        b = (i + 1);
        BBytes = (LBytes / 1024);
        sprintf(output, "%.02lf %s (%.02lf %s)", BBytes, sfx[b], LBytes, sfx[i]);
        return output;
}

int segmentCheck(int64_t bytes, int64_t max_bytes) {
        int64_t num_chunks = 1;
        if (bytes <= 0) {
                printf(" Invalid file size.\n");
                return 1;
        }

        if (bytes > max_bytes) {
                num_chunks = (bytes + max_bytes - 1) / max_bytes;
                printf(" Number of chunks needed: %" PRId64 "\n", num_chunks);
        } else {
                printf(" File size is within the limit of 4 GB. Not splitting...\n");
        }

        return (int) num_chunks;
}

void banner() {
        printf("\n=======================================\n");
        printf(" %s v.%.02f\t FAT32 FFmpeg Splitter\n", PROGRAM_NAME, PROGRAM_VERSION);
        printf("---------------------------------------\n");
        printf(" %s \n", AUTHOR);
        // build info, other good stuff can go here later
        printf("=======================================\n");

}

static int make_output_dir(const char *output_dir) {
        if (mkdir(output_dir, 0755) != 0) {
                if (errno == EEXIST) {
                        return 0;
                }
                return -1;
        }
        return 0;
}

static void split_path(const char *input_file, char *base_name, size_t base_size, char *ext, size_t ext_size) {
        const char *slash = strrchr(input_file, '/');
        const char *filename = slash ? (slash + 1) : input_file;
        const char *dot = strrchr(filename, '.');
        size_t base_len = dot ? (size_t)(dot - filename) : strlen(filename);

        if (base_len >= base_size) {
                base_len = base_size - 1;
        }
        memcpy(base_name, filename, base_len);
        base_name[base_len] = '\0';

        if (dot != NULL) {
                size_t ext_len = strlen(dot);
                if (ext_len >= ext_size) {
                        ext_len = ext_size - 1;
                }
                memcpy(ext, dot, ext_len);
                ext[ext_len] = '\0';
        } else {
                ext[0] = '\0';
        }
}

static int digits_for_int(int value) {
        int digits = 1;
        while (value >= 10) {
                value /= 10;
                digits++;
        }
        return digits;
}

static int64_t chunks_for_size(int64_t file_size, int64_t chunk_size) {
        if (file_size <= 0 || chunk_size <= 0) {
                return 1;
        }
        return (file_size + chunk_size - 1) / chunk_size;
}

static int64_t ask_chunk_size(int64_t file_size, int64_t suggested_max) {
        char buffer[64];
        int64_t max_mib = suggested_max / (1024LL * 1024);
        int choice = 0;

        printf(" Suggested chunk size: %s\n", humanReadable(suggested_max));
        printf(" Estimated chunks at suggested size: %" PRId64 "\n", chunks_for_size(file_size, suggested_max));

        while (1) {
                printf("\n Choose chunk size:\n");
                printf("  [1] Suggested max (%" PRId64 " MiB)\n", max_mib);
                printf("  [2] Custom size in MiB (1 - %" PRId64 ")\n", max_mib);
                printf(" Select option (1/2): ");

                if (!fgets(buffer, sizeof(buffer), stdin)) {
                        return suggested_max;
                }
                choice = (int)strtol(buffer, NULL, 10);

                if (choice == 1) {
                        return suggested_max;
                }
                if (choice == 2) {
                        printf(" Enter chunk size in MiB: ");
                        if (!fgets(buffer, sizeof(buffer), stdin)) {
                                return suggested_max;
                        }
                        int64_t custom_mib = strtoll(buffer, NULL, 10);
                        if (custom_mib > 0 && custom_mib <= max_mib) {
                                return custom_mib * 1024LL * 1024;
                        }
                        printf(" Invalid size. Please enter a value between 1 and %" PRId64 ".\n", max_mib);
                        continue;
                }

                printf(" Invalid selection. Please choose 1 or 2.\n");
        }
}

static int open_output_segment(AVFormatContext *input_ctx, AVFormatContext **output_ctx, const char *output_path) {
        AVFormatContext *out_ctx = NULL;
        int ret = avformat_alloc_output_context2(&out_ctx, NULL, NULL, output_path);
        if (ret < 0 || out_ctx == NULL) {
                return ret < 0 ? ret : -1;
        }

        for (unsigned int i = 0; i < input_ctx->nb_streams; i++) {
                AVStream *in_stream = input_ctx->streams[i];
                AVStream *out_stream = avformat_new_stream(out_ctx, NULL);
                if (!out_stream) {
                        avformat_free_context(out_ctx);
                        return -1;
                }
                ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
                if (ret < 0) {
                        avformat_free_context(out_ctx);
                        return ret;
                }
                out_stream->time_base = in_stream->time_base;
        }

        if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
                ret = avio_open(&out_ctx->pb, output_path, AVIO_FLAG_WRITE);
                if (ret < 0) {
                        avformat_free_context(out_ctx);
                        return ret;
                }
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
        if (output_ctx == NULL || *output_ctx == NULL) {
                return;
        }
        av_write_trailer(*output_ctx);
        if (!((*output_ctx)->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&(*output_ctx)->pb);
        }
        avformat_free_context(*output_ctx);
        *output_ctx = NULL;
}

static int split_video(const char *input_file, const char *output_dir, const char *base_name, const char *ext,
                       int64_t max_bytes, int total_segments, int make_playlist) {
        AVFormatContext *input_ctx = NULL;
        AVFormatContext *output_ctx = NULL;
        AVPacket packet;
        int ret = 0;
        int current_segment = 1;
        int width = digits_for_int(total_segments);
        int video_stream_index = -1;
        int split_on_non_keyframe = 0;
        char output_path[PATH_MAX];
        FILE *playlist = NULL;

        if (avformat_open_input(&input_ctx, input_file, NULL, NULL) != 0) {
                return 2;
        }

        if (avformat_find_stream_info(input_ctx, NULL) < 0) {
                avformat_close_input(&input_ctx);
                return 3;
        }

        for (unsigned int i = 0; i < input_ctx->nb_streams; i++) {
                if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        video_stream_index = (int)i;
                        break;
                }
        }

        snprintf(output_path, sizeof(output_path), "%s/%s_%0*d%s", output_dir, base_name, width, current_segment, ext);
        ret = open_output_segment(input_ctx, &output_ctx, output_path);
        if (ret < 0) {
            avformat_close_input(&input_ctx);
            return ret;
        }

        if (make_playlist) {
                char playlist_path[PATH_MAX];
                snprintf(playlist_path, sizeof(playlist_path), "%s/%s.m3u8", output_dir, base_name);
                playlist = fopen(playlist_path, "w");
                if (playlist) {
                        fprintf(playlist, "#EXTM3U\n");
                        fprintf(playlist, "%s_%0*d%s\n", base_name, width, current_segment, ext);
                }
        }

        while (av_read_frame(input_ctx, &packet) >= 0) {
                AVStream *in_stream = input_ctx->streams[packet.stream_index];
                AVStream *out_stream = output_ctx->streams[packet.stream_index];

                int64_t current_size = avio_tell(output_ctx->pb);
                if (current_size >= 0 && (current_size + packet.size > max_bytes)) {
                        if (packet.stream_index != video_stream_index || !(packet.flags & AV_PKT_FLAG_KEY)) {
                                split_on_non_keyframe = 1;
                        }

                        close_output_segment(&output_ctx);
                        current_segment++;
                        snprintf(output_path, sizeof(output_path), "%s/%s_%0*d%s", output_dir, base_name, width, current_segment, ext);
                        ret = open_output_segment(input_ctx, &output_ctx, output_path);
                        if (ret < 0) {
                                av_packet_unref(&packet);
                                break;
                        }

                        if (playlist) {
                                fprintf(playlist, "%s_%0*d%s\n", base_name, width, current_segment, ext);
                        }
                }

                av_packet_rescale_ts(&packet, in_stream->time_base, out_stream->time_base);
                packet.pos = -1;
                ret = av_interleaved_write_frame(output_ctx, &packet);
                av_packet_unref(&packet);
                if (ret < 0) {
                        break;
                }
        }

        close_output_segment(&output_ctx);

        if (playlist) {
                fclose(playlist);
        }

        avformat_close_input(&input_ctx);

        if (split_on_non_keyframe) {
                fprintf(stderr, " Warning: One or more segments began on a non-keyframe to stay under the FAT32 limit.\n");
        }

        return ret < 0 ? ret : 0;
}

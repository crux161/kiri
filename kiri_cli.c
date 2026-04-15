/*
 * kiri_cli.c — interactive CLI for libkiri.
 *
 * All UI lives here. The library is I/O-free; this program owns stdout,
 * stdin, signals, and prompting. Build: kiri_cli.o links against libkiri.
 */
#include "kiri.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MIN_CHUNK_MIB 100

static volatile sig_atomic_t g_interrupted = 0;

static void on_signal(int sig) { (void)sig; g_interrupted = 1; }

/* --- UI helpers ------------------------------------------------------- */

static void human_readable(int64_t bytes, char *buf, size_t buf_size) {
    const char *sfx[] = { "B", "KB", "MB", "GB" };
    int len = sizeof(sfx) / sizeof(sfx[0]);
    int i = 0;
    double d = (double)bytes;
    if (bytes > 1024) {
        for (i = 0; (bytes / 1024) > 0 && i < (len - 1); i++, bytes /= 1024) {
            d = bytes / 1024.0;
        }
    }
    snprintf(buf, buf_size, "%.2f %s", d, sfx[i]);
}

static void banner(void) {
    printf("\n=======================================\n");
    printf(" Kiri (libkiri %s)\t FAT32 FFmpeg Splitter\n", kiri_version_string());
    printf("---------------------------------------\n");
    printf(" crux161\n");
    printf("=======================================\n");
}

static int make_output_dir(const char *path) {
    if (mkdir(path, 0755) != 0) {
        if (errno == EEXIST) return 0;
        return -1;
    }
    return 0;
}

/* --- Library callbacks ----------------------------------------------- */

static int progress_cb(void *user, int segment, int total,
                       int64_t done, int64_t total_bytes) {
    (void)user;
    if (g_interrupted) return 1;               /* cooperative cancel */
    double pct = total_bytes > 0 ? 100.0 * (double)done / (double)total_bytes : 0.0;
    if (pct > 100.0) pct = 100.0;
    printf("\r Segment %d/%d  [%.1f%%]", segment, total, pct);
    fflush(stdout);
    return 0;
}

static void log_cb(void *user, kiri_log_level level, const char *msg) {
    (void)user;
    const char *prefix = "info";
    switch (level) {
        case KIRI_LOG_ERROR:   prefix = "error"; break;
        case KIRI_LOG_WARN:    prefix = "warn";  break;
        case KIRI_LOG_INFO:    prefix = "info";  break;
        case KIRI_LOG_VERBOSE: prefix = "debug"; break;
    }
    fprintf(stderr, "\n [%s] %s\n", prefix, msg);
}

/* --- Interactive prompting ------------------------------------------- */

static int64_t ask_chunk_size(int64_t file_size, int64_t suggested_max) {
    char buffer[64], hr[64];
    int64_t max_mib = suggested_max / (1024LL * 1024);

    human_readable(suggested_max, hr, sizeof(hr));
    printf(" Suggested chunk size: %s\n", hr);
    printf(" Estimated chunks: %" PRId64 "\n",
           kiri_chunk_count(file_size, suggested_max));

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
            int64_t mib = strtoll(buffer, NULL, 10);
            if (mib >= MIN_CHUNK_MIB && mib <= max_mib) return mib * 1024LL * 1024;
            printf(" Invalid size. Must be between %d and %" PRId64 " MiB.\n",
                   MIN_CHUNK_MIB, max_mib);
        }
    }
}

/* --- Main ------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input> [--playlist|-p]\n", argv[0]);
        return 1;
    }

    const char *input_file = argv[1];
    int make_playlist = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--playlist") || !strcmp(argv[i], "-p")) {
            make_playlist = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    /* Probe first */
    kiri_input_info info;
    kiri_status s = kiri_probe(input_file, &info);
    if (s != KIRI_OK) {
        fprintf(stderr, "Error: probe failed: %s\n", kiri_strerror(s));
        return 2;
    }

    banner();
    char hr[64];
    printf(" File:\t\t'%s'\n", input_file);
    printf(" Format:\t'%s'\n", info.format_long_name);
    human_readable(info.size_bytes, hr, sizeof(hr));
    printf(" Size:\t\t %s\n", hr);
    if (info.duration_us >= 0) {
        int64_t secs = (info.duration_us + 5000) / 1000000;
        printf(" Time:\t\t %02" PRId64 ":%02" PRId64 ":%02" PRId64 "\n",
               secs / 3600, (secs % 3600) / 60, secs % 60);
    } else {
        printf(" Time:\t\t [Unknown - Stream might be damaged]\n");
    }
    printf(" Streams:\t %d\n", info.nb_streams);
    if (info.has_video) {
        printf(" Codec:\t\t'%s'\n", info.video_codec_name);
        printf(" Resolution:\t %d x %d\n\n", info.video_width, info.video_height);
    } else {
        printf(" Warning: No video stream — keyframe-aware splitting unavailable.\n\n");
    }

    /* Prompt */
    int64_t max_bytes = ask_chunk_size(info.size_bytes, kiri_fat32_max_bytes());
    int64_t chunks    = kiri_chunk_count(info.size_bytes, max_bytes);
    human_readable(max_bytes, hr, sizeof(hr));
    printf(" Selected chunk size: %s\n", hr);
    printf(" This will produce approximately %" PRId64 " chunk(s).\n", chunks);

    /* Derive basename for output dir name */
    const char *slash = strrchr(input_file, '/');
    const char *fname = slash ? slash + 1 : input_file;
    const char *dot   = strrchr(fname, '.');
    char base[PATH_MAX] = {0};
    size_t blen = dot ? (size_t)(dot - fname) : strlen(fname);
    if (blen >= sizeof(base)) blen = sizeof(base) - 1;
    memcpy(base, fname, blen);

    printf(" A directory named '%s' will be created.\n Proceed? (Y/N) ", base);
    fflush(stdout);

    char inbuf[64];
    int confirmed = 0;
    while (fgets(inbuf, sizeof(inbuf), stdin)) {
        if (inbuf[0] == 'y' || inbuf[0] == 'Y') { confirmed = 1; break; }
        if (inbuf[0] == 'n' || inbuf[0] == 'N') { printf(" Aborting...\n"); break; }
        printf(" Invalid input. (Y/N): ");
        fflush(stdout);
    }
    if (!confirmed) return 0;

    if (make_output_dir(base) != 0) {
        fprintf(stderr, " Error creating output directory '%s'.\n", base);
        return 4;
    }

    /* Install signal handlers for cooperative cancellation */
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Drive the library */
    kiri_split_options opts = {
        .input_path        = input_file,
        .output_dir        = base,
        .max_bytes         = max_bytes,
        .make_playlist     = make_playlist,
        .preallocate_bytes = -1,            /* use max_bytes */
        .on_progress       = progress_cb,
        .on_log            = log_cb,
    };

    printf(" Proceeding with split...\n");
    s = kiri_split(&opts);

    if (s == KIRI_ERR_INTERRUPTED) {
        printf("\n Interrupted. Partial output in '%s/'.\n", base);
        return 0;
    }
    if (s != KIRI_OK) {
        fprintf(stderr, "\n !!! SPLIT FAILED !!!\n %s\n", kiri_strerror(s));
        return (int)(-s);
    }
    printf("\n Success! Output is in directory '%s/'\n", base);
    return 0;
}

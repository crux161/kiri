#include <libavformat/avformat.h>
#include <libgen.h>

// prototypes
static const char *humanReadable(int64_t bytes);
void banner();

static const char *PROGRAM_NAME = "Kiri";
static const float PROGRAM_VERSION = 1.0;
static const char *AUTHOR = "crux161 <tunic_09_coyotes@icloud.com>";

int main(int argc, char *argv[]) {
	
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <input>\n", argv[0]);
		return 1;
	}


	const char *input_file = argv[1];

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
	
	int chunks = segmentCheck(avio_size(formatCtx->pb));
	char choice;
	int lock = 1;
	int confirm = 0;

	char *operation_title = basename(input_file);
	char *dot = strrchr(operation_title, '.');
	if (dot != NULL) {
		*dot = '\0';
	}

	if (chunks > 1) {
		printf(" A directory named '%s' will be created with a copy of the movie in compatible segments.\n Do You want to proceed? (Y/N) ", operation_title);
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
		// TODO: implement split~!
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

int segmentCheck(int64_t bytes) {
    int64_t num_chunks = 0;
    if (bytes <= 0) {
        printf(" Invalid file size.\n");
        return 1;
    }

    if (bytes > (4LL * 1024 * 1024 * 1024)) {
        int64_t chunk_size = 4LL * 1024 * 1024 * 1024; // 4 GB
        num_chunks = (bytes + chunk_size - 1) / chunk_size;

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

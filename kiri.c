#include <libavformat/avformat.h>

int main(int argc, char *argv[]) {
	
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <input>\n", argv[0]);
		return 1;
	}


	const char *input_file = argv[1];

	av_register_all();

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

	//av_dump_format(formatCtx, 0, input_file, 0);

	// Print overall format information
    	printf("File:           '%s'\n", input_file);
    	printf("Format:         '%s'\n", formatCtx->iformat->name);
    	//printf("Size:           %.2f GB (%.2f MB)\n", formatCtx->file_size / (float)(1024 * 1024 * 1024), formatCtx->file_size / (float)(1024 * 1024));
    	
	int64_t fileSz = avio_size(formatCtx->pb);  // (float)(1024 * 1024 * 1024);
	printf("Size:		%ld\n", fileSz);

	printf("Time:           %02d:%02d:%02d\n", (int)formatCtx->duration / AV_TIME_BASE / 3600, ((int)formatCtx->duration / AV_TIME_BASE / 60) % 60, (int)formatCtx->duration / AV_TIME_BASE % 60);
    	printf("Segments:       %d\n", formatCtx->nb_streams);

    	// Print video stream information
    	for (int i = 0; i < formatCtx->nb_streams; i++) {
        	if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        	    printf("Codec:          '%s'\n", avcodec_get_name(formatCtx->streams[i]->codecpar->codec_id));
        	    printf("Resolution:     %d x %d\n", formatCtx->streams[i]->codecpar->width, formatCtx->streams[i]->codecpar->height);
        	}
    	}

	avformat_close_input(&formatCtx);

	return 0;
}

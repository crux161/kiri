CC = gcc
CFLAGS = -Wall -Wextra -std=c17

SRC = kiri.c
EXEC = kiri

all:
        $(CC) $(CFLAGS) `pkg-config --cflags --libs libavutil libavformat libavcodec` -o $(EXEC) $(SRC)

clean:
        rm -f $(OBJ) $(EXEC)

re:
        make clean
        make
        if [ -f "bynny.mp4" ]; then
          kiri ./bunny.mp4
        else
          wget --output-document bunny.mp4 http://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4
          kiri ./bunny.mp4
        fi

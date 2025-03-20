CC = clang
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
	./kiri ./bunny.mp4

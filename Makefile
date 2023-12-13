CC = gcc
CFLAGS = -Wall -Wextra -std=c17 
LDFLAGS = -lavformat -lavcodec -lavutil

SRC = kiri.c
OBJ = $(SRC:.c=.o)
EXEC = kiri

all: $(EXEC)

$(EXEC): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(EXEC)

re:
	make clean
	make
	./kiri bunny.mp4

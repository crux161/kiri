CC = gcc
CFLAGS = -Wall -Wextra -std=c11
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


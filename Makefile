CC = gcc
CFLAGS = -Wall -Wextra -pthread
INCLUDES = -I./include
SRC = src/main.c src/filesystem.c src/scheduler.c src/commands.c src/paging.c src/globals.c
OBJ = $(SRC:.c=.o)
EXEC = mini_fs

all: $(EXEC)

$(EXEC): $(OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJ) $(EXEC)

.PHONY: all clean
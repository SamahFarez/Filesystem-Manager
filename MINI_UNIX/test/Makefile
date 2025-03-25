CC = gcc
CFLAGS = -Wall -Wextra -pthread
INCLUDES = -I../include
SRC = test_fs.c
OBJ = $(SRC:.c=.o)
EXEC = test_fs

all: $(EXEC)

$(EXEC): $(OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ ../src/filesystem.c ../src/paging.c

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJ) $(EXEC)

.PHONY: all clean
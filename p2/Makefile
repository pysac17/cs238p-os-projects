# CS238P Project 2 - build inside the x86-64 Linux VM.
CC = gcc
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Werror -O2 -g
DEST = cs238
SRCS = main.c system.c scheduler.c
OBJS = $(SRCS:.c=.o)
all: $(DEST)
$(DEST): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
clean:
	rm -f $(DEST) *.o *.d
.PHONY: all clean
-include $(OBJS:.o=.d)

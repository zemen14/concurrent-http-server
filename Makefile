CC = clang
CFLAGS = -Wall -Wextra -Werror -pedantic -std=c11 -pthread

OBJS = main.o queue.o util.o http.o

all: httpserver

httpserver: $(OBJS)
	$(CC) $(CFLAGS) -o httpserver $(OBJS)

main.o: main.c queue.h util.h http.h
queue.o: queue.c queue.h
util.o: util.c util.h
http.o: http.c http.h util.h

clean:
	rm -f httpserver *.o

.PHONY: all clean

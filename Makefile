CC = gcc
CFLAGS = -Wall -Werror

TARGET = fs
OBJS = fs.o
HEADERS = fs-sim.h

fs: $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

compile: fs.o

fs.o: fs.c $(HEADERS)
	$(CC) $(CFLAGS) -c fs.c

clean:
	rm -f $(OBJS) $(TARGET)
	rm -f stdout.txt
	rm -f stderr.txt
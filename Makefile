SHELL = /bin/sh
CC = gcc
CFLAGS = -Wall -ansi -pedantic -g -ggdb3 -D_BSD_SOURCE -D_POSIX_C_SOURCE
MAIN = mush2
OBJS = $(MAIN).o
all: $(MAIN)

$(MAIN): $(MAIN).o
	$(CC) $(CFLAGS) -o $(MAIN) $(OBJS) -L ~pn-cs357/Given/Mush/lib64 -lmush

mush2.o: mush2.c
	$(CC) -c $(CFLAGS) -I ~pn-cs357/Given/Mush/include mush2.c

test: $(MAIN)
	valgrind --leak-check=full \
		 --show-leak-kinds=all \
		 --tool=memcheck \
		 --track-origins=yes \
         	 --verbose \
         	 --log-file=valgrind-out \
		 ./$(MAIN) commands 

clean: 
	rm -f $(OBJS) *~

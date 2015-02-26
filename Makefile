CC=gcc
CFLAGS = -g -Wall
OBJ = http.o server.o

server: $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o server

http.o: http.h

server.o: http.h

clean:
	rm *.o 

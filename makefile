
CC = gcc
CFLAG = -g -I../include
LFLAG = -L../lib -llua51

OBJ = stream.o buffer.o

all : $(OBJ)
	$(CC) -lmingw32 -shared -fPIC -Wl,--out-implib,stream.lib -o stream.dll $(OBJ) $(LFLAG)

buffer.o : buffer.c
	$(CC) -c buffer.c $(CFLAG)
	
stream.o : stream.c
	$(CC) -c stream.c $(CFLAG)

clean :
	rm -f $(OBJ) stream.dll
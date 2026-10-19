CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread
LDLIBS = -pthread
OBJ = servidor.o http.o arquivos.o

.PHONY: all clean test
all: servidor

servidor: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

servidor.o: servidor.c http.h
http.o: http.c http.h arquivos.h
arquivos.o: arquivos.c arquivos.h http.h

test: servidor
	python3 testes.py

clean:
	rm -f servidor $(OBJ)

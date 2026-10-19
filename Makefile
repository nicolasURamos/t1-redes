CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread
LDLIBS = -pthread
OBJ = servidor.o http.o arquivos.o qos.o painel.o

.PHONY: all clean test
all: servidor

servidor: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

servidor.o: servidor.c http.h painel.h qos.h
http.o: http.c http.h arquivos.h
arquivos.o: arquivos.c arquivos.h http.h qos.h
qos.o: qos.c qos.h
painel.o: painel.c painel.h qos.h

test: servidor
	python3 testes.py

clean:
	rm -f servidor $(OBJ)

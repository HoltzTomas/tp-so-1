CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c99 -I include
LDFLAGS = -lrt -lpthread

SRC_UTILS = src/utils/shmADT.c src/utils/game_sync.c
OBJ_UTILS = shmADT.o game_sync.o

all: player view

shmADT.o: src/utils/shmADT.c include/shmADT.h
	$(CC) $(CFLAGS) -c $< -o $@

game_sync.o: src/utils/game_sync.c include/game_sync.h include/constants.h
	$(CC) $(CFLAGS) -c $< -o $@

player: src/player.c $(OBJ_UTILS) include/game_state.h include/game_sync.h include/shmADT.h include/constants.h
	$(CC) $(CFLAGS) $< $(OBJ_UTILS) -o $@ $(LDFLAGS)

view: src/view.c $(OBJ_UTILS) include/game_state.h include/game_sync.h include/shmADT.h include/constants.h
	$(CC) $(CFLAGS) $< $(OBJ_UTILS) -o $@ $(LDFLAGS) -lncurses

clean:
	rm -f $(OBJ_UTILS) player view

.PHONY: all clean

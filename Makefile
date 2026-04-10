CC ?= gcc
CFLAGS ?= -Wall -Wextra -pedantic -std=c99 -I include
DEBUG_FLAGS ?= -g -O0
LDLIBS ?= -lrt -lpthread
DOCKER_IMAGE ?= tp-so-1-valgrind
VALGRIND ?= valgrind
VALGRIND_FLAGS ?= --leak-check=full --show-leak-kinds=all --track-origins=yes --trace-children=yes --child-silent-after-fork=yes --error-exitcode=1
ARGS ?= -w 10 -h 10 -d 10 -t 3 -p ./player ./player

SRC_UTILS = src/utils/shmADT.c src/utils/game_sync.c
OBJ_UTILS = shmADT.o game_sync.o

all: master player view

shmADT.o: src/utils/shmADT.c include/shmADT.h
	$(CC) $(CFLAGS) -c $< -o $@

game_sync.o: src/utils/game_sync.c include/game_sync.h include/constants.h
	$(CC) $(CFLAGS) -c $< -o $@

master: src/master.c $(OBJ_UTILS) include/game_state.h include/game_sync.h include/shmADT.h include/constants.h
	$(CC) $(CFLAGS) $< $(OBJ_UTILS) -o $@ $(LDLIBS) -lm

player: src/player.c $(OBJ_UTILS) include/game_state.h include/game_sync.h include/shmADT.h include/constants.h
	$(CC) $(CFLAGS) $< $(OBJ_UTILS) -o $@ $(LDLIBS)

view: src/view.c $(OBJ_UTILS) include/game_state.h include/game_sync.h include/shmADT.h include/constants.h
	$(CC) $(CFLAGS) $< $(OBJ_UTILS) -o $@ $(LDLIBS) -lncurses

debug: CFLAGS += $(DEBUG_FLAGS)
debug: clean all

valgrind: CFLAGS += $(DEBUG_FLAGS)
valgrind: clean all
	$(VALGRIND) $(VALGRIND_FLAGS) ./master $(ARGS)

docker-build:
	docker build -t $(DOCKER_IMAGE) .

docker-shell: docker-build
	docker run --rm -it -v "$(CURDIR):/work" -w /work $(DOCKER_IMAGE) bash

docker-valgrind: docker-build
	docker run --rm -v "$(CURDIR):/work" -w /work $(DOCKER_IMAGE) make valgrind ARGS='$(ARGS)'

clean:
	rm -f $(OBJ_UTILS) master player view

.PHONY: all clean debug valgrind docker-build docker-shell docker-valgrind

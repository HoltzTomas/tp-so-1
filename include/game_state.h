#ifndef GAME_STATE_H
#define GAME_STATE_H

#include <stdbool.h>
#include <sys/types.h>
#include "constants.h"

typedef struct
{
    char name[16];
    unsigned int score;
    unsigned int invalid_move_requests;
    unsigned int valid_move_requests;
    unsigned short x, y;
    pid_t pid;
    bool blocked;
} Player;

typedef struct
{
    unsigned short width;
    unsigned short height;
    unsigned char player_count;
    Player players[MAX_PLAYERS];
    bool finished;
    char board[];
} GameState;

#define GAME_STATE_MAP_SIZE(w, h) (sizeof(GameState) + (size_t)(w) * (size_t)(h))

#endif

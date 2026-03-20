#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <semaphore.h>
#include "game_state.h"
#include "game_sync.h"
#include "shmADT.h"

static bool find_player_index_by_pid(const GameState *state, GameSync *sync,
                                     pid_t pid, unsigned *out_index,
                                     bool *out_finished_now)
{
    game_sync_reader_enter(sync);
    unsigned player_count_snapshot = state->player_count;
    if (player_count_snapshot > MAX_PLAYERS)
        player_count_snapshot = MAX_PLAYERS;
    for (unsigned i = 0; i < player_count_snapshot; i++)
    {
        if (state->players[i].pid == pid)
        {
            bool finished_snapshot = state->finished;
            game_sync_reader_exit(sync);
            if (out_index)
                *out_index = i;
            if (out_finished_now)
                *out_finished_now = finished_snapshot;
            return true;
        }
    }
    bool finished_snapshot = state->finished;
    game_sync_reader_exit(sync);
    if (out_finished_now)
        *out_finished_now = finished_snapshot;
    return false;
}

typedef struct
{
    unsigned long width;
    unsigned long height;
} PlayerArgs;

typedef struct
{
    ShmADT state_shm;
    GameState *state;
    ShmADT sync_shm;
    GameSync *sync;
} PlayerResources;

//Funcion privada para parsear los argumentos
static bool parse_args(int argc, char *argv[], PlayerArgs *args);

static bool init_resources(const PlayerArgs *args, PlayerResources *out_res);

static void run_player_loop(GameState *state, GameSync *sync);

static void cleanup_resources(PlayerResources *res);

int main(int argc, char *argv[]) {

    //1. Recibe el width y height del tablero
    PlayerArgs args;
    if(!parse_args(argc, argv, &args))
        return 1;

    signal(SIGPIPE, SIG_IGN);//Ignoramos el SIGPIPE, ya que no queremos que el proceso se termine cuando el pipe se cierre.

    PlayerResources res;
    if(!init_resources(&args, &res))
        return 1;

    run_player_loop(res.state, res.sync);

    cleanup_resources(&res);
    return 0;
}


static bool parse_args(int argc, char *argv[], PlayerArgs *out_args) {

    if(argc != 3){
        errno = EINVAL;
        fprintf(stderr, "player: invalid usage. Usage: %s <width> <height>\n", argv[0]);
        return false;
    }

    out_args->width = strtoul(argv[1], NULL, 10);
    out_args->height = strtoul(argv[2], NULL, 10);
    if (out_args->width == 0 || out_args->height == 0)
    {
        errno = EINVAL;
        fprintf(stderr,
                "player: invalid dimensions: width=%lu height=%lu (must be > 0)\n",
                out_args->width, out_args->height);
        return false;
    }
    return true;
}

static bool init_resources(const PlayerArgs *args, PlayerResources *out_res) {
    size_t map_size = GAME_STATE_MAP_SIZE(args->width, args->height);

    out_res->state_shm = open_shm(GAME_STATE_SHM_NAME, map_size, O_RDONLY, 0600, PROT_READ);
    if (out_res->state_shm == NULL)
    {
        fprintf(stderr,
                "player: failed to open shm '%s' (read-only, size=%zu): %s\n",
                GAME_STATE_SHM_NAME, map_size, strerror(errno));
        return false;
    }

    out_res->state = (GameState *)get_shm_pointer(out_res->state_shm);
    out_res->sync_shm = open_shm(GAME_SYNC_SHM_NAME, sizeof(GameSync), O_RDWR, 0600, PROT_READ | PROT_WRITE);
    if (out_res->sync_shm == NULL)
    {
        fprintf(stderr,
                "player: failed to open shm '%s' (read/write, size=%zu): %s\n",
                GAME_SYNC_SHM_NAME, sizeof(GameSync), strerror(errno));
        close_shm(out_res->state_shm);
        return false;
    }
    out_res->sync = (GameSync *)get_shm_pointer(out_res->sync_shm);
    return true;
}

static void run_player_loop(GameState *state, GameSync *sync)
{
    pid_t mypid = getpid();
    unsigned me = 0;
    bool finished_now = false;
    //Tengo que primero encontrar que indice de player soy yo.
    bool found = find_player_index_by_pid(state, sync, mypid, &me, &finished_now);
    if (!found)
    {
        fprintf(stderr, "player: PID %d not registered in GameState\n", (int)mypid);
        return;
    }

    if (finished_now)
        return;

    static const int DX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    static const int DY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

    while (true) {
        if (sem_wait(&sync->player_can_move[me]) == -1)
        {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "player: error in sem_wait(player_can_move[%u]): %s\n", me, strerror(errno));
            break;
        }

        unsigned short width_snapshot = 0, height_snapshot = 0;
        unsigned short x_snapshot = 0, y_snapshot = 0;
        unsigned neighbor_ok_mask = 0;
        int neighbor_vals[8] = {0};

        game_sync_reader_enter(sync);
        finished_now = state->finished;

        if(!finished_now) {
            width_snapshot = state->width;
            height_snapshot = state->height;
            x_snapshot = state->players[me].x;
            y_snapshot = state->players[me].y;
            for (int d = 0; d < 8; d++)
            {
                int nx = (int)x_snapshot + DX[d];
                int ny = (int)y_snapshot + DY[d];
                if (nx >= 0 && ny >= 0 && nx < (int)width_snapshot && ny < (int)height_snapshot)
                {
                    neighbor_ok_mask |= (1u << d);
                    neighbor_vals[d] = state->board[ny * (int)width_snapshot + nx];
                }
            }
        }

        game_sync_reader_exit(sync);

        if (finished_now)
            break;

        int chosen_dir = -1;
        int bestv = -1;

        for (int d = 0; d < 8; d++)
        {
            if (((neighbor_ok_mask >> d) & 1u) == 0)
                continue;
            int v = neighbor_vals[d];
            if (v >= 1 && v <= 9 && v > bestv)
            {
                bestv = v;
                chosen_dir = d;
            }
        }

        if (chosen_dir < 0)
        {
            close(STDOUT_FILENO);
            break;
        }

        unsigned char dir = (unsigned char)chosen_dir;
        ssize_t w = write(STDOUT_FILENO, &dir, 1);
        if (w != 1)
        {
            close(STDOUT_FILENO);
            break;
        }
    }
}

static void cleanup_resources(PlayerResources *res)
{
    if (res && res->sync_shm)
        close_shm(res->sync_shm);
    if (res && res->state_shm)
        close_shm(res->state_shm);
}
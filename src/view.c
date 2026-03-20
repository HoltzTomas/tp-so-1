#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <ncurses.h>
#include <stdbool.h>

#include "constants.h"
#include "game_state.h"
#include "game_sync.h"
#include "shmADT.h"

static const short BASE_COLORS[] = {COLOR_RED, COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE};
static const int NUM_BASE_COLORS = (int)(sizeof(BASE_COLORS) / sizeof(BASE_COLORS[0]));

static volatile sig_atomic_t stop_requested = 0;
static int colors_ok = 0;

typedef struct
{
    unsigned long width;
    unsigned long height;
} ViewArgs;

typedef struct
{
    ShmADT state_shm;
    GameState *state;
    ShmADT sync_shm;
    GameSync *sync;
    int *owner_map;
    int *head_map;
} ViewResources;

static bool parse_args(int argc, char **argv, ViewArgs *out_args);

static bool init_resources(const ViewArgs *args, ViewResources *out_res);

static void init_ncurses(void);

static void run_view_loop(ViewResources *res);

static void cleanup_resources(ViewResources *res);

int main(int argc, char *argv[]) {

    ViewArgs args;
    if (!parse_args(argc, argv, &args))
        return 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    ViewResources res;
    if (!init_resources(&args, &res))
        return 1;

    init_ncurses();
    run_view_loop(&res);
    cleanup_resources(&res);
    return 0;
}

static bool parse_args(int argc, char **argv, ViewArgs *out_args) {

    if (argc != 3)
    {
        errno = EINVAL;
        fprintf(stderr, "view: invalid usage. Usage: %s <width> <height>\n", argv[0]);
        return false;
    }

    out_args->width = strtoul(argv[1], NULL, 10);
    out_args->height = strtoul(argv[2], NULL, 10);
    if (out_args->width == 0 || out_args->height == 0)
    {
        errno = EINVAL;
        fprintf(stderr,
                "view: invalid dimensions: width=%lu height=%lu (must be > 0)\n",
                out_args->width, out_args->height);
        return false;
    }
    return true;

}

static bool init_resources(const ViewArgs *args, ViewResources *out_res) {

    size_t map_size = GAME_STATE_MAP_SIZE(args->width, args->height);

    out_res->state_shm = open_shm(GAME_STATE_SHM_NAME, map_size, O_RDONLY, 0600, PROT_READ);
    if (out_res->state_shm == NULL)
    {
        fprintf(stderr,
                "view: failed to open shm '%s' (read-only, size=%zu): %s\n",
                GAME_STATE_SHM_NAME, map_size, strerror(errno));
        return false;
    }
    out_res->state = (GameState *)get_shm_pointer(out_res->state_shm);

    out_res->sync_shm = open_shm(GAME_SYNC_SHM_NAME, sizeof(GameSync), O_RDWR, 0600, PROT_READ | PROT_WRITE);
    if (out_res->sync_shm == NULL)
    {
        fprintf(stderr,
                "view: failed to open shm '%s' (read/write, size=%zu): %s\n",
                GAME_SYNC_SHM_NAME, sizeof(GameSync), strerror(errno));
        close_shm(out_res->state_shm);
        return false;
    }
    out_res->sync = (GameSync *)get_shm_pointer(out_res->sync_shm);

    size_t cells = (size_t)out_res->state->width * (size_t)out_res->state->height;
    out_res->owner_map = malloc(cells * sizeof(int));
    out_res->head_map = malloc(cells * sizeof(int));

    if (out_res->owner_map == NULL || out_res->head_map == NULL)
    {
        fprintf(stderr, "view: out of memory for maps\n");
        close_shm(out_res->sync_shm);
        close_shm(out_res->state_shm);
        free(out_res->owner_map);
        free(out_res->head_map);
        return false;
    }

    for (size_t i = 0; i < cells; ++i)
        out_res->owner_map[i] = -1;
    for (size_t i = 0; i < cells; ++i)
        out_res->head_map[i] = -1;

    return true;
}

static void init_ncurses(void) {
    if (!getenv("TERM"))
        setenv("TERM", "xterm-256color", 1);

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    
    if (has_colors())
    {
        start_color();
        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            short fg = BASE_COLORS[i % NUM_BASE_COLORS];
            init_pair((short)(i + 1), fg, COLOR_BLACK);
        }
        colors_ok = 1;
    }
}

static void run_view_loop(ViewResources *res) {
    GameState *state = res->state;
    GameSync *sync = res->sync;
    int *owner_map = res->owner_map;
    int *head_map = res->head_map;
    size_t cells = (size_t)state->width * (size_t)state->height;

    while (!stop_requested){
        if (sem_wait(&sync->view_update_ready) == -1)
        {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "view: error in sem_wait(view_update_ready): %s\n", strerror(errno));
            break;
        }

        bool finished = false;
        game_sync_reader_enter(sync);
        clear();
        attron(A_BOLD);
        mvprintw(0, 0, "==== JUEGO ====");
        attroff(A_BOLD);

        for (size_t i = 0; i < cells; ++i)
            head_map[i] = -1;

        for (unsigned int i = 0; i < state->player_count && i < MAX_PLAYERS; ++i)
        {
            unsigned int px = state->players[i].x;
            unsigned int py = state->players[i].y;
            if (px < state->width && py < state->height)
            {
                owner_map[py * state->width + px] = (int)i;
                head_map[py * state->width + px] = (int)i;
            }
        }

        print_board(state, owner_map, head_map);
        print_players(state);
        mvprintw((int)state->height + 3 + (int)state->player_count + 2, 0,
        "finished=%s", state->finished ? "true" : "false");
        refresh();
        finished = state->finished;
        game_sync_reader_exit(sync);

        if (sem_post(&sync->view_print_done) == -1)
        {
            fprintf(stderr, "view: error in sem_post(view_print_done): %s\n", strerror(errno));
            break;
        }

        if (finished)
            break;
    }
}

static void cleanup_resources(ViewResources *res)
{
    endwin();
    free(res->owner_map);
    free(res->head_map);
    if (res->sync_shm)
        close_shm(res->sync_shm);
    if (res->state_shm)
        close_shm(res->state_shm);
}
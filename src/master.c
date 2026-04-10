#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <time.h>
#include <sys/select.h>
#include <math.h>
#include "shmADT.h"
#include "game_state.h"
#include "game_sync.h"
#include "constants.h"

#define NUM_DIRECTIONS 8
#define COORD_BUF_LEN 16
#define BOARD_INDEX(state, X, Y) ((Y) * (state)->width + (X))

static const int DIR_DX[NUM_DIRECTIONS] = {0, 1, 1, 1, 0, -1, -1, -1};
static const int DIR_DY[NUM_DIRECTIONS] = {-1, -1, 0, 1, 1, 1, 0, -1};
static const double SPAWN_RADIUS_DIVISOR = 3.0;

static volatile sig_atomic_t stop_requested = 0;

static void handle_sigint_master(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

typedef struct
{
    unsigned int width;
    unsigned int height;
    unsigned int delay;
    unsigned int timeout;
    unsigned int seed;
    char *view_path;
    char *player_paths[MAX_PLAYERS];
    int player_count;
} MasterArgs;

typedef struct
{
    ShmADT state_shm;
    GameState *state;
    ShmADT sync_shm;
    GameSync *sync;
    pid_t *player_pids;
    pid_t view_pid;
    int *player_pipes;
    int *player_statuses;
    int view_status;
} GameResources;

static inline void notify_view(const MasterArgs *args, GameResources *res)
{
    if (!args->view_path)
        return;
    sem_post(&res->sync->view_update_ready);
    if (stop_requested)
        return;
    sem_wait(&res->sync->view_print_done);
    if (!stop_requested)
    {
        struct timespec delay = {.tv_sec = args->delay / 1000, .tv_nsec = (args->delay % 1000) * 1000000L};
        nanosleep(&delay, NULL);
    }
}

static inline void lock_writer(GameResources *res)
{
    sem_wait(&res->sync->master_starvation_guard);
    sem_wait(&res->sync->state_mutex);
    sem_post(&res->sync->master_starvation_guard);
}

static inline void unlock_writer(GameResources *res)
{
    sem_post(&res->sync->state_mutex);
}

static inline void finish_game_and_notify(const MasterArgs *args, GameResources *res)
{
    lock_writer(res);
    res->state->finished = true;
    unlock_writer(res);
    notify_view(args, res);
}

static void request_graceful_shutdown(const MasterArgs *args, GameResources *res)
{
    finish_game_and_notify(args, res);

    for (int i = 0; i < args->player_count; i++)
        sem_post(&res->sync->player_can_move[i]);

    if (res->player_pipes)
    {
        for (int i = 0; i < args->player_count; i++)
        {
            if (res->player_pipes[i] >= 0)
            {
                close(res->player_pipes[i]);
                res->player_pipes[i] = -1;
            }
        }
    }
}

static inline void set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags != -1)
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static inline long long monotonic_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

static bool any_player_can_move(const GameState *state)
{
    for (unsigned int i = 0; i < state->player_count; i++)
    {
        const Player *p = &state->players[i];
        if (p->blocked)
            continue;
        for (int m = 0; m < NUM_DIRECTIONS; m++)
        {
            int nx = (int)p->x + DIR_DX[m];
            int ny = (int)p->y + DIR_DY[m];
            if (nx >= 0 && nx < (int)state->width && ny >= 0 && ny < (int)state->height)
            {
                if (state->board[BOARD_INDEX(state, nx, ny)] > 0)
                    return true;
            }
        }
    }
    return false;
}

static void close_player_start_wfds(int *player_start_wfds, int player_count)
{
    if (!player_start_wfds)
        return;

    for (int i = 0; i < player_count; i++)
    {
        if (player_start_wfds[i] >= 0)
        {
            close(player_start_wfds[i]);
            player_start_wfds[i] = -1;
        }
    }
}

static void release_players_for_exec(int *player_start_wfds, int player_count)
{
    if (!player_start_wfds)
        return;

    for (int i = 0; i < player_count; i++)
    {
        if (player_start_wfds[i] < 0)
            continue;

        char token = 1;
        ssize_t bytes_written;
        do
        {
            bytes_written = write(player_start_wfds[i], &token, sizeof(token));
        } while (bytes_written == -1 && errno == EINTR);

        if (bytes_written != (ssize_t)sizeof(token))
            perror("write to player start pipe failed");

        close(player_start_wfds[i]);
        player_start_wfds[i] = -1;
    }
}

static bool launch_player(const MasterArgs *args, GameResources *res, int player_index,
                          const char *width_str, const char *height_str,
                          int *out_start_wfd)
{
    int pipe_fds[2] = {-1, -1};
    int start_pipe_fds[2] = {-1, -1};

    if (pipe(pipe_fds) == -1)
    {
        perror("pipe creation failed");
        return false;
    }

    for (int i = 0; i < 2; i++)
        set_cloexec(pipe_fds[i]);

    if (pipe(start_pipe_fds) == -1)
    {
        perror("player start pipe creation failed");
        close(pipe_fds[R_END]);
        close(pipe_fds[W_END]);
        return false;
    }

    for (int i = 0; i < 2; i++)
        set_cloexec(start_pipe_fds[i]);

    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork failed for player");
        close(pipe_fds[R_END]);
        close(pipe_fds[W_END]);
        close(start_pipe_fds[R_END]);
        close(start_pipe_fds[W_END]);
        return false;
    }

    if (pid == 0)
    {
        close(pipe_fds[R_END]);
        close(start_pipe_fds[W_END]);

        char token;
        ssize_t bytes_read;
        do
        {
            bytes_read = read(start_pipe_fds[R_END], &token, sizeof(token));
        } while (bytes_read == -1 && errno == EINTR);

        close(start_pipe_fds[R_END]);

        if (bytes_read != (ssize_t)sizeof(token))
            exit(EXIT_FAILURE);

        if (dup2(pipe_fds[W_END], STDOUT_FILENO) == -1)
        {
            perror("dup2 failed for player");
            exit(EXIT_FAILURE);
        }
        close(pipe_fds[W_END]);

        char *argv[] = {args->player_paths[player_index], (char *)width_str, (char *)height_str, NULL};
        execv(args->player_paths[player_index], argv);
        perror("execv player failed");
        exit(EXIT_FAILURE);
    }

    close(pipe_fds[W_END]);
    close(start_pipe_fds[R_END]);
    res->player_pipes[player_index] = pipe_fds[R_END];
    res->player_pids[player_index] = pid;
    if (out_start_wfd)
        *out_start_wfd = start_pipe_fds[W_END];
    else
        close(start_pipe_fds[W_END]);
    return true;
}

static bool launch_view(const MasterArgs *args, GameResources *res, const char *width_str, const char *height_str)
{
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork failed for view");
        return false;
    }
    if (pid == 0)
    {
        char *argv[] = {args->view_path, (char *)width_str, (char *)height_str, NULL};
        execv(args->view_path, argv);
        perror("execv view failed");
        exit(EXIT_FAILURE);
    }
    res->view_pid = pid;
    return true;
}

static bool launch_children(const MasterArgs *args, GameResources *res, int *player_start_wfds)
{
    char width_str[COORD_BUF_LEN];
    char height_str[COORD_BUF_LEN];
    snprintf(width_str, sizeof(width_str), "%u", args->width);
    snprintf(height_str, sizeof(height_str), "%u", args->height);

    for (int i = 0; i < args->player_count; i++)
    {
        if (!launch_player(args, res, i, width_str, height_str, &player_start_wfds[i]))
            return false;
    }

    if (args->view_path)
    {
        if (!launch_view(args, res, width_str, height_str))
            return false;
    }

    return true;
}

static void init_game_state(const MasterArgs *args, GameResources *res)
{
    srand(args->seed);

    GameState *state = res->state;
    state->width = (unsigned short)args->width;
    state->height = (unsigned short)args->height;
    state->player_count = (unsigned char)args->player_count;
    state->finished = false;

    for (unsigned int i = 0; i < state->width * state->height; i++)
        state->board[i] = (signed char)(1 + (rand() % 9));

    for (int i = 0; i < args->player_count; i++)
    {
        Player *p = &state->players[i];
        p->pid = res->player_pids[i];
        p->score = 0;
        p->valid_move_requests = 0;
        p->invalid_move_requests = 0;
        p->blocked = false;
        snprintf(p->name, sizeof(p->name), "Player %d", i);

        double radius_x = ((double)state->width) / SPAWN_RADIUS_DIVISOR;
        double radius_y = ((double)state->height) / SPAWN_RADIUS_DIVISOR;
        if (radius_x < 1.0)
            radius_x = 1.0;
        if (radius_y < 1.0)
            radius_y = 1.0;
        int center_x = (int)state->width / 2;
        int center_y = (int)state->height / 2;

        double theta = (2.0 * M_PI * (double)i) / (double)state->player_count;
        int tx = center_x + (int)lround(radius_x * cos(theta));
        int ty = center_y + (int)lround(radius_y * sin(theta));
        tx = clampi(tx, 0, (int)state->width - 1);
        ty = clampi(ty, 0, (int)state->height - 1);

        p->x = (unsigned short)tx;
        p->y = (unsigned short)ty;
        state->board[BOARD_INDEX(state, p->x, p->y)] = (signed char)(-(i));
    }
}

static void process_player_move(int player_idx, int pipe_fd, const MasterArgs *args, GameResources *res)
{
    unsigned char move;
    ssize_t bytes_read = read(pipe_fd, &move, sizeof(move));

    if (bytes_read <= 0)
    {
        if (bytes_read != 0)
            perror("read from pipe failed");

        sem_wait(&res->sync->state_mutex);
        res->state->players[player_idx].blocked = true;
        sem_post(&res->sync->state_mutex);

        close(pipe_fd);
        res->player_pipes[player_idx] = -1;

        notify_view(args, res);
        return;
    }

    sem_wait(&res->sync->master_starvation_guard);
    sem_wait(&res->sync->state_mutex);
    sem_post(&res->sync->master_starvation_guard);

    GameState *state = res->state;
    Player *player = &state->players[player_idx];
    bool is_valid = false;

    if (move < NUM_DIRECTIONS)
    {
        int nx = player->x + DIR_DX[move];
        int ny = player->y + DIR_DY[move];

        if (nx >= 0 && nx < state->width && ny >= 0 && ny < state->height &&
            state->board[BOARD_INDEX(state, nx, ny)] > 0)
        {
            is_valid = true;
            int reward = state->board[BOARD_INDEX(state, nx, ny)];
            player->score += reward;
            player->x = (unsigned short)nx;
            player->y = (unsigned short)ny;
            state->board[BOARD_INDEX(state, nx, ny)] = (signed char)(-(player_idx));
            player->valid_move_requests++;
        }
    }

    if (!is_valid)
        player->invalid_move_requests++;

    unlock_writer(res);

    sem_post(&res->sync->player_can_move[player_idx]);

    notify_view(args, res);
}

static void cleanup_game_resources(GameResources *res, int player_count)
{
    if (res->sync)
    {
        sem_destroy(&res->sync->view_update_ready);
        sem_destroy(&res->sync->view_print_done);
        sem_destroy(&res->sync->master_starvation_guard);
        sem_destroy(&res->sync->state_mutex);
        sem_destroy(&res->sync->readers_count_mutex);
        for (int i = 0; i < player_count && i < MAX_PLAYERS; i++)
            sem_destroy(&res->sync->player_can_move[i]);
    }

    if (res->player_pipes)
    {
        for (int i = 0; i < player_count; i++)
        {
            if (res->player_pipes[i] >= 0)
            {
                close(res->player_pipes[i]);
                res->player_pipes[i] = -1;
            }
        }
        free(res->player_pipes);
    }
    if (res->player_pids)
        free(res->player_pids);
    if (res->player_statuses)
        free(res->player_statuses);
    if (res->state_shm)
        destroy_shm(res->state_shm);
    if (res->sync_shm)
        destroy_shm(res->sync_shm);
}

static void print_finish_status(const MasterArgs *args, GameResources *res)
{
    if (res->view_pid > 0)
    {
        if (WIFEXITED(res->view_status))
            printf("View exited (%d)\n", WEXITSTATUS(res->view_status));
        else if (WIFSIGNALED(res->view_status))
            printf("View terminated by signal %d\n", WTERMSIG(res->view_status));
    }

    for (int i = 0; i < args->player_count; i++)
    {
        if (res->player_pids[i] > 0)
        {
            if (WIFEXITED(res->player_statuses[i]))
            {
                printf("Player %s (%d) exited (%d) with a score of %d / %d / %d\n",
                       res->state->players[i].name, i,
                       WEXITSTATUS(res->player_statuses[i]),
                       res->state->players[i].score,
                       res->state->players[i].valid_move_requests,
                       res->state->players[i].invalid_move_requests);
            }
            else if (WIFSIGNALED(res->player_statuses[i]))
            {
                printf("Player %s (%d) terminated by signal %d with a score of %d / %d / %d\n",
                       res->state->players[i].name, i,
                       WTERMSIG(res->player_statuses[i]),
                       res->state->players[i].score,
                       res->state->players[i].valid_move_requests,
                       res->state->players[i].invalid_move_requests);
            }
        }
    }
}

static void print_usage(const char *exec_name)
{
    fprintf(stderr, "Usage: %s [-w width] [-h height] [-d delay] [-t timeout] [-s seed] [-v view_path] -p player1 [player2 ...]\n", exec_name);
}

static bool parse_args(int argc, char **argv, MasterArgs *args)
{
    args->width = DEFAULT_WIDTH;
    args->height = DEFAULT_HEIGHT;
    args->delay = DEFAULT_DELAY;
    args->timeout = DEFAULT_TIMEOUT;
    args->seed = (unsigned int)time(NULL);
    args->view_path = NULL;
    args->player_count = 0;

    int opt;
    bool players_set = false;
    while ((opt = getopt(argc, argv, "w:h:d:t:s:v:p:i")) != -1)
    {
        switch (opt)
        {
        case 'w':
            args->width = (unsigned int)atoi(optarg);
            break;
        case 'h':
            args->height = (unsigned int)atoi(optarg);
            break;
        case 'd':
            args->delay = (unsigned int)atoi(optarg);
            break;
        case 't':
            args->timeout = (unsigned int)atoi(optarg);
            break;
        case 's':
            args->seed = (unsigned int)atoi(optarg);
            break;
        case 'v':
            args->view_path = optarg;
            break;
        case 'i':
            break;
        case 'p':
            if (!players_set)
            {
                players_set = true;
                if (args->player_count == MAX_PLAYERS)
                {
                    fprintf(stderr, "Error: Maximum number of players is %d.\n", MAX_PLAYERS);
                    return false;
                }
                args->player_paths[args->player_count++] = optarg;

                while (optind < argc && argv[optind][0] != '-')
                {
                    if (args->player_count == MAX_PLAYERS)
                    {
                        fprintf(stderr, "Error: Maximum number of players is %d.\n", MAX_PLAYERS);
                        return false;
                    }
                    args->player_paths[args->player_count++] = argv[optind++];
                }
            }
            else
            {
                while (optind < argc && argv[optind][0] != '-')
                    optind++;
            }
            break;
        default:
            print_usage(argv[0]);
            return false;
        }
    }

    if (args->player_count == 0)
    {
        fprintf(stderr, "Error: At least one player must be specified with -p.\n");
        print_usage(argv[0]);
        return false;
    }

    if (args->width < MIN_WIDTH || args->height < MIN_HEIGHT)
    {
        fprintf(stderr, "Error: Minimum width and height are %d and %d.\n", MIN_WIDTH, MIN_HEIGHT);
        return false;
    }

    return true;
}

static bool init_game_resources(const MasterArgs *args, GameResources *res)
{
    res->sync_shm = create_shm(GAME_SYNC_SHM_NAME, sizeof(GameSync), O_RDWR | O_CREAT | O_EXCL, 0666, PROT_READ | PROT_WRITE);
    if (res->sync_shm == NULL)
    {
        perror("create_shm GameSync failed");
        return false;
    }
    res->sync = get_shm_pointer(res->sync_shm);

    sem_init(&res->sync->view_update_ready, 1, 0);
    sem_init(&res->sync->view_print_done, 1, 0);
    sem_init(&res->sync->master_starvation_guard, 1, 1);
    sem_init(&res->sync->state_mutex, 1, 1);
    sem_init(&res->sync->readers_count_mutex, 1, 1);
    res->sync->readers_count = 0;
    for (int i = 0; i < args->player_count; i++)
        sem_init(&res->sync->player_can_move[i], 1, 1);

    size_t state_size = GAME_STATE_MAP_SIZE(args->width, args->height);
    res->state_shm = create_shm(GAME_STATE_SHM_NAME, state_size, O_RDWR | O_CREAT | O_EXCL, 0666, PROT_READ | PROT_WRITE);
    if (res->state_shm == NULL)
    {
        perror("create_shm GameState failed");
        destroy_shm(res->sync_shm);
        return false;
    }
    res->state = get_shm_pointer(res->state_shm);

    return true;
}

static bool init_resources(const MasterArgs *args, GameResources *res)
{
    *res = (GameResources){0};

    res->player_pipes = calloc((size_t)args->player_count, sizeof(int));
    res->player_pids = calloc((size_t)args->player_count, sizeof(pid_t));
    res->player_statuses = calloc((size_t)args->player_count, sizeof(int));
    if (!res->player_pipes || !res->player_pids || !res->player_statuses)
    {
        perror("allocating memory for child resources failed");
        cleanup_game_resources(res, args->player_count);
        return false;
    }

    for (int i = 0; i < args->player_count; i++)
        res->player_pipes[i] = -1;

    if (!init_game_resources(args, res))
    {
        fprintf(stderr, "Error: Game resources could not be initialized.\n");
        cleanup_game_resources(res, args->player_count);
        return false;
    }

    return true;
}

static void print_config(const MasterArgs *args)
{
    printf("width: %u\n", args->width);
    printf("height: %u\n", args->height);
    printf("delay: %u\n", args->delay);
    printf("timeout: %u\n", args->timeout);
    printf("seed: %u\n", args->seed);
    printf("view: %s\n", args->view_path ? args->view_path : "");
    printf("num_players: %d\n", args->player_count);
    for (int i = 0; i < args->player_count; i++)
        printf("  %s\n", args->player_paths[i]);
}

static void init_game(const MasterArgs *args, GameResources *resources, int *player_start_wfds)
{
    init_game_state(args, resources);
    notify_view(args, resources);
    release_players_for_exec(player_start_wfds, args->player_count);

    int current_player_turn = 0;
    fd_set read_fds;
    int max_fd = 0;
    long long last_valid_move_ms = monotonic_millis();

    while (!resources->state->finished)
    {
        if (stop_requested)
        {
            request_graceful_shutdown(args, resources);
            break;
        }

        FD_ZERO(&read_fds);
        max_fd = 0;
        int active_players = 0;
        for (int i = 0; i < args->player_count; i++)
        {
            if (!resources->state->players[i].blocked && resources->player_pipes[i] != -1)
            {
                FD_SET(resources->player_pipes[i], &read_fds);
                if (resources->player_pipes[i] > max_fd)
                    max_fd = resources->player_pipes[i];
                active_players++;
            }
        }

        if (active_players == 0)
        {
            finish_game_and_notify(args, resources);
            break;
        }

        long long now_ms = monotonic_millis();
        long long elapsed_ms = now_ms - last_valid_move_ms;
        long long remaining_ms = (long long)args->timeout * 1000LL - elapsed_ms;
        if (remaining_ms <= 0)
        {
            finish_game_and_notify(args, resources);
            break;
        }

        struct timeval timeout;
        timeout.tv_sec = (time_t)(remaining_ms / 1000LL);
        timeout.tv_usec = (suseconds_t)((remaining_ms % 1000LL) * 1000LL);

        int ready_fds = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (ready_fds == -1)
        {
            if (errno == EINTR || stop_requested)
                request_graceful_shutdown(args, resources);
            else
                perror("select failed");
            break;
        }

        if (ready_fds == 0)
        {
            finish_game_and_notify(args, resources);
            break;
        }

        for (int i = 0; i < args->player_count; i++)
        {
            int player_idx = (current_player_turn + i) % args->player_count;
            int player_pipe = resources->player_pipes[player_idx];

            if (player_pipe >= 0 && FD_ISSET(player_pipe, &read_fds))
            {
                unsigned int prev_valid = resources->state->players[player_idx].valid_move_requests;
                process_player_move(player_idx, player_pipe, args, resources);

                if (resources->state->players[player_idx].valid_move_requests > prev_valid)
                    last_valid_move_ms = monotonic_millis();

                int remaining_active = 0;
                for (int p = 0; p < args->player_count; p++)
                {
                    if (!resources->state->players[p].blocked && resources->player_pipes[p] != -1)
                        remaining_active++;
                }

                if (remaining_active == 0)
                {
                    finish_game_and_notify(args, resources);
                    break;
                }

                if (!any_player_can_move(resources->state))
                {
                    finish_game_and_notify(args, resources);
                    break;
                }

                current_player_turn = (player_idx + 1) % args->player_count;
                break;
            }
        }
    }

    if (resources->view_pid > 0)
    {
        int status;
        waitpid(resources->view_pid, &status, 0);
        resources->view_status = status;
    }
    for (int i = 0; i < args->player_count; i++)
    {
        if (resources->player_pids[i] > 0)
        {
            int status;
            waitpid(resources->player_pids[i], &status, 0);
            resources->player_statuses[i] = status;
        }
    }
}

int main(int argc, char **argv)
{
    MasterArgs args;
    if (!parse_args(argc, argv, &args))
        return EXIT_FAILURE;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint_master;
    sigaction(SIGINT, &sa, NULL);

    print_config(&args);

    GameResources resources;
    int player_start_wfds[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++)
        player_start_wfds[i] = -1;

    if (!init_resources(&args, &resources))
        return EXIT_FAILURE;

    if (!launch_children(&args, &resources, player_start_wfds))
    {
        fprintf(stderr, "Error: Child processes could not be launched.\n");
        close_player_start_wfds(player_start_wfds, args.player_count);
        cleanup_game_resources(&resources, args.player_count);
        return EXIT_FAILURE;
    }

    init_game(&args, &resources, player_start_wfds);
    close_player_start_wfds(player_start_wfds, args.player_count);

    print_finish_status(&args, &resources);

    cleanup_game_resources(&resources, args.player_count);
    return 0;
}

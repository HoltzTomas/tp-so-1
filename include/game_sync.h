#ifndef GAME_SYNC_H
#define GAME_SYNC_H

#include <semaphore.h>
#include "constants.h"

typedef struct
{
    sem_t view_update_ready; //El master le indica a la vista que hay cambios a mostrar
    sem_t view_print_done; //La vista indica que ya mostro los cambios
    sem_t master_starvation_guard; //Esto es para evitar que el master se quede esperando para siempre
    sem_t state_mutex; //Protege el GameState, queda en cero (habilitado para el writer) cuando no hay nadie leyendo
    sem_t readers_count_mutex; // Hace que no modifiquen el readers_count al mismo tiempo dos procesos
    unsigned int readers_count; // Cuenta el numero de lectores
    sem_t player_can_move[MAX_PLAYERS]; // Semaforo para cada jugador, indica si puede moverse o no
} GameSync;

void game_sync_reader_enter(GameSync *sync);
void game_sync_reader_exit(GameSync *sync);

#endif

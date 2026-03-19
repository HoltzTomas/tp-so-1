#include "game_sync.h"

void game_sync_reader_enter(GameSync *s)
{
    sem_wait(&s->master_starvation_guard); //Espera a que el master indique que puede leer
    sem_post(&s->master_starvation_guard); //Si puedo, volver a dejar el semaforo en el estado inicial.

    sem_wait(&s->readers_count_mutex); //Evitamos race condition sobre readers count
    s->readers_count++;
    if (s->readers_count == 1) //Si es el primero, bloqueamos state mutex para que el master no pueda escribir
        sem_wait(&s->state_mutex);
    sem_post(&s->readers_count_mutex); //Desbloqueamos readers count mutex
}

void game_sync_reader_exit(GameSync *s)
{
    sem_wait(&s->readers_count_mutex); //Evitamos race condition sobre readers count
    s->readers_count--;
    if (s->readers_count == 0) //Si es el ultimo, desbloqueamos state mutex para que el master pueda escribir
        sem_post(&s->state_mutex);
    sem_post(&s->readers_count_mutex); //Desbloqueamos readers count mutex
}

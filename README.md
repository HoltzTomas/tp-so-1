# TP 1 - ChompChamps

Implementacion en C de `master`, `player` y `view` para el TP de IPC de Sistemas Operativos.

## Decisiones de diseno

El `master` crea primero todos los procesos `player` con `fork()`, pero cada hijo queda bloqueado temporalmente en la lectura de un pipe privado antes de ejecutar `execv()`.

Esto se hace para que el `master` pueda obtener los `pid` reales de cada jugador y completar `GameState` antes de que el binario `player` arranque y consulte la memoria compartida.

Una vez que el estado inicial ya fue cargado, el `master` libera a cada hijo escribiendo un byte en ese pipe. Recien ahi el proceso hijo hace `execv()` y comienza la logica normal del jugador.

Esta decision no modifica el protocolo publico del TP: no cambia argumentos, semaforos, estructuras compartidas ni la forma en que los jugadores envian movimientos.

## Problemas encontrados y solucion

Detectamos una condicion de carrera en el arranque de los `player`.

Antes de este cambio, el `master` hacia `fork()` y el hijo ejecutaba `execv()` enseguida. Eso permitia que un `player` abriera la memoria compartida y ejecutara `find_player_index_by_pid()` antes de que el `master` hubiera terminado de guardar su `pid` en `state->players[]`.

En ese escenario, el jugador podia no encontrar su entrada en `GameState` simplemente porque el estado todavia no estaba completamente inicializado.

La solucion fue agregar una barrera de arranque pre-`execv()` usando un pipe anonimo privado por jugador:

- el hijo espera bloqueado en un `read()`
- el padre inicializa `GameState` con los `pid`
- el padre libera al hijo
- el hijo hace `execv()` con el estado ya consistente

Con esto, cuando el binario `player` empieza a correr, `GameState` ya contiene los `pid`, las posiciones iniciales y el tablero cargado.

## Compilacion y ejecucion

Compilacion:

```sh
make
```

Ejemplo de ejecucion:

```sh
./master -v ./view -p ./player ./player
```

## Rutas relativas para torneo

- Vista: `./view`
- Jugador: `./player`

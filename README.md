# TP 1 - ChompChamps

Implementacion en C de `master`, `player` y `view` para el TP de IPC de Sistemas Operativos.

## Arquitectura general

El proyecto esta compuesto por tres binarios:

- `master`: crea los recursos compartidos, lanza los procesos, inicializa el tablero, arbitra los movimientos y decide cuando termina la partida.
- `player`: lee el estado del juego de forma sincronizada, elige un movimiento automaticamente y lo envia al `master`.
- `view`: espera notificaciones del `master`, lee el estado de forma sincronizada y renderiza el tablero en la terminal usando `ncurses`.

La comunicacion entre procesos se resuelve con tres mecanismos:

- `GameState` en la memoria compartida `/game_state`: tablero, dimensiones, jugadores, puntajes, posiciones y flag de finalizacion.
- `GameSync` en la memoria compartida `/game_sync`: semaforos y contador de lectores para coordinar acceso concurrente al estado.
- un pipe anonimo por jugador: cada `player` escribe un byte con la direccion elegida y el `master` lo consume.

## Flujo del juego

El flujo general de una partida es el siguiente:

1. `master` parsea argumentos y crea ambas memorias compartidas.
2. `master` inicializa los semaforos de `GameSync`.
3. `master` crea todos los procesos `player` con `fork()`.
4. Cada hijo queda bloqueado en un `read()` sobre un pipe privado antes de ejecutar `execv()`.
5. `master` lanza la `view`, inicializa completamente `GameState` y guarda los `pid` reales de los jugadores.
6. `master` notifica el estado inicial a la `view`.
7. `master` libera a los `player` para que hagan `execv()` y empiecen su logica normal.
8. Cada `player` abre la memoria compartida, busca su entrada en `state->players[]` comparando su `pid` y espera su permiso de movimiento.
9. Cuando un `player` puede moverse, entra como lector, toma un snapshot consistente de su posicion y de sus vecinos, sale del lock de lectura y decide la direccion.
10. El `player` escribe un byte por `stdout`, que en realidad esta redirigido al pipe que lee el `master`.
11. `master` usa `select()` para esperar solicitudes sobre todos los pipes activos, procesa una, actualiza el tablero como escritor exclusivo, vuelve a habilitar a ese jugador y notifica a la `view`.
12. `view` espera `view_update_ready`, entra como lectora, imprime el tablero y la informacion de jugadores, luego hace `sem_post(view_print_done)` para que el `master` continue.
13. El ciclo termina cuando no quedan movimientos posibles, todos los jugadores quedaron bloqueados, se alcanza el timeout o se recibe `SIGINT`.

## Sincronizacion

El acceso a `GameState` se protege con un esquema de lectores-escritores:

- `state_mutex`: garantiza acceso exclusivo al estado cuando escribe el `master`.
- `readers_count_mutex`: protege el contador `readers_count`.
- `master_starvation_guard`: evita que entren lectores nuevos indefinidamente mientras el `master` intenta escribir.

Ademas, se usan semaforos para coordinar el flujo del juego:

- `player_can_move[i]`: indica cuando el jugador `i` puede enviar una nueva solicitud de movimiento. Evita que un mismo jugador acumule multiples movimientos pendientes.
- `view_update_ready`: el `master` lo incrementa cuando hay un frame nuevo para mostrar.
- `view_print_done`: la `view` lo incrementa cuando ya termino de imprimir y el `master` puede continuar.

Con este esquema:

- `master` es el unico proceso que modifica el estado.
- `player` y `view` siempre leen snapshots consistentes.
- no hay espera activa para la sincronizacion normal del juego.

## Decisiones de diseno

El `master` concentra toda la logica de validacion y actualizacion del juego. Esto evita conflictos entre jugadores y deja a los `player` como procesos simples que solo observan el estado y proponen una direccion.

Los movimientos se envian por pipes en lugar de escribirse directamente en memoria compartida. Eso simplifica la arbitraje: el `master` recibe solicitudes independientes, las valida contra el estado actual y mantiene una unica fuente de verdad.

La atencion de movimientos se hace con `select()` y una politica round-robin sobre los pipes listos. Asi se evita bloquearse en un solo jugador y se reparte mejor la prioridad entre solicitudes simultaneas.

La `view` se actualiza a demanda. No hace polling del estado, sino que espera a que el `master` le indique explicitamente cuando debe volver a leer e imprimir.

El `player` implementado usa una estrategia greedy simple: inspecciona las 8 celdas adyacentes validas, elige la de mayor recompensa y, si no encuentra ninguna celda libre con valor positivo, cierra su pipe y queda bloqueado para el resto de la partida.

En esta branch agregamos una barrera de arranque pre-`execv()` para los `player`. El motivo es que cada jugador identifica su indice buscando su propio `pid` dentro de `state->players[]`. Sin esa barrera, un `player` podia ejecutar `find_player_index_by_pid()` antes de que el `master` hubiera terminado de inicializar `GameState`.

## Problemas encontrados y solucion

El principal problema encontrado durante el desarrollo fue una condicion de carrera en el arranque de los `player`.

Antes de corregirla, el `master` hacia `fork()` y el hijo ejecutaba `execv()` enseguida. Eso hacia posible que un `player` arrancara, abriera la memoria compartida y buscara su `pid` antes de que el `master` hubiera escrito todos los `pid` dentro de `state->players[]`.

La solucion implementada fue agregar un pipe anonimo privado por jugador que se usa solo como barrera de arranque:

- el hijo queda bloqueado en un `read()`
- el padre inicializa `GameState`
- el padre libera al hijo escribiendo un byte
- el hijo recien ahi ejecuta `execv()`

Con esto, cuando el binario `player` empieza a correr, el estado ya contiene las dimensiones, el tablero, los jugadores y sus `pid`.

Otro punto observado durante las pruebas es que la `view` depende de `ncurses`. En algunos entornos eso requiere instalar los headers y la biblioteca antes de compilar. El codigo de la vista no cambia por eso, pero la dependencia debe estar disponible en el entorno donde se ejecute `make`.

## Limitaciones

La `view` depende de `ncurses`, por lo que su compilacion y ejecucion requieren un entorno con esa biblioteca correctamente instalada.

La inicializacion de la `view` todavia puede endurecerse mas: hoy el `master` la lanza antes de inicializar por completo `GameState`, y la vista usa datos del estado muy temprano en su arranque. En la practica el sistema funciona, pero ese punto queda como mejora pendiente.

En las corridas con Valgrind, `master` y `player` no reportan errores ni leaks propios. En el caso de `view`, pueden aparecer bloques `still reachable` o `possibly lost` asociados a inicializacion interna de `ncurses`. No detectamos perdidas definitivas (`definitely lost`) originadas en memoria administrada por codigo propio.

La politica del jugador es deliberadamente simple. Cumple con el protocolo del TP y permite jugar partidas completas, pero no intenta optimizar estrategia global ni predecir movimientos de otros jugadores.

## Compilacion y ejecucion

El enunciado exige desarrollar, compilar y ejecutar con la imagen oficial de la catedra:

```sh
docker pull agodio/itba-so-multiarch:3.1
docker run --rm -it -v "$PWD:/work" -w /work agodio/itba-so-multiarch:3.1 bash
```

Dentro del contenedor, la compilacion se realiza con:

```sh
make
```

Si la imagen o el entorno local no incluyen los headers de `ncurses`, la compilacion de `view` requiere instalar previamente la dependencia correspondiente.

Ejemplos de ejecucion:

```sh
./master -p ./player
```

```sh
./master -v ./view -p ./player ./player
```

```sh
./master -w 20 -h 12 -d 100 -t 10 -v ./view -p ./player ./player ./player
```

## Rutas relativas para torneo

- Vista: `./view`
- Jugador: `./player`

## Citas de fragmentos de codigo / uso de IA

No se incorporaron fragmentos de codigo de terceros de forma textual dentro del proyecto. Se utilizo IA (Claude) como herramienta de apoyo para analisis de codigo y debug durante el desarrollo.

// main.c — servidor del shooter 3D (autoritativo sobre balas, confía en posiciones)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "protocol.h"

// -------- helpers de red --------

static int recv_all(int fd, void *buf, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(fd, (char*)buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static int send_state(int fds[2], const GameState *gs) {
    MsgHeader hdr = { .type = MSG_STATE, .payload_len = sizeof(GameState) };
    for (int i = 0; i < 2; i++) {
        if (send(fds[i], &hdr, sizeof(hdr), 0) < 0) return -1;
        if (send(fds[i], gs,   sizeof(*gs),  0) < 0) return -1;
    }
    return 0;
}

// -------- lógica del juego --------

static void spawn_bullet(GameState *gs, int owner,
                         float ox, float oy, float oz,
                         float dx, float dy, float dz)
{
    // Normalizar dirección por las dudas
    float len = sqrtf(dx*dx + dy*dy + dz*dz);
    if (len < 1e-5f) return;
    dx /= len; dy /= len; dz /= len;

    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!gs->bullets[i].active) {
            gs->bullets[i].x  = ox;
            gs->bullets[i].y  = oy;
            gs->bullets[i].z  = oz;
            gs->bullets[i].dx = dx * BULLET_SPEED;
            gs->bullets[i].dy = dy * BULLET_SPEED;
            gs->bullets[i].dz = dz * BULLET_SPEED;
            gs->bullets[i].owner  = owner;
            gs->bullets[i].active = 1;
            return;
        }
    }
}

static void update_game(GameState *gs) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        BulletState *b = &gs->bullets[i];
        if (!b->active) continue;

        // Avanzar bala
        b->x += b->dx;
        b->y += b->dy;
        b->z += b->dz;

        // Fuera del mundo
        if (b->x < WORLD_MIN_X || b->x > WORLD_MAX_X ||
            b->z < WORLD_MIN_Z || b->z > WORLD_MAX_Z ||
            b->y < WORLD_FLOOR  || b->y > WORLD_CEIL) {
            b->active = 0;
            continue;
        }

        // Colisión con jugadores (esfera)
        for (int p = 0; p < 2; p++) {
            if (!gs->players[p].alive) continue;
            if (b->owner == p) continue;          // no auto-impacto

            float ddx = b->x - gs->players[p].x;
            float ddy = b->y - gs->players[p].y;
            float ddz = b->z - gs->players[p].z;
            float d2  = ddx*ddx + ddy*ddy + ddz*ddz;

            if (d2 < PLAYER_RADIUS * PLAYER_RADIUS) {
                gs->players[p].hp -= BULLET_DAMAGE;
                b->active = 0;
                if (gs->players[p].hp <= 0.0f) {
                    gs->players[p].alive = 0;
                    gs->players[p].hp    = 0.0f;
                    gs->winner = 1 - p;
                }
                break;
            }
        }
    }
}

// -------- bucle principal --------

void start_game(int client_fds[2]) {
    GameState gs;
    memset(&gs, 0, sizeof(gs));
    gs.winner = -1;

    // Posiciones iniciales: enfrentados a lo largo del eje X
    gs.players[0] = (PlayerState){
        .x = -10.0f, .y = 1.5f, .z = 0.0f,
        .yaw = 0.0f,   .pitch = 0.0f,
        .hp = MAX_HP, .alive = 1
    };
    gs.players[1] = (PlayerState){
        .x = 10.0f, .y = 1.5f, .z = 0.0f,
        .yaw = 180.0f, .pitch = 0.0f,
        .hp = MAX_HP, .alive = 1
    };

    int lamport = 0;
    int tick_counter = 0;

    while (gs.winner < 0) {
        // Esperar paquetes con timeout corto (~16 ms = 60 Hz)
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = 0;
        for (int i = 0; i < 2; i++) {
            FD_SET(client_fds[i], &rfds);
            if (client_fds[i] > maxfd) maxfd = client_fds[i];
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 16000 };
        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        if (ready > 0) {
            for (int i = 0; i < 2; i++) {
                if (!FD_ISSET(client_fds[i], &rfds)) continue;

                MsgHeader hdr;
                if (recv_all(client_fds[i], &hdr, sizeof(hdr)) < 0) {
                    printf("Jugador %d desconectado\n", i);
                    gs.winner = 1 - i;
                    break;
                }

                if (hdr.type == MSG_MOVE && hdr.payload_len == (int)sizeof(MoveMsg)) {
                    MoveMsg mv;
                    if (recv_all(client_fds[i], &mv, sizeof(mv)) < 0) {
                        gs.winner = 1 - i;
                        break;
                    }
                    // El cliente es autoridad de su propia posición y orientación
                    if (gs.players[i].alive) {
                        gs.players[i].x     = mv.x;
                        gs.players[i].y     = mv.y;
                        gs.players[i].z     = mv.z;
                        gs.players[i].yaw   = mv.yaw;
                        gs.players[i].pitch = mv.pitch;
                    }
                    if (lamport < mv.lamport_ts) lamport = mv.lamport_ts;

                } else if (hdr.type == MSG_SHOOT && hdr.payload_len == (int)sizeof(ShootMsg)) {
                    ShootMsg sm;
                    if (recv_all(client_fds[i], &sm, sizeof(sm)) < 0) {
                        gs.winner = 1 - i;
                        break;
                    }
                    if (gs.players[i].alive) {
                        printf("Jugador %d disparó\n", i);
                        spawn_bullet(&gs, i,
                                     sm.ox, sm.oy, sm.oz,
                                     sm.dx, sm.dy, sm.dz);
                    }
                    if (lamport < sm.lamport_ts) lamport = sm.lamport_ts;

                } else if (hdr.type == MSG_HEARTBEAT && hdr.payload_len == (int)sizeof(HeartbeatMsg)) {
                    HeartbeatMsg hb;
                    if (recv_all(client_fds[i], &hb, sizeof(hb)) < 0) {
                        gs.winner = 1 - i;
                        break;
                    }
                    if (lamport < hb.lamport_ts) lamport = hb.lamport_ts;

                } else {
                    // Payload desconocido: descartar
                    char discard[256];
                    int rem = hdr.payload_len;
                    while (rem > 0) {
                        int chunk = rem < 256 ? rem : 256;
                        if (recv_all(client_fds[i], discard, chunk) < 0) {
                            gs.winner = 1 - i;
                            break;
                        }
                        rem -= chunk;
                    }
                }
            }
        }

        update_game(&gs);
        gs.lamport_ts = ++lamport;
        send_state(client_fds, &gs);

        tick_counter++;
        if (tick_counter % 60 == 0) {
            int active_bullets = 0;
            for (int b = 0; b < MAX_BULLETS; b++) {
                if (gs.bullets[b].active) active_bullets++;
            }
            printf("[Stats] P0: HP %.0f, Pos(%.1f, %.1f) | P1: HP %.0f, Pos(%.1f, %.1f) | Balas: %d\n",
                   gs.players[0].hp, gs.players[0].x, gs.players[0].z,
                   gs.players[1].hp, gs.players[1].x, gs.players[1].z,
                   active_bullets);
        }
    }

    // Mandar un último estado para que los clientes vean al ganador
    gs.lamport_ts = ++lamport;
    send_state(client_fds, &gs);

    printf("Partida terminada — Ganador: Jugador %d\n", gs.winner);
}

// -------- entry point --------

int main(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(9090),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, 2) < 0) {
        perror("listen"); return 1;
    }

    printf("Servidor 3D escuchando en puerto 9090. Esperando 2 jugadores...\n");

    int client_fds[2];
    for (int i = 0; i < 2; i++) {
        client_fds[i] = accept(server_fd, NULL, NULL);
        if (client_fds[i] < 0) { perror("accept"); return 1; }

        int nodelay = 1;
        setsockopt(client_fds[i], IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // Enviar id asignado
        send(client_fds[i], &i, sizeof(int), 0);
        printf("Jugador %d conectado\n", i);
    }

    printf("Ambos conectados. Iniciando partida.\n");
    start_game(client_fds);

    close(client_fds[0]);
    close(client_fds[1]);
    close(server_fd);
    return 0;
}
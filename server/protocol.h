// protocol.h — protocolo compartido entre servidor y clientes (3D)
#ifndef PROTOCOL_H
#define PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

// -------- constantes del juego --------

#define MAX_BULLETS    64
#define MAX_HP         100.0f

// Velocidades (en unidades del mundo OpenGL, no píxeles)
#define PLAYER_SPEED   0.15f
#define BULLET_SPEED   0.8f

// Límites del mundo 3D (en lugar de MAP_W / MAP_H)
#define WORLD_MIN_X   -20.0f
#define WORLD_MAX_X    20.0f
#define WORLD_MIN_Z   -20.0f
#define WORLD_MAX_Z    20.0f
#define WORLD_FLOOR    0.0f
#define WORLD_CEIL     50.0f

// Radio de hitbox del jugador (esfera)
#define PLAYER_RADIUS  0.6f

// Daño por bala
#define BULLET_DAMAGE  10.0f

// -------- tipos de mensaje --------

typedef enum {
    MSG_MOVE      = 1,   // cliente -> servidor: posición/orientación propia
    MSG_INPUT     = 2,   // (legacy, sin uso en 3D pero lo mantenemos)
    MSG_STATE     = 3,   // servidor -> cliente: GameState completo
    MSG_HEARTBEAT = 4,   // cliente -> servidor: keepalive
    MSG_SHOOT     = 5    // cliente -> servidor: evento de disparo (origen + dir)
} MsgType;

// Header común de todo mensaje (8 bytes)
#pragma pack(push, 1)
typedef struct {
    int type;
    int payload_len;
} MsgHeader;

// -------- estado del juego --------

typedef struct {
    float x, y, z;        // posición 3D (en el cliente, equivale a camera.Position)
    float yaw, pitch;     // orientación (grados)
    float hp;
    int   alive;          // 1 = vivo, 0 = muerto
} PlayerState;

typedef struct {
    float x, y, z;        // posición actual
    float dx, dy, dz;     // velocidad (dirección normalizada * BULLET_SPEED)
    int   owner;          // id del jugador que la disparó (0 o 1)
    int   active;         // 1 = en vuelo, 0 = libre
} BulletState;

typedef struct {
    PlayerState players[2];
    BulletState bullets[MAX_BULLETS];
    int         winner;       // -1 si nadie ganó aún, 0 o 1 si hubo ganador
    int         lamport_ts;
} GameState;

// -------- payloads cliente -> servidor --------

// Posición autoritativa del propio jugador
typedef struct {
    int   player_id;
    float x, y, z;
    float yaw, pitch;
    int   lamport_ts;
} MoveMsg;

// Disparo: origen (cañón del arma) + dirección normalizada
typedef struct {
    int   player_id;
    float ox, oy, oz;
    float dx, dy, dz;
    int   lamport_ts;
} ShootMsg;

// Heartbeat
typedef struct {
    int player_id;
    int lamport_ts;
} HeartbeatMsg;

// Legacy: lo dejamos para no romper compilaciones antiguas
typedef struct {
    int dx, dy;
    int shooting;
    int lamport_ts;
} InputMsg;
#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif // PROTOCOL_H
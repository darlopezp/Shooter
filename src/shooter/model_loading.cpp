// model_loading.cpp — cliente OpenGL del shooter 3D multiplayer
// Conecta a 127.0.0.1:9090, recibe id de jugador, envía MoveMsg/ShootMsg,
// renderiza al oponente como un zapper y las balas como cubos.
#define _HAS_STD_BYTE 0
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <learnopengl/filesystem.h>
#include <learnopengl/shader_m.h>
#include <learnopengl/camera.h>
#include <learnopengl/model.h>

#include <iostream>
#include <vector>
#include <cstring>
#include <cstdio>

// Red (POSIX)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>

extern "C" {
#include "protocol.h"
}

// -------- prototipos --------
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void processInput(GLFWwindow* window);
void toggleFullscreen(GLFWwindow* window);
unsigned int createPlane();
unsigned int createCube();

// -------- configuración --------
const unsigned int SCR_WIDTH = 800;
const unsigned int SCR_HEIGHT = 600;

Camera camera(glm::vec3(-10.0f, 1.5f, 0.0f));  // se sobreescribirá según id
float lastX = SCR_WIDTH / 2.0f;
float lastY = SCR_HEIGHT / 2.0f;
bool  firstMouse = true;
float run = 1.2f;

float deltaTime = 0.0f;
float lastFrame = 0.0f;

// Salto
const float ALTURA_BASE = 1.5f;
const float ALTURA_SALTO = 3.0f;
const float GRAVEDAD = 9.8f;
bool  estaSaltando = false;
float velocidadVertical = 0.0f;

// Fullscreen
bool isFullscreen = false;
int  windowedX, windowedY, windowedWidth, windowedHeight;

// Sol
const glm::vec3 SUN_POSITION = glm::vec3(-5.0f, 10.0f, -3.0f);
float           SUN_BRIGHTNESS = 1.0f;

// Disparo (efecto visual local)
bool  disparando = false;
float tiempoDisparo = 0.0f;
const float DURACION_DISPARO = 0.15f;
float retroceso = 0.0f;
const float MAX_RETROCESO = 15.0f;

// -------- colisiones --------
struct AABB {
    glm::vec3 min;
    glm::vec3 max;
};

struct Cubo {
    glm::vec3 position;
    glm::vec3 scale;
    AABB      hitbox;
};

// -------- struct árbol --------
struct Arbol {
    glm::vec3 position;
    float     rotationY;  // rotación en grados sobre el eje Y
    glm::vec3 scale;
    AABB      hitbox;
};

AABB calcularHitbox(glm::vec3 pos, glm::vec3 scale) {
    return { pos - scale * 0.5f, pos + scale * 0.5f };
}

bool colisiona(glm::vec3 playerPos, AABB box) {
    float radio = 0.3f;
    bool overlapXZ =
        playerPos.x + radio > box.min.x &&
        playerPos.x - radio < box.max.x &&
        playerPos.z + radio > box.min.z &&
        playerPos.z - radio < box.max.z;

    float pies = playerPos.y - ALTURA_BASE;
    float techoCubo = box.max.y;

    bool overlapY = pies < techoCubo - 0.05f && playerPos.y > box.min.y;
    return overlapXZ && overlapY;
}

std::vector<Cubo>  cubosEscenario;
std::vector<Arbol> arboles;          // ← vector global de árboles

// -------- struct instancias de modelos --------
struct ModelInstance {
    glm::vec3 position;
    float     rotationAngle;
    glm::vec3 rotationAxis;
    glm::vec3 scale;
};

// -------- estado de red (globales para mantener cambios mínimos en processInput) --------
static int        g_sockfd = -1;
static int        g_my_id = 0;
static int        g_lamport = 0;
static GameState  g_gs;
static bool       g_match_ended = false;

// -------- helpers de red --------
static int send_all(int fd, const void* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = send(fd, (const char*)buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static int recv_all_blocking(int fd, void* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(fd, (char*)buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static int send_msg(int fd, MsgType type, const void* payload, int len) {
    MsgHeader hdr;
    hdr.type = (int)type;
    hdr.payload_len = len;
    if (send_all(fd, &hdr, sizeof(hdr)) < 0) return -1;
    if (len > 0 && send_all(fd, payload, len) < 0) return -1;
    return 0;
}

// Drena todos los GameState pendientes en el socket, queda con el más reciente
static void poll_server_state() {
    while (true) {
        MsgHeader hdr;
        int n = recv(g_sockfd, (char*)&hdr, sizeof(hdr), MSG_PEEK);

        if (n == 0) {
            std::cerr << "Servidor desconectado\n";
            g_match_ended = true;
            return;
        }
        if (n < 0) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                std::cerr << "Error de socket: " << err << "\n";
                g_match_ended = true;
            }
            return;
        }
        if (n < (int)sizeof(hdr)) return;

        if (hdr.payload_len < 0 || hdr.payload_len > 65536) {
            std::cerr << "payload_len inválido: " << hdr.payload_len << "\n";
            g_match_ended = true;
            return;
        }

        int total = (int)sizeof(MsgHeader) + hdr.payload_len;
        std::vector<char> peek_buf(total);
        int m = recv(g_sockfd, peek_buf.data(), total, MSG_PEEK);
        if (m < total) return;

        recv_all_blocking(g_sockfd, &hdr, sizeof(hdr));

        if (hdr.type == MSG_STATE && hdr.payload_len == (int)sizeof(GameState)) {
            GameState tmp;
            if (recv_all_blocking(g_sockfd, &tmp, sizeof(tmp)) < 0) {
                g_match_ended = true;
                return;
            }
            if (g_lamport < tmp.lamport_ts) g_lamport = tmp.lamport_ts;

            tmp.players[g_my_id].x = camera.Position.x;
            tmp.players[g_my_id].y = camera.Position.y;
            tmp.players[g_my_id].z = camera.Position.z;
            tmp.players[g_my_id].yaw = camera.Yaw;
            tmp.players[g_my_id].pitch = camera.Pitch;
            g_gs = tmp;

            if (g_gs.winner >= 0) g_match_ended = true;
        }
        else {
            std::vector<char> trash(hdr.payload_len);
            recv_all_blocking(g_sockfd, trash.data(), hdr.payload_len);
        }
    }
}

static void send_move_update() {
    MoveMsg mv;
    mv.player_id = g_my_id;
    mv.x = camera.Position.x;
    mv.y = camera.Position.y;
    mv.z = camera.Position.z;
    mv.yaw = camera.Yaw;
    mv.pitch = camera.Pitch;
    mv.lamport_ts = ++g_lamport;
    if (send_msg(g_sockfd, MSG_MOVE, &mv, sizeof(mv)) < 0) {
        std::cerr << "Error enviando MOVE\n";
        g_match_ended = true;
    }
}

static void send_shoot(const glm::vec3& origen, const glm::vec3& dir) {
    ShootMsg sm;
    sm.player_id = g_my_id;
    sm.ox = origen.x; sm.oy = origen.y; sm.oz = origen.z;
    sm.dx = dir.x;    sm.dy = dir.y;    sm.dz = dir.z;
    sm.lamport_ts = ++g_lamport;
    if (send_msg(g_sockfd, MSG_SHOOT, &sm, sizeof(sm)) < 0) {
        std::cerr << "Error enviando SHOOT\n";
        g_match_ended = true;
    }
}

static void send_heartbeat() {
    HeartbeatMsg hb;
    hb.player_id = g_my_id;
    hb.lamport_ts = ++g_lamport;
    send_msg(g_sockfd, MSG_HEARTBEAT, &hb, sizeof(hb));
}

// -------- conexión al servidor --------
static bool connect_to_server(const char* host, int port) {
    g_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sockfd < 0) { perror("socket"); return false; }

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &srv.sin_addr) <= 0) {
        std::cerr << "Host inválido: " << host << "\n";
        return false;
    }

    std::cout << "Conectando a " << host << ":" << port << " ...\n";
    if (connect(g_sockfd, (sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("connect");
        return false;
    }

    int nodelay = 1;
    setsockopt(g_sockfd, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

    if (recv_all_blocking(g_sockfd, &g_my_id, sizeof(int)) < 0) {
        std::cerr << "No se recibió ID del servidor\n";
        return false;
    }
    std::cout << "Conectado como Jugador " << g_my_id
        << ". Esperando al otro jugador...\n";

    u_long mode = 1;
    ioctlsocket(g_sockfd, FIONBIO, &mode);

    std::memset(&g_gs, 0, sizeof(g_gs));
    g_gs.winner = -1;
    g_gs.players[0].hp = MAX_HP; g_gs.players[0].alive = 1;
    g_gs.players[1].hp = MAX_HP; g_gs.players[1].alive = 1;

    return true;
}

// -------- main --------
int main(int argc, char* argv[]) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Fallo al inicializar Winsock" << std::endl;
        return -1;
    }
    const char* host = (argc > 1) ? argv[1] : "56.124.77.160";
    int         port = (argc > 2) ? atoi(argv[2]) : 9090;

    if (!connect_to_server(host, port)) return 1;

    if (g_my_id == 0) {
        camera.Position = glm::vec3(-10.0f, ALTURA_BASE, 0.0f);
        camera.Yaw = 0.0f;
    }
    else {
        camera.Position = glm::vec3(10.0f, ALTURA_BASE, 0.0f);
        camera.Yaw = 180.0f;
    }
    camera.Pitch = 0.0f;
    camera.ProcessMouseMovement(0.0f, 0.0f);

    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    char title0[64];
    snprintf(title0, sizeof(title0), "Shooter 3D — Jugador %d", g_my_id);
    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, title0, NULL, NULL);
    if (window == NULL) {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cout << "Failed to initialize GLAD" << std::endl;
        return -1;
    }

    stbi_set_flip_vertically_on_load(true);
    glEnable(GL_DEPTH_TEST);

    // -------- Shaders --------
    Shader ourShader("model_loading.vs", "model_loading.fs");
    Shader sunShader("sun.vs", "sun.fs");

    // -------- Modelos --------
    Model zapperModel("../../resources/objects/zapper2/zapper.obj");
    Model arbolModel("../../resources/objects/arbol/tree.obj");

    std::vector<ModelInstance> zapperInstances = {};

    // -------- Cubos del escenario --------
    cubosEscenario = {};
    for (auto& cubo : cubosEscenario)
        cubo.hitbox = calcularHitbox(cubo.position, cubo.scale);

    // ======================================================
    // -------- Árboles — EDITA AQUÍ para agregar más -------
    // Formato: { posición(X,Y,Z), rotaciónY°, escala(X,Y,Z) }
    // El hitbox cubre solo el tronco: ajusta glm::vec3(0.4f, 2.0f, 0.4f)
    // si tu modelo es más gordo/alto.
    // ======================================================
    arboles = {
        // Zona norte
        { glm::vec3(-13.0f, 0.0f, -14.0f),  37.0f, glm::vec3(1.1f), {} },
        { glm::vec3(-8.5f,  0.0f, -11.0f), 112.0f, glm::vec3(0.9f), {} },
        { glm::vec3(-15.0f, 0.0f,  -7.0f), 255.0f, glm::vec3(1.3f), {} },
        { glm::vec3(6.0f,  0.0f, -13.0f),  83.0f, glm::vec3(1.0f), {} },
        { glm::vec3(11.0f, 0.0f,  -9.0f), 194.0f, glm::vec3(0.85f),{} },
        { glm::vec3(14.0f, 0.0f, -15.0f),  21.0f, glm::vec3(1.2f), {} },

        // Zona sur
        { glm::vec3(-12.0f, 0.0f,  10.0f), 310.0f, glm::vec3(1.0f), {} },
        { glm::vec3(-7.0f, 0.0f,  14.0f),  66.0f, glm::vec3(1.15f),{} },
        { glm::vec3(-16.0f, 0.0f,  16.0f), 142.0f, glm::vec3(0.95f),{} },
        { glm::vec3(5.0f, 0.0f,  12.0f), 229.0f, glm::vec3(1.05f),{} },
        { glm::vec3(13.0f, 0.0f,   8.0f),  55.0f, glm::vec3(1.3f), {} },
        { glm::vec3(16.0f, 0.0f,  15.0f), 178.0f, glm::vec3(0.8f), {} },

        // Zona oeste
        { glm::vec3(-17.0f, 0.0f,  -2.0f),  90.0f, glm::vec3(1.2f), {} },
        { glm::vec3(-14.0f, 0.0f,   3.0f), 333.0f, glm::vec3(0.9f), {} },
        { glm::vec3(-19.0f, 0.0f,   7.0f),  17.0f, glm::vec3(1.1f), {} },

        // Zona este
        { glm::vec3(17.0f, 0.0f,   1.0f), 205.0f, glm::vec3(1.0f), {} },
        { glm::vec3(15.0f, 0.0f,  -4.0f),  73.0f, glm::vec3(1.25f),{} },
        { glm::vec3(19.0f, 0.0f,  -9.0f), 158.0f, glm::vec3(0.88f),{} },

        // Árboles dispersos intermedios
        { glm::vec3(-9.0f, 0.0f,   6.0f), 280.0f, glm::vec3(1.05f),{} },
        { glm::vec3(8.0f, 0.0f,  -6.0f),  44.0f, glm::vec3(0.92f),{} },
        { glm::vec3(-5.0f, 0.0f, -10.0f), 317.0f, glm::vec3(1.1f), {} },
        { glm::vec3(10.0f, 0.0f,   5.0f), 101.0f, glm::vec3(0.95f),{} },

        // Árboles centrales
		{ glm::vec3(0.0f, 0.0f, 0.0f), 45.0f, glm::vec3(1.3f), {} },
        { glm::vec3(-4.0f, 0.0f, 2.0f), 120.0f, glm::vec3(1.2f), {} },
        { glm::vec3(5.0f, 0.0f, -3.0f), 200.0f, glm::vec3(1.15f),{} },
    };

    // Calcula la hitbox de cada árbol (tronco escalado)
    for (auto& a : arboles) {
        glm::vec3 trunkHalf = glm::vec3(0.4f, 2.0f, 0.4f) * a.scale;
        a.hitbox.min = a.position - trunkHalf;
        a.hitbox.max = a.position + trunkHalf;
    }

    // -------- Plano --------
    unsigned int planeVAO = createPlane();

    unsigned int planeTexture;
    glGenTextures(1, &planeTexture);
    glBindTexture(GL_TEXTURE_2D, planeTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    int texWidth, texHeight, texChannels;
    unsigned char* data = stbi_load(
        "../../resources/textures/pasto2.png",
        &texWidth, &texHeight, &texChannels, 0
    );
    if (data) {
        GLenum format = (texChannels == 4) ? GL_RGBA : GL_RGB;
        glTexImage2D(GL_TEXTURE_2D, 0, format, texWidth, texHeight, 0, format, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
        std::cout << "Textura cargada OK: " << texWidth << "x" << texHeight << std::endl;
    }
    else {
        std::cout << "ERROR al cargar textura. Razon: " << stbi_failure_reason() << std::endl;
    }
    stbi_image_free(data);

    unsigned int cubeVAO = createCube();

    int  net_tick = 0;
    int  hb_tick = 0;
    bool prev_mouse = false;

    // -------- Render loop --------
    while (!glfwWindowShouldClose(window)) {
        float currentFrame = static_cast<float>(glfwGetTime());
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        if (!g_match_ended) poll_server_state();

        if (g_match_ended) {
            char endTitle[128];
            if (g_gs.winner == g_my_id)
                snprintf(endTitle, sizeof(endTitle), "Shooter 3D — ¡GANASTE!");
            else if (g_gs.winner >= 0)
                snprintf(endTitle, sizeof(endTitle), "Shooter 3D — Perdiste");
            else
                snprintf(endTitle, sizeof(endTitle), "Shooter 3D — Desconectado");
            glfwSetWindowTitle(window, endTitle);
        }

        processInput(window);

        bool mouse_now = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
        if (mouse_now && !prev_mouse && !g_match_ended) {
            glm::vec3 origen = camera.Position + camera.Front * 1.0f;
            glm::vec3 dir = glm::normalize(camera.Front);
            send_shoot(origen, dir);
        }
        prev_mouse = mouse_now;

        if (!g_match_ended && ++net_tick >= 2) {
            net_tick = 0;
            send_move_update();
        }

        if (++hb_tick >= 60) {
            hb_tick = 0;
            if (!g_match_ended) send_heartbeat();
        }

        if (!g_match_ended) {
            char hudTitle[160];
            int other = 1 - g_my_id;
            snprintf(hudTitle, sizeof(hudTitle),
                "Shooter 3D — P%d HP=%.0f | Enemigo HP=%.0f",
                g_my_id,
                g_gs.players[g_my_id].hp,
                g_gs.players[other].hp);
            glfwSetWindowTitle(window, hudTitle);
        }

        // -------- Renderizado --------
        glClearColor(0.53f, 0.81f, 0.98f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::mat4 projection = glm::perspective(
            glm::radians(camera.Zoom),
            (float)SCR_WIDTH / (float)SCR_HEIGHT,
            0.1f, 100.0f
        );
        glm::mat4 view = camera.GetViewMatrix();

        ourShader.use();

        glm::vec3 sunDirection = glm::normalize(glm::vec3(0.0f) - SUN_POSITION);
        ourShader.setVec3("dirLight.direction", sunDirection);
        ourShader.setVec3("dirLight.ambient", glm::vec3(0.3f, 0.3f, 0.3f) * SUN_BRIGHTNESS);
        ourShader.setVec3("dirLight.diffuse", glm::vec3(0.9f, 0.85f, 0.7f) * SUN_BRIGHTNESS);
        ourShader.setVec3("dirLight.specular", glm::vec3(1.0f, 1.0f, 0.9f) * SUN_BRIGHTNESS);

        glm::vec3 posicionArma = camera.Position
            + camera.Front * 1.8f
            + camera.Right * 0.5f
            + camera.Up * -0.85f;

        ourShader.setVec3("shotLight.position", posicionArma);
        ourShader.setVec3("shotLight.diffuse", glm::vec3(1.5f, 1.0f, 0.3f));
        ourShader.setFloat("shotLight.constant", 1.0f);
        ourShader.setFloat("shotLight.linear", 0.35f);
        ourShader.setFloat("shotLight.quadratic", 0.44f);
        ourShader.setFloat("shotLight.intensity",
            disparando ? (1.0f - tiempoDisparo / DURACION_DISPARO) : 0.0f
        );

        ourShader.setVec3("viewPos", camera.Position);
        ourShader.setFloat("shininess", 32.0f);
        ourShader.setMat4("projection", projection);
        ourShader.setMat4("view", view);

        // -------- Zappers decorativos del mundo --------
        for (const auto& inst : zapperInstances) {
            glm::mat4 model = glm::mat4(1.0f);
            model = glm::translate(model, inst.position);
            model = glm::rotate(model, glm::radians(inst.rotationAngle), inst.rotationAxis);
            model = glm::scale(model, inst.scale);
            ourShader.setMat4("model", model);
            zapperModel.Draw(ourShader);
        }

        // -------- Árboles --------
        for (const auto& a : arboles) {
            glm::mat4 model = glm::mat4(1.0f);
            model = glm::translate(model, a.position);
            model = glm::rotate(model, glm::radians(a.rotationY), glm::vec3(0.0f, 1.0f, 0.0f));
            model = glm::scale(model, a.scale);
            ourShader.setMat4("model", model);
            arbolModel.Draw(ourShader);
        }

        // -------- Oponente: zapper en su posición/orientación --------
        int other = 1 - g_my_id;
        const PlayerState& op = g_gs.players[other];
        if (op.alive) {
            glm::mat4 m = glm::mat4(1.0f);
            m = glm::translate(m, glm::vec3(op.x, op.y - ALTURA_BASE, op.z));
            m = glm::rotate(m, glm::radians(-op.yaw + 90.0f), glm::vec3(0, 1, 0));
            m = glm::scale(m, glm::vec3(0.5f));
            ourShader.setMat4("model", m);
            zapperModel.Draw(ourShader);
        }

        // -------- Zapper en mano (propio) --------
        glm::mat4 modelMano = glm::mat4(1.0f);

        glm::vec3 posicionMano = camera.Position
            + camera.Front * 1.8f
            + camera.Right * 0.5f
            + camera.Up * -0.85f;

        modelMano = glm::translate(modelMano, posicionMano);

        glm::mat4 rotacionCamara = glm::mat4(1.0f);
        rotacionCamara[0] = glm::vec4(camera.Right, 0.0f);
        rotacionCamara[1] = glm::vec4(camera.Up, 0.0f);
        rotacionCamara[2] = glm::vec4(-camera.Front, 0.0f);
        modelMano = modelMano * rotacionCamara;

        modelMano = glm::rotate(modelMano, glm::radians(retroceso), glm::vec3(1.0f, 0.0f, 0.0f));
        modelMano = glm::rotate(modelMano, glm::radians(180.0f), glm::vec3(0, 1, 0));
        modelMano = glm::scale(modelMano, glm::vec3(0.3f));

        ourShader.setMat4("model", modelMano);
        zapperModel.Draw(ourShader);

        // -------- Sol, cubos escenario y balas (sunShader) --------
        sunShader.use();
        sunShader.setMat4("projection", projection);
        sunShader.setMat4("view", view);

        sunShader.setVec3("uColor", glm::vec3(1.0f, 0.95f, 0.3f));
        glm::mat4 sunModel = glm::mat4(1.0f);
        sunModel = glm::translate(sunModel, SUN_POSITION);
        sunModel = glm::scale(sunModel, glm::vec3(0.8f));
        sunShader.setMat4("model", sunModel);
        glBindVertexArray(cubeVAO);
        glDrawArrays(GL_TRIANGLES, 0, 36);

        sunShader.setVec3("uColor", glm::vec3(0.6f, 0.6f, 0.6f));
        for (const auto& cubo : cubosEscenario) {
            glm::mat4 cuboModel = glm::mat4(1.0f);
            cuboModel = glm::translate(cuboModel, cubo.position);
            cuboModel = glm::scale(cuboModel, cubo.scale);
            sunShader.setMat4("model", cuboModel);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }

        sunShader.setVec3("uColor", glm::vec3(1.0f, 0.35f, 0.1f));
        for (int i = 0; i < MAX_BULLETS; i++) {
            if (!g_gs.bullets[i].active) continue;
            glm::mat4 bm = glm::mat4(1.0f);
            bm = glm::translate(bm, glm::vec3(g_gs.bullets[i].x,
                g_gs.bullets[i].y,
                g_gs.bullets[i].z));
            bm = glm::scale(bm, glm::vec3(0.15f));
            sunShader.setMat4("model", bm);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }

        // Cubo del oponente (hitbox visual)
        if (op.alive) {
            sunShader.setVec3("uColor", glm::vec3(0.2f, 0.9f, 0.2f));
            glm::mat4 pm = glm::mat4(1.0f);
            pm = glm::translate(pm, glm::vec3(op.x, op.y, op.z));
            pm = glm::scale(pm, glm::vec3(1.2f));
            sunShader.setMat4("model", pm);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }
        glBindVertexArray(0);

        // -------- Plano --------
        ourShader.use();
        ourShader.setMat4("projection", projection);
        ourShader.setMat4("view", view);

        glm::mat4 planeModel = glm::mat4(1.0f);
        planeModel = glm::translate(planeModel, glm::vec3(0.0f, -0.01f, 0.0f));
        planeModel = glm::scale(planeModel, glm::vec3(50.0f, 1.0f, 50.0f));
        ourShader.setMat4("model", planeModel);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, planeTexture);
        glBindVertexArray(planeVAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);

        glfwSwapBuffers(window);
        glfwPollEvents();

        if (g_match_ended) {
            static float endTimer = 0.0f;
            endTimer += deltaTime;
            if (endTimer > 3.0f) glfwSetWindowShouldClose(window, true);
        }
    }

    glDeleteVertexArrays(1, &planeVAO);
    glDeleteVertexArrays(1, &cubeVAO);
    glDeleteTextures(1, &planeTexture);

    closesocket(g_sockfd);
    glfwTerminate();
    return 0;
}

// -------- Plano --------
unsigned int createPlane() {
    float planeVertices[] = {
        -0.5f, 0.0f, -0.5f,   0.0f, 1.0f, 0.0f,    0.0f, 10.0f,
         0.5f, 0.0f, -0.5f,   0.0f, 1.0f, 0.0f,   10.0f, 10.0f,
         0.5f, 0.0f,  0.5f,   0.0f, 1.0f, 0.0f,   10.0f,  0.0f,
        -0.5f, 0.0f,  0.5f,   0.0f, 1.0f, 0.0f,    0.0f,  0.0f,
    };
    unsigned int planeIndices[] = { 0, 1, 2, 2, 3, 0 };

    unsigned int VAO, VBO, EBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(planeVertices), planeVertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(planeIndices), planeIndices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    return VAO;
}

// -------- Cubo --------
unsigned int createCube() {
    float vertices[] = {
        -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  -0.5f, 0.5f,-0.5f,  -0.5f,-0.5f,-0.5f,
        -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,  -0.5f,-0.5f, 0.5f,
        -0.5f, 0.5f, 0.5f, -0.5f, 0.5f,-0.5f, -0.5f,-0.5f,-0.5f, -0.5f,-0.5f,-0.5f,  -0.5f,-0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,
         0.5f, 0.5f, 0.5f,  0.5f, 0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,   0.5f,-0.5f, 0.5f,   0.5f, 0.5f, 0.5f,
        -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  -0.5f,-0.5f, 0.5f,  -0.5f,-0.5f,-0.5f,
        -0.5f, 0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,  -0.5f, 0.5f,-0.5f
    };

    unsigned int VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    return VAO;
}

// -------- Fullscreen --------
void toggleFullscreen(GLFWwindow* window) {
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    if (!isFullscreen) {
        glfwGetWindowPos(window, &windowedX, &windowedY);
        glfwGetWindowSize(window, &windowedWidth, &windowedHeight);
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        isFullscreen = true;
    }
    else {
        glfwSetWindowMonitor(window, NULL, windowedX, windowedY, windowedWidth, windowedHeight, 0);
        isFullscreen = false;
    }
}

// -------- Input --------
void processInput(GLFWwindow* window) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);

    if (!g_gs.players[g_my_id].alive) return;

    glm::vec3 posAnterior = camera.Position;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        camera.ProcessKeyboard(FORWARD, deltaTime * run);
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        camera.ProcessKeyboard(BACKWARD, deltaTime * run);
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        camera.ProcessKeyboard(LEFT, deltaTime * run);
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        camera.ProcessKeyboard(RIGHT, deltaTime * run);

    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
        run = 2.0f;
    else
        run = 1.1f;

    if (camera.Position.x < WORLD_MIN_X) camera.Position.x = WORLD_MIN_X;
    if (camera.Position.x > WORLD_MAX_X) camera.Position.x = WORLD_MAX_X;
    if (camera.Position.z < WORLD_MIN_Z) camera.Position.z = WORLD_MIN_Z;
    if (camera.Position.z > WORLD_MAX_Z) camera.Position.z = WORLD_MAX_Z;

    // Colisiones horizontales con cubos del escenario
    for (const auto& cubo : cubosEscenario) {
        if (colisiona(camera.Position, cubo.hitbox)) {
            glm::vec3 soloX = { camera.Position.x, posAnterior.y, posAnterior.z };
            if (!colisiona(soloX, cubo.hitbox))
                camera.Position = soloX;
            else {
                glm::vec3 soloZ = { posAnterior.x, posAnterior.y, camera.Position.z };
                if (!colisiona(soloZ, cubo.hitbox))
                    camera.Position = soloZ;
                else
                    camera.Position = posAnterior;
            }
        }
    }

    // Colisiones horizontales con árboles
    for (const auto& a : arboles) {
        if (colisiona(camera.Position, a.hitbox)) {
            glm::vec3 soloX = { camera.Position.x, posAnterior.y, posAnterior.z };
            if (!colisiona(soloX, a.hitbox))
                camera.Position = soloX;
            else {
                glm::vec3 soloZ = { posAnterior.x, posAnterior.y, camera.Position.z };
                if (!colisiona(soloZ, a.hitbox))
                    camera.Position = soloZ;
                else
                    camera.Position = posAnterior;
            }
        }
    }

    // Salto y gravedad
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS && !estaSaltando) {
        estaSaltando = true;
        velocidadVertical = sqrt(2.0f * GRAVEDAD * (ALTURA_SALTO - ALTURA_BASE));
    }

    float suelo = ALTURA_BASE;
    float radio = 0.3f;

    for (const auto& cubo : cubosEscenario) {
        bool dentroXZ =
            camera.Position.x + radio > cubo.hitbox.min.x &&
            camera.Position.x - radio < cubo.hitbox.max.x &&
            camera.Position.z + radio > cubo.hitbox.min.z &&
            camera.Position.z - radio < cubo.hitbox.max.z;

        if (dentroXZ) {
            float superficieCubo = cubo.hitbox.max.y + ALTURA_BASE;
            if (superficieCubo > suelo) suelo = superficieCubo;
        }
    }

    // Superficie de los árboles (por si escala hace que tengan techo)
    for (const auto& a : arboles) {
        bool dentroXZ =
            camera.Position.x + radio > a.hitbox.min.x &&
            camera.Position.x - radio < a.hitbox.max.x &&
            camera.Position.z + radio > a.hitbox.min.z &&
            camera.Position.z - radio < a.hitbox.max.z;

        if (dentroXZ) {
            float superficieArbol = a.hitbox.max.y + ALTURA_BASE;
            if (superficieArbol > suelo) suelo = superficieArbol;
        }
    }

    if (!estaSaltando && camera.Position.y > suelo + 0.01f) {
        estaSaltando = true;
        velocidadVertical = 0.0f;
    }

    if (estaSaltando) {
        camera.Position.y += velocidadVertical * deltaTime;
        velocidadVertical -= GRAVEDAD * deltaTime;
        if (camera.Position.y <= suelo) {
            camera.Position.y = suelo;
            velocidadVertical = 0.0f;
            estaSaltando = false;
        }
    }
    else {
        camera.Position.y = suelo;
    }

    // Disparo (efecto visual local)
    static bool mouseWasPressed = false;
    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS && !mouseWasPressed) {
        disparando = true;
        tiempoDisparo = 0.0f;
        retroceso = MAX_RETROCESO;
        mouseWasPressed = true;
    }
    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_RELEASE)
        mouseWasPressed = false;

    if (disparando) {
        tiempoDisparo += deltaTime;
        retroceso = MAX_RETROCESO * (1.0f - (tiempoDisparo / DURACION_DISPARO));
        if (tiempoDisparo >= DURACION_DISPARO) {
            disparando = false;
            retroceso = 0.0f;
        }
    }

    // Fullscreen
    static bool fKeyWasPressed = false;
    if (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS && !fKeyWasPressed) {
        toggleFullscreen(window);
        fKeyWasPressed = true;
    }
    if (glfwGetKey(window, GLFW_KEY_F) == GLFW_RELEASE)
        fKeyWasPressed = false;
}

// -------- Callbacks --------
void framebuffer_size_callback(GLFWwindow* /*window*/, int width, int height) {
    glViewport(0, 0, width, height);
}

void mouse_callback(GLFWwindow* /*window*/, double xposIn, double yposIn) {
    float xpos = static_cast<float>(xposIn);
    float ypos = static_cast<float>(yposIn);
    if (firstMouse) { lastX = xpos; lastY = ypos; firstMouse = false; }
    float xoffset = xpos - lastX;
    float yoffset = lastY - ypos;
    lastX = xpos;
    lastY = ypos;
    camera.ProcessMouseMovement(xoffset, yoffset);
}

void scroll_callback(GLFWwindow* /*window*/, double /*xoffset*/, double yoffset) {
    camera.ProcessMouseScroll(static_cast<float>(yoffset));
}
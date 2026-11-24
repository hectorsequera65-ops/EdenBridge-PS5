#include "bridge.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#ifdef EDEN_PS5
#include <ifaddrs.h>
#include <net/if.h>
#endif
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PORT 24879
#define ORIGIN "https://eden-rooms-ps5.pages.dev"
#define ROOMS_ORIGIN "https://eden-rooms.pages.dev"
#define CONFIG "/data/prosperoeden/config/prosperoeden.json"
#define MAX_HEADER 8192
#define MAX_BODY 2048
#define REQUEST_TIMEOUT_MS 5000
#define VERSION "v0.3.4-experimental"
#define TRANSFER_TIMEOUT_MS 300000
#ifdef EDEN_PS5
#define PLATFORM "ps5"
#else
#define PLATFORM "linux"
#endif

/* The server handles one request at a time. Keep the most recent form results
 * so a retry after a lost response cannot replay the write or notification. */
static struct submission {
    char token[33];
    char intent[33];
    int64_t expires;
    struct room room;
    int result;
} submissions[64];
static size_t next_submission;
static const char *config = CONFIG;

static void notify(const char *message)
{
    fprintf(stderr, "%s\n", message);
#ifdef EDEN_PS5
    /* The SDK sample and original ELF use a 0xc30-byte request. */
    struct {
        char prefix[45];
        char message[3075];
    } request = {0};
    extern int sceKernelSendNotificationRequest(int, const void *, size_t, int);
    snprintf(request.message, sizeof request.message, "%s", message);
    sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
#endif
}

static void notify_started(int port)
{
    char message[128];
#ifdef EDEN_PS5
    struct ifaddrs *interfaces;
    char ip[INET_ADDRSTRLEN] = "";
    if (!getifaddrs(&interfaces)) {
        for (struct ifaddrs *it = interfaces; it; it = it->ifa_next) {
            if (it->ifa_addr && it->ifa_addr->sa_family == AF_INET &&
                (it->ifa_flags & IFF_UP) && !(it->ifa_flags & IFF_LOOPBACK)) {
                const struct sockaddr_in *address = (const struct sockaddr_in *)it->ifa_addr;
                if (address->sin_addr.s_addr && inet_ntop(AF_INET, &address->sin_addr, ip, sizeof ip))
                    break;
            }
        }
        freeifaddrs(interfaces);
    }
    if (*ip) {
        snprintf(message, sizeof message, "Eden Bridge PS5: http://%s:%d", ip, port);
        notify(message);
        return;
    }
#endif
    snprintf(message, sizeof message, "Eden Bridge iniciado: puerto %d", port);
    notify(message);
}

static void send_all(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t n = send(fd, data, length, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        data += n;
        length -= (size_t)n;
    }
}

enum content { JSON, PAGE, SCRIPT };

static void reply(int fd, int status, const char *body, enum content content, int cors)
{
    const char *reason;
    switch (status) {
    case 200: reason = "OK"; break;
    case 204: reason = "No Content"; break;
    case 403: reason = "Forbidden"; break;
    case 404: reason = "Not Found"; break;
    case 408: reason = "Request Timeout"; break;
    case 409: reason = "Conflict"; break;
    case 500: reason = "Internal Server Error"; break;
    default: reason = "Bad Request"; break;
    }
    char header[1024];
    int size = snprintf(header, sizeof header,
        "HTTP/1.1 %d %s\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
        "Referrer-Policy: same-origin\r\n"
        "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; "
        "script-src 'self'; connect-src 'self'; frame-ancestors 'none'; form-action 'self'; base-uri 'none'\r\n"
        "Content-Type: %s; charset=utf-8\r\nContent-Length: %zu\r\n%s\r\n",
        status, reason, content == PAGE ? "text/html" :
        content == SCRIPT ? "text/javascript" : "application/json", strlen(body),
        cors ? "Access-Control-Allow-Origin: " ORIGIN "\r\nVary: Origin\r\n"
               "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
               "Access-Control-Allow-Headers: Content-Type\r\n"
               "Access-Control-Allow-Private-Network: true\r\n" : "");
    if (size > 0 && (size_t)size < sizeof header) {
        send_all(fd, header, (size_t)size);
        send_all(fd, body, strlen(body));
    }
}

struct request {
    char *method, *target, *host, *origin, *type, *referer, *body;
};

static int header_value(char **slot, char *value)
{
    if (*slot || !*value)
        return -1;
    *slot = value;
    return 0;
}

static int decimal(const char *text, size_t max, int *result)
{
    size_t length = strlen(text);
    if (!length || length > max || strspn(text, "0123456789") != length)
        return -1;
    *result = atoi(text);
    return 0;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static ssize_t receive(int fd, char *buffer, size_t size, int64_t deadline,
                       const char **error)
{
    struct pollfd socket = { .fd = fd, .events = POLLIN };
    for (;;) {
        int64_t now = monotonic_ms();
        if (now < 0) {
            *error = "clock";
            return -1;
        }
        int ready = now < deadline ? poll(&socket, 1, (int)(deadline - now)) : 0;
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0) {
            *error = ready == 0 ? "timeout" : "poll";
            return -1;
        }
        ssize_t count = recv(fd, buffer, size, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count <= 0 || memchr(buffer, 0, (size_t)count)) {
            *error = count == 0 ? "eof" : count < 0 ? "recv" : "nul";
            return -1;
        }
        return count;
    }
}

static int read_request(int fd, char *buffer, struct request *req,
                        const char **stage, const char **error)
{
    int64_t now = monotonic_ms();
    if (now < 0) {
        *error = "clock";
        return -1;
    }
    int64_t deadline = now + REQUEST_TIMEOUT_MS;
    size_t used = 0;
    char *end = NULL;
    while (!end && used < MAX_HEADER) {
        ssize_t n = receive(fd, buffer + used, MAX_HEADER - used, deadline, error);
        if (n < 0)
            return -1;
        used += (size_t)n;
        buffer[used] = 0;
        end = strstr(buffer, "\r\n\r\n");
    }
    if (!end) {
        *error = "header_limit";
        return -1;
    }
    *stage = "header_syntax";
    req->body = end + 4;
    char *line = strstr(buffer, "\r\n");
    *line = 0;
    req->method = buffer;
    char *target = strchr(buffer, ' ');
    if (!target)
        return -1;
    *target++ = 0;
    req->target = target;
    char *version = strchr(target, ' ');
    if (!version || *target != '/')
        return -1;
    *version++ = 0;
    if (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))
        return -1;
    char *length = NULL;
    while (line < end) {
        char *name = line + 2;
        line = strstr(name, "\r\n");
        *line = 0;
        char *colon = strchr(name, ':');
        if (!colon || colon == name)
            return -1;
        *colon = 0;
        if (strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&'*+-.^_`|~") != strlen(name))
            return -1;
        char *value = colon + 1;
        value += strspn(value, " \t");
        char *tail = line;
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t'))
            *--tail = 0;
        for (const unsigned char *c = (const unsigned char *)value; *c; c++)
            if ((*c < 32 && *c != '\t') || *c == 127)
                return -1;
        char **slot = NULL;
        if (!strcasecmp(name, "Host"))
            slot = &req->host;
        else if (!strcasecmp(name, "Origin"))
            slot = &req->origin;
        else if (!strcasecmp(name, "Referer"))
            slot = &req->referer;
        else if (!strcasecmp(name, "Content-Length"))
            slot = &length;
        else if (!strcasecmp(name, "Content-Type"))
            slot = &req->type;
        else if (!strcasecmp(name, "Transfer-Encoding"))
            return -1;
        if (slot && header_value(slot, value))
            return -1;
    }
    *stage = "content_length";
    if (!length && !strcmp(req->method, "POST"))
        return -1;
    int body_size = 0;
    if (length && (decimal(length, 4, &body_size) || body_size > MAX_BODY))
        return -1;
    size_t offset = (size_t)(req->body - buffer);
    size_t total = offset + (size_t)body_size;
    if (used > total)
        return -1;
    *stage = "body";
    while (used < total) {
        ssize_t n = receive(fd, buffer + used, total - used, deadline, error);
        if (n < 0)
            return -1;
        used += (size_t)n;
    }
    buffer[total] = 0;
    return 0;
}

static int room_valid(const struct room *room)
{
    struct in_addr ip;
    return inet_pton(AF_INET, room->host, &ip) == 1 &&
           room->port > 0 && room->port <= 65535 &&
           strspn(room->nick, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")
               == strlen(room->nick);
}

/* All accepted field values use URL-unreserved characters. */
static int form(char *text, struct room *room, const char **token, const char **intent)
{
    char *host = NULL, *port = NULL, *nick = NULL, *key = NULL, *intent_value = NULL;
    while (*text) {
        char *next = strchr(text, '&');
        if (next)
            *next++ = 0;
        char *value = strchr(text, '=');
        if (!value)
            return -1;
        *value++ = 0;
        char **slot = NULL;
        if (!strcmp(text, "host"))
            slot = &host;
        else if (!strcmp(text, "port"))
            slot = &port;
        else if (!strcmp(text, "nick"))
            slot = &nick;
        else if (!strcmp(text, "confirm") && token)
            slot = &key;
        else if (!strcmp(text, "intent") && intent)
            slot = &intent_value;
        if (!slot || header_value(slot, value))
            return -1;
        if (!next)
            break;
        if (!*next)
            return -1;
        text = next;
    }
    if (!host || !port || strlen(host) >= sizeof room->host ||
        decimal(port, 5, &room->port) || (nick && strlen(nick) >= sizeof room->nick))
        return -1;
    strcpy(room->host, host);
    if (nick)
        strcpy(room->nick, nick);
    if (token)
        *token = key;
    if (intent)
        *intent = intent_value;
    return 0;
}

static int valid_intent(const char *intent)
{
    return intent && strlen(intent) == 32 && strspn(intent, "0123456789abcdef") == 32;
}

static const char *new_submission(const struct room *room, const char *intent)
{
    struct submission *entry = &submissions[next_submission];
    next_submission = (next_submission + 1) % (sizeof submissions / sizeof *submissions);
    entry->result = 0;
    strcpy(entry->intent, intent ? intent : "");
    entry->expires = intent ? monotonic_ms() + TRANSFER_TIMEOUT_MS : 0;
    entry->room = *room;
    unsigned char random[16];
    arc4random_buf(random, sizeof random);
    for (size_t i = 0; i < sizeof random; i++)
        snprintf(entry->token + i * 2, 3, "%02x", (unsigned)random[i]);
    return entry->token;
}

static struct submission *find_submission(const char *token)
{
    if (!token || strlen(token) != sizeof submissions[0].token - 1)
        return NULL;
    for (size_t i = 0; i < sizeof submissions / sizeof *submissions; i++) {
        unsigned int diff = 0;
        for (size_t j = 0; j < sizeof submissions[i].token - 1; j++)
            diff |= (unsigned char)token[j] ^ (unsigned char)submissions[i].token[j];
        if (!diff)
            return &submissions[i];
    }
    return NULL;
}

static void transfer_challenge(int fd, const char *intent, const char *token)
{
    char response[512];
    int size = snprintf(response, sizeof response,
        "HTTP/1.1 303 See Other\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "Referrer-Policy: no-referrer\r\nContent-Length: 0\r\n"
        "Location: " ROOMS_ORIGIN "/#eden-bridge-challenge=%s.%s\r\n\r\n", intent, token);
    if (size > 0 && (size_t)size < sizeof response)
        send_all(fd, response, (size_t)size);
}

static int referred_by(const char *referer, const char *origin)
{
    size_t length = strlen(origin);
    return referer && !strncmp(referer, origin, length) &&
           (referer[length] == '/' || referer[length] == 0);
}

#define HTML "<!doctype html><html lang='es'><meta charset='utf-8'>" \
             "<meta name='viewport' content='width=device-width,initial-scale=1'>" \
             "<title>Eden Bridge PS5</title><style>" \
             "body{font:17px system-ui;background:#101b2c;color:white;max-width:32rem;margin:10vh auto;padding:1rem}" \
             "label,input,button{display:block;margin:.7rem 0}input,button{font:inherit;padding:.6rem}" \
             "a{color:#9cf}</style><h1>Eden Bridge PS5</h1>"

static const char save_script[] =
    "const form = document.getElementById('room');\n"
    "const status = document.getElementById('status');\n"
    "const button = form.querySelector('button');\n"
    "const back = document.getElementById('back');\n"
    "const transfer = form.dataset.transfer === '1';\n"
    "const previous = history.state;\n"
    "if (transfer && !previous) {\n"
    "  const match = /^#([a-f0-9]{32})\\.([a-f0-9]{32})$/.exec(location.hash);\n"
    "  if (!match) {\n"
    "    status.textContent = 'Autorización inválida. Volvé a iniciar el envío desde Eden Rooms.';\n"
    "    form.hidden = true;\n"
    "    back.hidden = false;\n"
    "  } else {\n"
    "    form.elements.intent.value = match[1];\n"
    "    form.elements.confirm.value = match[2];\n"
    "  }\n"
    "}\n"
    "if (previous && previous.confirm) {\n"
    "  form.elements.confirm.value = previous.confirm;\n"
    "  form.elements.nick.value = previous.nick;\n"
    "  if (transfer) form.elements.intent.value = previous.intent;\n"
    "  form.elements.nick.readOnly = true;\n"
    "}\n"
    "const opener = transfer ? null : window.opener;\n"
    "const roomsOrigin = '" ROOMS_ORIGIN "';\n"
    "let saving = false;\n"
    "async function submit(data) {\n"
    "  for (let attempt = 0; ; attempt++) {\n"
    "    const controller = new AbortController();\n"
    "    const timeout = setTimeout(() => controller.abort(), 12000);\n"
    "    try {\n"
    "      const response = await fetch(form.action, {method: 'POST', body: data, signal: controller.signal});\n"
    "      if ([408, 502, 503, 504].includes(response.status) && attempt === 0) continue;\n"
    "      const result = await response.json();\n"
    "      if (!response.ok || result.saved !== true) throw new Error(result.error);\n"
    "      return;\n"
    "    } catch (error) {\n"
    "      if (attempt || !(error instanceof TypeError || error.name === 'AbortError' ||\n"
    "                       error instanceof SyntaxError)) throw error;\n"
    "    } finally { clearTimeout(timeout); }\n"
    "  }\n"
    "}\n"
    "async function save() {\n"
    "  if (saving || !form.reportValidity()) return;\n"
    "  saving = true;\n"
    "  button.disabled = true;\n"
    "  back.hidden = true;\n"
    "  status.textContent = 'Guardando sala…';\n"
    "  try {\n"
    "    const data = new URLSearchParams(new FormData(form));\n"
    "    form.elements.nick.readOnly = true;\n"
    "    try { history.replaceState({confirm: data.get('confirm'), nick: data.get('nick'),\n"
    "      intent: data.get('intent')}, '', location.pathname + location.search); } catch {}\n"
    "    await submit(data);\n"
    "    form.hidden = true;\n"
    "    status.textContent = 'Sala guardada correctamente';\n"
    "    if (transfer) {\n"
    "      location.replace(roomsOrigin + '/#eden-bridge-saved=' + data.get('intent') + '.' + data.get('confirm'));\n"
    "      return;\n"
    "    }\n"
    "    back.hidden = false;\n"
    "    if (opener) opener.postMessage({type: 'eden-bridge:saved', host: data.get('host'),\n"
    "      port: data.get('port'), nick: data.get('nick')}, roomsOrigin);\n"
    "    window.close();\n"
    "  } catch (error) {\n"
    "    status.textContent = error instanceof TypeError || error.name === 'AbortError' || error instanceof SyntaxError ?\n"
    "      'No se recibió la confirmación del puente. Comprobá la conexión con la PS5.' :\n"
    "      error.message || 'El puente devolvió una respuesta inesperada.';\n"
    "    saving = false;\n"
    "    button.disabled = false;\n"
    "    button.textContent = 'Reintentar';\n"
    "    if (transfer) back.hidden = false;\n"
    "  }\n"
    "}\n"
    "form.addEventListener('submit', event => { event.preventDefault(); save(); });\n"
    "button.disabled = form.hidden;\n"
    "if (!form.hidden && form.dataset.auto === '1' && form.checkValidity()) {\n"
    "  save();\n"
    "} else if (!transfer && opener && form.checkValidity()) {\n"
    "  function authorize(event) {\n"
    "    if (event.origin !== roomsOrigin || event.source !== opener ||\n"
    "        event.data?.type !== 'eden-bridge:send-room') return;\n"
    "    const {host, port, nick} = event.data;\n"
    "    if (host !== form.elements.host.value || port !== form.elements.port.value ||\n"
    "        nick !== form.elements.nick.value) return;\n"
    "    window.removeEventListener('message', authorize);\n"
    "    clearTimeout(timeout);\n"
    "    back.querySelector('a').href = roomsOrigin + '/';\n"
    "    save();\n"
    "  }\n"
    "  const timeout = setTimeout(() => window.removeEventListener('message', authorize), 15000);\n"
    "  window.addEventListener('message', authorize);\n"
    "  opener.postMessage({type: 'eden-bridge:ready'}, roomsOrigin);\n"
    "}\n";

static int media(const char *header, const char *type)
{
    size_t size = strlen(type);
    return header && !strncasecmp(header, type, size) &&
           (header[size] == 0 || header[size] == ';');
}

static void config_failure(char *body, size_t size, const struct config_error *error,
                           int result, int saving)
{
    const char *target = error->target ? error->target : "configuracion";
    const char *message = !saving ? "No se pudo validar la configuración."
                        : result == -2 ? "Se reemplazó la configuración. Revisá el archivo y el respaldo."
                        : "No se guardó la sala.";
    fprintf(stderr, "Eden Bridge: %s %s/%s (errno=%d%s%s)\n",
            saving ? "guardado" : "validación", target, error->step, error->system_errno,
            error->system_errno ? ": " : "", error->system_errno ? strerror(error->system_errno) : "");
    snprintf(body, size, "{\"ok\":false,\"saved\":false,\"writeEnabled\":false,"
             "\"step\":\"%s\",\"target\":\"%s\",\"errno\":%d,"
             "\"error\":\"%s Etapa %s/%s (errno %d).\"}",
             error->step, target, error->system_errno, message, target, error->step, error->system_errno);
}

static void serve(int fd)
{
    char buffer[MAX_HEADER + MAX_BODY + 1], body[2048];
    struct request req = {0};
    struct room room = {0};
    const char *stage = "headers", *error = "invalid";
    if (read_request(fd, buffer, &req, &stage, &error)) {
        int timeout = !strcmp(error, "timeout");
        fprintf(stderr, "Eden Bridge %s HTTP: %s/%s\n", VERSION, stage, error);
        snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\","
                 "\"stage\":\"%s\",\"code\":\"%s\"}",
                 timeout ? "Tiempo de espera al leer la solicitud." : "invalid request", stage, error);
        reply(fd, timeout ? 408 : 400, body, JSON, 0);
        return;
    }

    struct sockaddr_in address = {0};
    socklen_t length = sizeof address;
    char ip[INET_ADDRSTRLEN], authority[64], self[80];
    if (getsockname(fd, (struct sockaddr *)&address, &length) ||
        !inet_ntop(AF_INET, &address.sin_addr, ip, sizeof ip))
        return;
    snprintf(authority, sizeof authority, "%s:%u", ip, (unsigned)ntohs(address.sin_port));
    snprintf(self, sizeof self, "http://%s", authority);
    int loopback = (ntohl(address.sin_addr.s_addr) >> 24) == 127;
    char local_authority[64];
    snprintf(local_authority, sizeof local_authority, "localhost:%u", (unsigned)ntohs(address.sin_port));
    int localhost = loopback && req.host && !strcmp(req.host, local_authority);
    if (localhost)
        snprintf(self, sizeof self, "http://%s", local_authority);

    int external = req.origin && !strcmp(req.origin, ORIGIN);
    int local = req.origin && !strcmp(req.origin, self);
    if (!req.host || (!localhost && strcmp(req.host, authority)) ||
        (req.origin && !external && !local)) {
        reply(fd, 403, "{\"ok\":false,\"error\":\"origin or host denied\"}", JSON, 0);
        return;
    }

    char *query = strchr(req.target, '?');
    if (query)
        *query++ = 0;
    int preview = !strcmp(req.target, "/v1/preview");
    int connection = !strcmp(req.target, "/v1/connection");
    int health = !strcmp(req.target, "/v1/health");
    int version = !strcmp(req.target, "/v1/version");
    if (!strcmp(req.method, "GET") && version && !query) {
        reply(fd, 200, "{\"ok\":true,\"service\":\"eden-bridge\",\"version\":\"" VERSION
              "\",\"platform\":\"" PLATFORM "\"}", JSON, external);
        return;
    }
    if (!strcmp(req.method, "GET") && health && !query) {
        reply(fd, 200, "{\"ok\":true,\"service\":\"eden-bridge\",\"writeEnabled\":false}", JSON, external);
        return;
    }
    if (!strcmp(req.method, "GET") && !strcmp(req.target, "/v1/config-check") && !query) {
        struct config_error error;
        int sections = config_update(config, NULL, &error);
        if (sections > 0)
            snprintf(body, sizeof body,
            "{\"ok\":true,\"configReadable\":true,\"multiplayerFieldsFound\":true,\"nicknameFieldsFound\":true,\"multiplayerSections\":%d,\"writeEnabled\":true}", sections);
        else
            config_failure(body, sizeof body, &error, sections, 0);
        reply(fd, sections > 0 ? 200 : 409, body, JSON, external);
        return;
    }
    if (!strcmp(req.method, "GET") && !strcmp(req.target, "/") && !query) {
        reply(fd, 200, HTML "<form action='/v1/prepare' method='get'>"
                         "<label>IP de la sala <input name='host' required></label>"
                         "<label>Puerto <input name='port' type='number' min='1' max='65535' required></label>"
                         "<label>Apodo <input name='nick' required maxlength='24' pattern='[A-Za-z0-9_\\-]{1,24}'></label>"
                         "<button>Revisar sala</button></form></html>", PAGE, 0);
        return;
    }
    if (!strcmp(req.method, "GET") && !strcmp(req.target, "/save.js") && !query) {
        reply(fd, 200, save_script, SCRIPT, 0);
        return;
    }
    int receive = !strcmp(req.target, "/v1/receive");
    int transfer = !strcmp(req.target, "/v1/transfer");
    if (!strcmp(req.method, "GET") && (receive || transfer || !strcmp(req.target, "/v1/prepare"))) {
        const char *intent = NULL;
        if (!query || form(query, &room, NULL, transfer ? &intent : NULL) || !room_valid(&room) ||
            ((receive || transfer) && !*room.nick) || (transfer && !valid_intent(intent))) {
            reply(fd, 400, HTML "<p>IP, puerto o apodo inválido.</p>"
                  "<form action='/'><button>Reintentar</button></form></html>", PAGE, 0);
            return;
        }
        if (transfer) {
            transfer_challenge(fd, intent, new_submission(&room, intent));
            return;
        }
        int from_rooms = req.referer && !strcmp(req.referer, ROOMS_ORIGIN "/");
        int automatic = receive || local || external || from_rooms || referred_by(req.referer, ORIGIN) ||
                        referred_by(req.referer, self);
        const char *message = !*room.nick ? "Ingresá tu apodo para continuar." :
                              automatic ? "Preparando el guardado…" :
                              "El navegador no identificó el origen del envío. Confirmá la sala.";
        snprintf(body, sizeof body, HTML "<p>Sala <strong>%s:%d</strong></p>"
            "<p id='status' role='status'>%s</p>"
            "<form id='room' data-auto='%d' data-transfer='%d' method='post' action='/v1/preview'>"
            "<input type='hidden' name='host' value='%s'><input type='hidden' name='port' value='%d'>"
            "<input type='hidden' name='confirm' value='%s'>%s"
            "<label>Apodo <input name='nick' maxlength='24' pattern='[A-Za-z0-9_\\-]{1,24}' value='%s' required></label>"
            "<button disabled>%s</button></form>"
            "<noscript><p>Activá JavaScript y volvé a abrir la sala.</p></noscript>"
            "<p id='back' hidden><a href='%s/'>%s</a></p>"
            "<script src='/save.js' defer></script></html>",
            room.host, room.port, message, automatic, receive, room.host, room.port,
            receive ? "" : new_submission(&room, NULL),
            receive ? "<input type='hidden' name='intent'>" : "", room.nick,
            receive ? "Reintentar" : "Confirmar sala",
            !receive && referred_by(req.referer, ORIGIN) ? ORIGIN : ROOMS_ORIGIN,
            receive ? "Iniciar un nuevo envío en Eden Rooms" : "Volver a Eden Rooms");
        reply(fd, 200, body, PAGE, 0);
        return;
    }
    if (!strcmp(req.method, "OPTIONS") && !query && (preview || connection || health || version)) {
        reply(fd, external ? 204 : 403, "", JSON, external);
        return;
    }
    if (strcmp(req.method, "POST") || query || (!preview && !connection)) {
        reply(fd, 404, "{\"ok\":false,\"error\":\"unknown route\"}", JSON, external);
        return;
    }
    if (preview && media(req.type, "application/x-www-form-urlencoded")) {
        const char *token = NULL, *intent = NULL;
        if (form(req.body, &room, &token, &intent) || !room_valid(&room) || !*room.nick) {
            reply(fd, 400, "{\"saved\":false,\"error\":\"IP, puerto o apodo inválido.\"}", JSON, 0);
            return;
        }
        struct submission *entry = local ? find_submission(token) : NULL;
        int64_t now = monotonic_ms();
        if (!entry || strcmp(entry->intent, intent ? intent : "") ||
            (*entry->intent && entry->result == 0 && (now < 0 || now >= entry->expires))) {
            reply(fd, 403, "{\"saved\":false,\"error\":\"Confirmación inválida o vencida. Volvé a abrir la sala.\"}", JSON, 0);
            return;
        }
        if (strcmp(entry->room.host, room.host) || entry->room.port != room.port ||
            ((*entry->intent || entry->result != 0) && strcmp(entry->room.nick, room.nick))) {
            reply(fd, 409, "{\"saved\":false,\"error\":\"La confirmación corresponde a otros datos. Volvé a abrir la sala.\"}", JSON, 0);
            return;
        }
        if (entry->result == 0) {
            struct config_error error;
            int saved = config_update(config, &room, &error);
            if (saved < 0) {
                config_failure(body, sizeof body, &error, saved, 1);
                if (saved == -2) {
                    entry->room = room;
                    entry->result = saved;
                }
                reply(fd, saved == -2 ? 500 : 409, body, JSON, 0);
                return;
            }
            entry->room = room;
            entry->result = saved;
            reply(fd, 200, "{\"ok\":true,\"saved\":true}", JSON, 0);
            snprintf(body, sizeof body, "Eden Bridge: %s - sala %s:%d guardada", room.nick, room.host, room.port);
            notify(body);
            return;
        }
        if (entry->result == -2) {
            reply(fd, 500, "{\"saved\":false,\"error\":\"Se reemplazó la configuración, pero no se pudo confirmar su persistencia. Revisá el archivo y el respaldo.\"}", JSON, 0);
            return;
        }
        fprintf(stderr, "Eden Bridge %s: confirmación repetida\n", VERSION);
        reply(fd, 200, "{\"ok\":true,\"saved\":true}", JSON, 0);
        return;
    }
    if (!external && !local) {
        reply(fd, 403, "{\"ok\":false,\"error\":\"origin denied\"}", JSON, 0);
        return;
    }
    if ((!media(req.type, "application/json") && !media(req.type, "text/plain")) ||
        json_room(req.body, &room) || !room_valid(&room)) {
        reply(fd, 400, "{\"ok\":false,\"error\":\"invalid room\"}", JSON, external);
        return;
    }
    if (connection && media(req.type, "text/plain")) {
        reply(fd, 409, "{\"ok\":false,\"saved\":false,\"error\":\"confirmation required at /v1/prepare\"}", JSON, external);
        return;
    }
    snprintf(body, sizeof body, "{\"ok\":true,\"received\":true,\"saved\":false,\"mode\":\"preview\","
        "\"host\":\"%s\",\"port\":%d,\"nick\":\"%s\",\"writeEnabled\":false}", room.host, room.port, room.nick);
    reply(fd, 200, body, JSON, external);
}

int main(int argc, char **argv)
{
    int port = PORT;
    uint32_t bind_address = INADDR_ANY;
#ifndef EDEN_PS5
    bind_address = INADDR_LOOPBACK;
    if (argc != 3 || decimal(argv[2], 5, &port) || port < 1 || port > 65535) {
        fprintf(stderr, "Uso: %s ARCHIVO_JSON PUERTO\n", argv[0]);
        return 2;
    }
    config = argv[1];
#else
    (void)argc;
    (void)argv;
#endif
    signal(SIGPIPE, SIG_IGN);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    struct sockaddr_in address = {0};
#ifdef EDEN_PS5
    address.sin_len = sizeof address;
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(bind_address);
    if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) ||
        bind(listener, (struct sockaddr *)&address, sizeof address) || listen(listener, 8)) {
        perror("Eden Bridge");
        notify("Eden Bridge: no se pudo iniciar el servidor HTTP");
        if (listener >= 0)
            close(listener);
        return 1;
    }
    fprintf(stderr, "Eden Bridge %s (%s)\n", VERSION, PLATFORM);
    notify_started(port);
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0 && errno == EINTR)
            continue;
        if (fd < 0) {
            perror("accept");
            break;
        }
        struct timeval timeout = { .tv_sec = 2 };
        if (!setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout))
            serve(fd);
        close(fd);
    }
    close(listener);
    return 1;
}

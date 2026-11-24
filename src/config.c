#include "bridge.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_CONFIG (1024 * 1024)
#define MAX_TOKENS 32768

struct token { int start, end, next; };
struct json {
    const char *text;
    int pos, count, capacity;
    struct token *tokens;
};

static void whitespace(struct json *j)
{
    j->pos += (int)strspn(j->text + j->pos, " \t\r\n");
}

static int string(struct json *j)
{
    const unsigned char *s = (const unsigned char *)j->text;
    for (j->pos++; s[j->pos]; j->pos++) {
        unsigned char c = s[j->pos];
        if (c == '"') {
            j->pos++;
            return 0;
        }
        if (c < 32)
            return -1;
        if (c == '\\') {
            c = s[++j->pos];
            if (c == 'u') {
                for (int i = 0; i < 4; i++)
                    if (!isxdigit(s[++j->pos]))
                        return -1;
            } else if (!c || !strchr("\"\\/bfnrt", c)) {
                return -1;
            }
        }
    }
    return -1;
}

static int number(struct json *j)
{
    const char *s = j->text;
    int p = j->pos;
    if (s[p] == '-')
        p++;
    if (!isdigit((unsigned char)s[p]))
        return -1;
    if (s[p++] != '0')
        p += (int)strspn(s + p, "0123456789");
    if (s[p] == '.') {
        if (!isdigit((unsigned char)s[++p]))
            return -1;
        p += (int)strspn(s + p, "0123456789");
    }
    if (s[p] == 'e' || s[p] == 'E') {
        p++;
        if (s[p] == '+' || s[p] == '-')
            p++;
        if (!isdigit((unsigned char)s[p]))
            return -1;
        p += (int)strspn(s + p, "0123456789");
    }
    j->pos = p;
    return 0;
}

static int value(struct json *j, int depth)
{
    whitespace(j);
    if (depth > 48 || j->count == j->capacity)
        return -1;
    struct token *token = &j->tokens[j->count++];
    token->start = j->pos;
    char c = j->text[j->pos];
    if (c == '[' || c == '{') {
        char end = c == '[' ? ']' : '}';
        j->pos++;
        whitespace(j);
        if (j->text[j->pos] != end) {
            for (;;) {
                if (c == '{') {
                    if (j->text[j->pos] != '"' || value(j, depth + 1))
                        return -1;
                    whitespace(j);
                    if (j->text[j->pos++] != ':')
                        return -1;
                }
                if (value(j, depth + 1))
                    return -1;
                whitespace(j);
                if (j->text[j->pos] != ',')
                    break;
                j->pos++;
                whitespace(j);
            }
        }
        if (j->text[j->pos++] != end)
            return -1;
    } else if (c == '"') {
        if (string(j))
            return -1;
    } else {
        const char *literal = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : NULL;
        if (literal) {
            size_t length = strlen(literal);
            if (strncmp(j->text + j->pos, literal, length))
                return -1;
            j->pos += (int)length;
        } else if (number(j)) {
            return -1;
        }
    }
    token->end = j->pos;
    token->next = j->count;
    return 0;
}

static int parse(struct json *j)
{
    if (value(j, 0))
        return -1;
    whitespace(j);
    return j->text[j->pos] || j->text[j->tokens[0].start] != '{' ? -1 : 0;
}

/* -1 denotes absent, -2 denotes an ambiguous or invalid object. */
static int member(const struct json *j, int object, const char *name)
{
    if (object < 0 || j->text[j->tokens[object].start] != '{')
        return -2;
    int found = -1;
    size_t length = strlen(name);
    for (int i = object + 1; i < j->tokens[object].next; i = j->tokens[i + 1].next) {
        const struct token *key = &j->tokens[i];
        const char *text = j->text + key->start + 1;
        size_t size = (size_t)(key->end - key->start - 2);
        if (memchr(text, '\\', size))
            return -2;
        if (size == length && !memcmp(text, name, size)) {
            if (found >= 0)
                return -2;
            found = i + 1;
        }
    }
    return found;
}

static int get_string(const struct json *j, int i, char *out, size_t capacity)
{
    if (i < 0 || j->text[j->tokens[i].start] != '"')
        return -1;
    const struct token *t = &j->tokens[i];
    size_t length = (size_t)(t->end - t->start - 2);
    if (length >= capacity || memchr(j->text + t->start + 1, '\\', length))
        return -1;
    memcpy(out, j->text + t->start + 1, length);
    out[length] = 0;
    return 0;
}

int json_room(const char *body, struct room *room)
{
    struct token tokens[128];
    struct json j = { .text = body, .capacity = 128, .tokens = tokens };
    if (parse(&j) || get_string(&j, member(&j, 0, "host"), room->host, sizeof room->host))
        return -1;
    int nick = member(&j, 0, "nick");
    int nickname = member(&j, 0, "nickname");
    if (nick == -2 || nickname == -2 || (nick >= 0 && nickname >= 0))
        return -1;
    if (nick >= 0 || nickname >= 0) {
        if (get_string(&j, nick >= 0 ? nick : nickname, room->nick, sizeof room->nick) || !room->nick[0])
            return -1;
    }
    int port = member(&j, 0, "port");
    if (port < 0)
        return -1;
    const char *start = body + tokens[port].start;
    size_t size = (size_t)(tokens[port].end - tokens[port].start);
    if (!size || size > 5 || strspn(start, "0123456789") != size)
        return -1;
    room->port = atoi(start);
    return 0;
}

struct replacement { struct token token; const char *text; };

static int replacements(const struct json *j, int object, struct replacement *out,
                        const char *host, const char *port, const char *nick)
{
    int section = member(j, object, "multiplayer");
    int h = member(j, section, "host");
    int p = member(j, section, "port");
    int n = member(j, section, "nickname");
    int root_nick = member(j, object, "nickname");
    if (n == -2 || root_nick == -2 || (n >= 0 && root_nick >= 0))
        return -1;
    if (n == -1)
        n = root_nick;
    if (h < 0 || p < 0 || n < 0 ||
        j->text[j->tokens[h].start] != '"' || j->text[j->tokens[n].start] != '"')
        return -1;
    const struct token *old_port = &j->tokens[p];
    if (strspn(j->text + old_port->start, "0123456789") != (size_t)(old_port->end - old_port->start))
        return -1;
    out[0] = (struct replacement){ j->tokens[h], host };
    out[1] = (struct replacement){ j->tokens[p], port };
    out[2] = (struct replacement){ j->tokens[n], nick };
    return 0;
}

static int compare(const void *a, const void *b)
{
    const struct replacement *left = a, *right = b;
    return left->token.start - right->token.start;
}

static int write_all(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t count = write(fd, data, length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (count == 0)
                errno = EIO;
            return -1;
        }
        data += count;
        length -= (size_t)count;
    }
    return 0;
}

static const char *stage(char *name, const char *data, size_t length, const struct stat *original)
{
    int fd = mkstemp(name);
    if (fd < 0)
        return "mkstemp";
    struct stat created;
    const char *operation = NULL;
    if (fstat(fd, &created))
        operation = "fstat";
    else if (created.st_uid != original->st_uid || created.st_gid != original->st_gid) {
        if (fchown(fd, original->st_uid, original->st_gid))
            operation = "fchown";
        else if (fstat(fd, &created))
            operation = "fstat";
        else if (created.st_uid != original->st_uid || created.st_gid != original->st_gid) {
            errno = EPERM;
            operation = "verificar-propietario";
        }
    }
    if (!operation && write_all(fd, data, length))
        operation = "write";
    else if (!operation && fchmod(fd, original->st_mode & 0777))
        operation = "fchmod";
    else if (!operation && fsync(fd))
        operation = "fsync";
    int saved_errno = errno;
    if (close(fd) && !operation) {
        operation = "close";
        saved_errno = errno;
    }
    if (operation)
        unlink(name);
    errno = saved_errno;
    return operation;
}

/* A null room checks the supported schema without writing. */
int config_update(const char *path, const struct room *room, struct config_error *error)
{
    struct stat original, current;
    char backup[1024], temp[1024], backup_temp[1024], directory[1024];
    *error = (struct config_error){ .step = "ruta-demasiado-larga", .system_errno = 0 };
    if (strlen(path) > 980)
        return -1;
    snprintf(backup, sizeof backup, "%s.edenrooms.bak", path);
    snprintf(temp, sizeof temp, "%s.edenrooms.tmp.XXXXXX", path);
    snprintf(backup_temp, sizeof backup_temp, "%s.edenrooms.bak.XXXXXX", path);
    strcpy(directory, path);
    char *slash = strrchr(directory, '/');
    if (!slash)
        strcpy(directory, ".");
    else
        slash[slash == directory ? 1 : 0] = 0;

    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        *error = (struct config_error){ .step = "config.open", .system_errno = errno };
        return -1;
    }
    int result = -1, dir = -1, temp_ready = 0, backup_ready = 0;
    char *input = NULL, *output = NULL;
    struct token *tokens = NULL;
    if (fstat(fd, &original)) {
        *error = (struct config_error){ .step = "config.fstat", .system_errno = errno };
        goto done;
    }
    *error = (struct config_error){ .step = "config.tipo-o-tamano", .system_errno = 0 };
    if (!S_ISREG(original.st_mode) || original.st_size <= 0 || original.st_size > MAX_CONFIG)
        goto done;
    if (flock(fd, (room ? LOCK_EX : LOCK_SH) | LOCK_NB)) {
        *error = (struct config_error){ .step = "config.flock", .system_errno = errno };
        goto done;
    }
    size_t size = (size_t)original.st_size;
    input = malloc(size + 1);
    tokens = malloc(MAX_TOKENS * sizeof *tokens);
    *error = (struct config_error){ .step = "memoria", .system_errno = 0 };
    if (!input || !tokens)
        goto done;
    size_t used = 0;
    while (used < size) {
        ssize_t count = read(fd, input + used, size - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            *error = (struct config_error){ .step = "config.read", .system_errno = count < 0 ? errno : EIO };
            goto done;
        }
        used += (size_t)count;
    }
    *error = (struct config_error){ .step = "json-invalido", .system_errno = 0 };
    if (memchr(input, 0, size))
        goto done;
    input[size] = 0;

    struct json j = { .text = input, .capacity = MAX_TOKENS, .tokens = tokens };
    struct replacement edits[6];
    char host[20] = "", port[8] = "", nick[30] = "";
    if (room) {
        snprintf(host, sizeof host, "\"%s\"", room->host);
        snprintf(port, sizeof port, "%d", room->port);
        snprintf(nick, sizeof nick, "\"%s\"", room->nick);
    }
    if (parse(&j))
        goto done;
    *error = (struct config_error){ .step = "multiplayer-general", .system_errno = 0 };
    if (replacements(&j, 0, edits, host, port, nick))
        goto done;

    int count = 3;
    int profiles = member(&j, 0, "profiles");
    int active = member(&j, 0, "activeProfile");
    int settings = member(&j, 0, "profile_settings");
    if (profiles != -1 || active != -1 || settings != -1) {
        *error = (struct config_error){ .step = "selector-de-perfil", .system_errno = 0 };
        char name[128];
        if (settings != -1) {
            if (active != -1 || settings < 0)
                goto done;
            active = member(&j, profiles, "current");
        } else {
            settings = profiles;
        }
        if (get_string(&j, active, name, sizeof name) || !name[0])
            goto done;
        *error = (struct config_error){ .step = "perfil-activo-ausente-o-ambiguo", .system_errno = 0 };
        int selected = member(&j, settings, name);
        if (selected < 0)
            goto done;
        *error = (struct config_error){ .step = "multiplayer-perfil-activo", .system_errno = 0 };
        if (replacements(&j, selected, edits + 3, host, port, nick))
            goto done;
        count = 6;
    }
    if (!room) {
        result = count / 3;
        goto done;
    }

    qsort(edits, (size_t)count, sizeof *edits, compare);
    *error = (struct config_error){ .step = "memoria", .system_errno = 0 };
    output = malloc(size + 256);
    if (!output)
        goto done;
    size_t cursor = 0, length = 0;
    for (int i = 0; i < count; i++) {
        size_t unchanged = (size_t)edits[i].token.start - cursor;
        memcpy(output + length, input + cursor, unchanged);
        length += unchanged;
        size_t replacement_length = strlen(edits[i].text);
        memcpy(output + length, edits[i].text, replacement_length);
        length += replacement_length;
        cursor = (size_t)edits[i].token.end;
    }
    memcpy(output + length, input + cursor, size - cursor);
    length += size - cursor;
    *error = (struct config_error){ .step = "config.tamano-de-salida", .system_errno = 0 };
    if (length > MAX_CONFIG)
        goto done;
    dir = open(directory, O_RDONLY | O_DIRECTORY);
    if (dir < 0) {
        *error = (struct config_error){ .step = "directorio.open", .system_errno = errno };
        goto done;
    }
    const char *operation = stage(temp, output, length, &original);
    if (operation) {
        *error = (struct config_error){ .step = operation, .system_errno = errno, .target = "temporal-config" };
        goto done;
    }
    temp_ready = 1;
    operation = stage(backup_temp, input, size, &original);
    if (operation) {
        *error = (struct config_error){ .step = operation, .system_errno = errno, .target = "temporal-respaldo" };
        goto done;
    }
    backup_ready = 1;
    if (lstat(path, &current)) {
        *error = (struct config_error){ .step = "config.lstat", .system_errno = errno };
        goto done;
    }
    *error = (struct config_error){ .step = "config.cambio-concurrente", .system_errno = 0 };
    if (current.st_dev != original.st_dev || current.st_ino != original.st_ino ||
        current.st_size != original.st_size ||
        current.st_mtim.tv_sec != original.st_mtim.tv_sec ||
        current.st_mtim.tv_nsec != original.st_mtim.tv_nsec ||
        current.st_ctim.tv_sec != original.st_ctim.tv_sec ||
        current.st_ctim.tv_nsec != original.st_ctim.tv_nsec)
        goto done;
    if (rename(backup_temp, backup)) {
        *error = (struct config_error){ .step = "respaldo.rename", .system_errno = errno };
        goto done;
    }
    backup_ready = 0;
    if (fsync(dir)) {
        *error = (struct config_error){ .step = "respaldo.fsync-directorio", .system_errno = errno };
        goto done;
    }
    if (rename(temp, path)) {
        *error = (struct config_error){ .step = "config.rename", .system_errno = errno };
        goto done;
    }
    temp_ready = 0;
    if (fsync(dir)) {
        *error = (struct config_error){ .step = "config.fsync-directorio", .system_errno = errno };
        result = -2;
        goto done;
    }
    result = count / 3;

done:
    if (backup_ready)
        unlink(backup_temp);
    if (temp_ready)
        unlink(temp);
    if (dir >= 0)
        close(dir);
    close(fd);
    free(tokens);
    free(output);
    free(input);
    return result;
}

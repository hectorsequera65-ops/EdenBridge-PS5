#ifndef EDEN_BRIDGE_H
#define EDEN_BRIDGE_H

struct room {
    char host[16];
    char nick[25];
    int port;
};

int json_room(const char *body, struct room *room);
struct config_error {
    const char *step;
    int system_errno;
    const char *target;
};

int config_update(const char *path, const struct room *room, struct config_error *error);

#endif

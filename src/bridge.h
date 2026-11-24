#ifndef EDEN_BRIDGE_H
#define EDEN_BRIDGE_H

struct room {
    char host[16];
    char nick[25];
    int port;
};

int json_room(const char *body, struct room *room);
int config_update(const char *path, const struct room *room);

#endif

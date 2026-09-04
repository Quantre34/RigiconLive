#ifndef RGCN_H
#define RGCN_H

#include <stdint.h>
#include <stddef.h>

#define RGCN_APP_NAME       "Rigicon Live"
#define RGCN_COMPANY        "Rigicon Inc."
#define RGCN_VERSION        "1.0.0"

#define RGCN_DEFAULT_PORT   7444
#define RGCN_MIN_PORT       1024
#define RGCN_MAX_PORT       65535

#define RGCN_MAX_NICK       32
#define RGCN_MAX_TEXT       512
#define RGCN_MAX_PACKET     1400

#define RGCN_MSG_CHAT       1
#define RGCN_MSG_JOIN       2
#define RGCN_MSG_LEAVE      3
#define RGCN_MSG_PING       4

typedef struct {
    uint8_t  type;
    uint64_t id;
    uint64_t station;
    uint64_t ts_ms;
    char     user[RGCN_MAX_NICK];
    char     text[RGCN_MAX_TEXT];
} rgcn_msg_t;

uint64_t rgcn_now_ms(void);

#endif

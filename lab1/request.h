#ifndef REQUEST_H
#define REQUEST_H

#include <stdint.h>

struct Request {
    int32_t priority;
    int32_t command;
    int32_t value;
};

enum Command : int32_t {
    COMMAND_NOP,
    COMMAND_WAIT,
    COMMAND_ECHO,
    COMMAND_NEG,
    COMMAND_TIMER,
};

struct Answer {
    int32_t type;
    int32_t value;
};

#endif // REQUEST_H
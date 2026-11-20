#ifndef BBS_H
#define BBS_H

#include <stdint.h>
#include <devctl.h>

namespace bbs {

struct BBSParams
{
    uint32_t seed;
    uint32_t p;
    uint32_t q;
};

}

#define BBS_CMD_SET_PARAMS   __DIOT(_DCMD_MISC, 1, bbs::BBSParams)

// __DION (DEVDIR_NONE) не принимает данные ответа в буфер клиента, поэтому __DIOF.
#define BBS_CMD_GET_ELEMENT  __DIOF(_DCMD_MISC, 2, uint32_t)

#endif // BBS_H

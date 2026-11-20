#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <devctl.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>

#include "bbs.h"

static resmgr_connect_funcs_t    connect_funcs;
static resmgr_io_funcs_t         io_funcs;
static iofunc_attr_t             attr;

namespace {

struct Generator
{
    bool     ready;
    uint32_t m;
    uint32_t x;
};

Generator g_gen = { false, 0, 0 };

bool isPrime(uint32_t n)
{
    if (n < 2) return false;
    if (n % 2 == 0) return n == 2;
    for (uint32_t d = 3; (uint64_t)d * d <= n; d += 2)
        if (n % d == 0) return false;
    return true;
}

uint32_t gcd(uint32_t a, uint32_t b)
{
    while (b != 0) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

int validate(const bbs::BBSParams &prm)
{
    if (!isPrime(prm.p) || !isPrime(prm.q)) {
        fprintf(stderr, "cryptobbs: p и q должны быть простыми (p=%u, q=%u)\n", prm.p, prm.q);
        return EINVAL;
    }
    if (prm.p == prm.q) {
        fprintf(stderr, "cryptobbs: p и q должны различаться\n");
        return EINVAL;
    }

    // p = q = 3 (mod 4)
    if (prm.p % 4 != 3 || prm.q % 4 != 3) {
        fprintf(stderr, "cryptobbs: p и q должны быть ≡ 3 (mod 4)\n");
        return EINVAL;
    }
    uint64_t m = (uint64_t)prm.p * prm.q;
    if (m > UINT32_MAX) {
        fprintf(stderr, "cryptobbs: M = p*q не помещается в 32 бита\n");
        return EOVERFLOW;
    }
    uint32_t x0 = prm.seed % (uint32_t)m;
    if (x0 < 2 || gcd(x0, (uint32_t)m) != 1) {
        fprintf(stderr, "cryptobbs: seed должен быть взаимно прост с M и seed mod M > 1\n");
        return EINVAL;
    }
    return EOK;
}

// x_{n+1} = x_n^2 mod M (младший бит)
uint32_t nextElement()
{
    g_gen.x = (uint32_t)(((uint64_t)g_gen.x * g_gen.x) % g_gen.m);
    return g_gen.x & 1u;
}

} // namespace

int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT)
        return status;

    switch (msg->i.dcmd) {

    case BBS_CMD_SET_PARAMS: {
        if (msg->i.nbytes < sizeof(bbs::BBSParams))
            return EINVAL;

        void *data = _DEVCTL_DATA(msg->i);
        bbs::BBSParams *prm = reinterpret_cast<bbs::BBSParams *>(data);

        int err = validate(*prm);
        if (err != EOK)
            return err;

        g_gen.m = prm->p * prm->q;
        g_gen.x = prm->seed % g_gen.m;
        g_gen.ready = true;
        printf("cryptobbs: параметры установлены: seed=%u p=%u q=%u M=%u\n",
               prm->seed, prm->p, prm->q, g_gen.m);

        memset(&msg->o, 0, sizeof(msg->o));
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }

    case BBS_CMD_GET_ELEMENT: {
        if (!g_gen.ready)
            return EPERM;

        uint32_t y = nextElement();

        memset(&msg->o, 0, sizeof(msg->o));
        *reinterpret_cast<uint32_t *>(_DEVCTL_DATA(msg->o)) = y;
        msg->o.nbytes = sizeof(y);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(y));
    }

    default:
        return ENOSYS;
    }
}

int main(int argc, char **argv)
{
    resmgr_attr_t        resmgr_attr;
    dispatch_t           *dpp;
    dispatch_context_t   *ctp;
    int                  id;

    /* initialize dispatch interface */
    if((dpp = dispatch_create()) == NULL) {
        fprintf(stderr,
                "%s: Unable to allocate dispatch handle.\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    memset(&resmgr_attr, 0, sizeof resmgr_attr);
    resmgr_attr.nparts_max = 1;
    resmgr_attr.msg_max_size = 2048;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);

    io_funcs.devctl = io_devctl;

    iofunc_attr_init(&attr, S_IFNAM | 0666, 0, 0);

    id = resmgr_attach(
            dpp,            /* dispatch handle        */
            &resmgr_attr,   /* resource manager attrs */
            "/dev/cryptobbs",  /* device name            */
            _FTYPE_ANY,     /* open type              */
            0,              /* flags                  */
            &connect_funcs, /* connect routines       */
            &io_funcs,      /* I/O routines           */
            &attr);         /* handle                 */
    if(id == -1) {
        fprintf(stderr, "%s: Unable to attach name.\n", argv[0]);
        return EXIT_FAILURE;
    }

    printf("cryptobbs: started, /dev/cryptobbs\n");

    ctp = dispatch_context_alloc(dpp);

    while(1) {
        if((ctp = dispatch_block(ctp)) == NULL) {
            fprintf(stderr, "block error\n");
            return EXIT_FAILURE;
        }
        dispatch_handler(ctp);
    }
    return EXIT_SUCCESS;
}

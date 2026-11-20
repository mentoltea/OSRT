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

#include "otp_dev.h"

static resmgr_connect_funcs_t    connect_funcs;
static resmgr_io_funcs_t         io_funcs;
static iofunc_attr_t             attr;

namespace {

int validate(const otp::OtpContext &ctx)
{
    const otp::LcgParams &p = ctx.lcg;
    if (p.m < 2) {
        fprintf(stderr, "otp: m must be >= 2 (m=%u)\n", p.m);
        return EINVAL;
    }
    if (p.a == 0 || p.a >= p.m) {
        fprintf(stderr, "otp: a must be in [1, m) (a=%u)\n", p.a);
        return EINVAL;
    }
    if (p.c >= p.m || p.seed >= p.m) {
        fprintf(stderr, "otp: c and seed must be < m (c=%u, seed=%u)\n", p.c, p.seed);
        return EINVAL;
    }
    if (ctx.size == 0 || ctx.size > otp::kMaxTextSize) {
        fprintf(stderr, "otp: size must be in [1, %u] (size=%u)\n", otp::kMaxTextSize, ctx.size);
        return EINVAL;
    }
    return EOK;
}

void cypher(otp::OtpContext &ctx)
{
    const otp::LcgParams &p = ctx.lcg;
    uint32_t x = p.seed;
    for (uint32_t i = 0; i < ctx.size; ++i) {
        x = (uint32_t)(((uint64_t)p.a * x + p.c) % p.m);
        uint8_t key = (uint8_t)(x & 0xFF);
        ctx.text[i] ^= key;
    }
}

} // namespace

int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT)
        return status;

    switch (msg->i.dcmd) {

    case CYPHER_TEXT: {
        if (msg->i.nbytes < sizeof(otp::OtpContext))
            return EINVAL;

        void *data = _DEVCTL_DATA(msg->i);
        otp::OtpContext *ctx = reinterpret_cast<otp::OtpContext *>(data);

        int err = validate(*ctx);
        if (err != EOK)
            return err;

        cypher(*ctx);
        printf("otp: processed %u bytes\n", ctx->size);

        void *out = _DEVCTL_DATA(msg->o);
        if (out != data)
            memmove(out, data, sizeof(otp::OtpContext));
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.nbytes = sizeof(otp::OtpContext);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(otp::OtpContext));
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

    if((dpp = dispatch_create()) == NULL) {
        fprintf(stderr, "%s: Unable to allocate dispatch handle.\n", argv[0]);
        return EXIT_FAILURE;
    }

    memset(&resmgr_attr, 0, sizeof resmgr_attr);
    resmgr_attr.nparts_max = 1;
    resmgr_attr.msg_max_size = 4096;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);

    io_funcs.devctl = io_devctl;

    iofunc_attr_init(&attr, S_IFNAM | 0666, 0, 0);

    id = resmgr_attach(dpp, &resmgr_attr, OTP_DEVICE_PATH, _FTYPE_ANY, 0,
                       &connect_funcs, &io_funcs, &attr);
    if(id == -1) {
        fprintf(stderr, "%s: Unable to attach name.\n", argv[0]);
        return EXIT_FAILURE;
    }

    printf("otp: started, %s\n", OTP_DEVICE_PATH);

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

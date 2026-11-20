#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <devctl.h>

#include <fstream>
#include <iostream>

#include "otp_dev.h"

namespace {

const uint32_t kSeed = 4242;
const uint32_t kA    = 1103515245;
const uint32_t kC    = 12345;
const uint32_t kM    = 2147483648u;

otp::OtpContext g_ctx;

}

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input file> <output file> [pause_sec]" << std::endl;
        return EXIT_FAILURE;
    }
    const char *inPath  = argv[1];
    const char *outPath = argv[2];
    int pauseSec = (argc > 3) ? atoi(argv[3]) : 0;

    std::ifstream in(inPath, std::ios::in | std::ios::binary);
    if (!in) {
        std::cerr << "E: unable to open input file " << inPath << std::endl;
        return EXIT_FAILURE;
    }
    memset(&g_ctx, 0, sizeof(g_ctx));
    in.read(reinterpret_cast<char *>(g_ctx.text), otp::kMaxTextSize);
    std::streamsize got = in.gcount();
    if (in.peek() != std::ifstream::traits_type::eof()) {
        std::cerr << "E: input file is larger than " << otp::kMaxTextSize << " bytes" << std::endl;
        return EXIT_FAILURE;
    }
    in.close();
    if (got <= 0) {
        std::cerr << "E: input file is empty" << std::endl;
        return EXIT_FAILURE;
    }

    g_ctx.size     = (uint32_t)got;
    g_ctx.lcg.seed = kSeed;
    g_ctx.lcg.a    = kA;
    g_ctx.lcg.c    = kC;
    g_ctx.lcg.m    = kM;

    int fd = open(OTP_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        std::cerr << "E: unable to open server connection: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    int err = devctl(fd, CYPHER_TEXT, &g_ctx, sizeof(g_ctx), NULL);
    if (err != EOK) {
        std::cerr << "E: CYPHER_TEXT failed: " << strerror(err) << std::endl;
        close(fd);
        return EXIT_FAILURE;
    }

    if (pauseSec > 0) {
        std::cerr << "I: pid " << getpid() << ", sleeping " << pauseSec << " s..." << std::endl;
        sleep(pauseSec);
    }

    close(fd);

    std::ofstream out(outPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "E: unable to open output file " << outPath << std::endl;
        return EXIT_FAILURE;
    }
    out.write(reinterpret_cast<const char *>(g_ctx.text), g_ctx.size);
    if (!out) {
        std::cerr << "E: write error" << std::endl;
        return EXIT_FAILURE;
    }
    out.close();

    std::cerr << "I: " << g_ctx.size << " bytes written to " << outPath << std::endl;
    return EXIT_SUCCESS;
}

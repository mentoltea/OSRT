// cryptobbs-client — клиент АР /dev/cryptobbs
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <devctl.h>

#include <iostream>
#include <vector>

#include "bbs.h"

namespace {

volatile sig_atomic_t g_stop = 0;

void onSigint(int)
{
    g_stop = 1;
}

const size_t kRingSize = 1024;

}

int main(int argc, char **argv)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        std::cerr << "E: sigaction: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    int fd = open("/dev/cryptobbs", O_RDWR);
    if (fd < 0) {
        std::cerr << "E: unable to open server connection: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    bbs::BBSParams params;
    params.seed = 866;
    params.p = 3;
    params.q = 263;

    int err = devctl(fd, BBS_CMD_SET_PARAMS, &params, sizeof(params), NULL);
    if (err != EOK) {
        std::cerr << "E: set params failed: " << strerror(err) << std::endl;
        close(fd);
        return EXIT_FAILURE;
    }

    std::vector<uint32_t> ring(kRingSize, 0);
    size_t head = 0;
    unsigned long long total = 0;
    int rc = EXIT_SUCCESS;

    while (!g_stop) {
        uint32_t value = 0;
        err = devctl(fd, BBS_CMD_GET_ELEMENT, &value, sizeof(value), NULL);
        if (err == EINTR)
            break;
        if (err != EOK) {
            std::cerr << "E: get element failed: " << strerror(err) << std::endl;
            rc = EXIT_FAILURE;
            break;
        }
        ring[head] = value;
        head = (head + 1) % kRingSize;
        ++total;
    }

    size_t count = total < kRingSize ? (size_t)total : kRingSize;
    size_t start = total < kRingSize ? 0 : head;
    for (size_t i = 0; i < count; ++i) {
        std::cout << ring[(start + i) % kRingSize];
        if ((i + 1) % 64 == 0)
            std::cout << '\n';
    }
    std::cout << std::endl;
    std::cerr << "I: total elements received: " << total << std::endl;

    close(fd);
    return rc;
}

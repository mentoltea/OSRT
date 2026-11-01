#include <iostream>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <poll.h>

#include "common.h"
#include "request.h"

// efd_to_send == -1: отправить запрос без дескриптора
ssize_t send_request_with_fd(int socket_fd, const Request* req, int efd_to_send) {
    struct msghdr msg = {0};
    struct iovec iov[1];

    iov[0].iov_base = const_cast<Request*>(req);
    iov[0].iov_len = sizeof(Request);
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    char control_buf[CMSG_SPACE(sizeof(int))];
    if (efd_to_send != -1) {
        std::memset(control_buf, 0, sizeof(control_buf));
        msg.msg_control = control_buf;
        msg.msg_controllen = sizeof(control_buf);

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &efd_to_send, sizeof(int));
    }

    return sendmsg(socket_fd, &msg, MSG_NOSIGNAL);
}

// ждёт ответ на запрос
// false, если соединение с сервером потеряно.
bool wait_answer(int client_fd, int efd) {
    struct pollfd fds[2];

    fds[0].fd = client_fd;
    fds[0].events = POLLIN;

    fds[1].fd = efd;
    fds[1].events = POLLIN;

    while (true) {
        int poll_count = poll(fds, 2, -1);
        if (poll_count == -1) {
            perror("[Client] poll error");
            return false;
        }

        if (fds[1].revents & POLLIN) {
            uint64_t counter = 0;
            ssize_t res = read(efd, &counter, sizeof(counter));
            if (res == sizeof(counter)) {
                std::cout << "\n[Client] >>> ASYNC TIMER NOTIFICATION RECEIVED! <<<" << std::endl;
                std::cout << "[Client] Signal count in eventfd: " << counter << std::endl << std::endl;
            } else {
                perror("[Client] eventfd read error");
            }
        }

        if (fds[0].revents & POLLIN) {
            Answer ans{};
            ssize_t bytes_read = read(client_fd, &ans, sizeof(ans));
            if (bytes_read <= 0) {
                std::cerr << "[Client] Server disconnected or failed to return answer." << std::endl;
                return false;
            }
            std::cout << "[Client] Received Answer { type: " << ans.type
                      << ", value: " << ans.value << " }" << std::endl;
            return true;
        }

        if (fds[0].revents & (POLLHUP | POLLERR)) {
            std::cerr << "[Client] Connection closed by server." << std::endl;
            return false;
        }
    }
}

int main(int argc, char** argv) {
    // команды из аргументов (пары <command> <value>)
    // если их нет — из stdin
    if (argc < 2 || argc % 2 != 0) {
        std::cerr << "Usage: " << argv[0] << " <priority> [<command> <value> ...]\n"
                  << "       without commands in arguments, '<command> <value>' lines are read from stdin\n";
        return 1;
    }
    bool from_stdin = (argc == 2);
    int next_arg = 2;

    int efd = eventfd(0, EFD_NONBLOCK);
    if (efd == -1) {
        perror("[Client] eventfd error");
        return 1;
    }

    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd == -1) {
        perror("[Client] socket error");
        close(efd);
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    std::cout << "[Client] Connecting to " << socket_path << "..." << std::endl;

    if (connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
        perror("[Client] connect error");
        close(client_fd);
        close(efd);
        return 1;
    }

    std::cout << "[Client] Connected successfully" << std::endl;

    Request req{};
    req.priority = std::atoi(argv[1]);
    bool efd_sent = false;
    int status = 0;

    while (true) {
        if (from_stdin) {
            if (!(std::cin >> req.command >> req.value)) break;
        } else {
            if (next_arg >= argc) break;
            req.command = std::atoi(argv[next_arg]);
            req.value   = std::atoi(argv[next_arg + 1]);
            next_arg += 2;
        }

        int efd_to_send = efd_sent ? -1 : efd;

        std::cout << "[Client] Sending Request { priority: " << req.priority
                  << ", command: " << req.command
                  << ", value: " << req.value << " }";
        if (efd_to_send != -1) std::cout << " with eventfd #" << efd;
        std::cout << "..." << std::endl;

        ssize_t bytes_sent = send_request_with_fd(client_fd, &req, efd_to_send);
        if (bytes_sent <= 0) {
            perror("[Client] sendmsg error");
            status = 1;
            break;
        }
        efd_sent = true;

        std::cout << "[Client] Request sent. Waiting for async notifications and final answer..." << std::endl;

        if (!wait_answer(client_fd, efd)) {
            status = 1;
            break;
        }
    }

    close(client_fd);
    close(efd);
    return status;
}
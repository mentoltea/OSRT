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

ssize_t send_request_with_fd(int socket_fd, const Request* req, int efd_to_send) {
    struct msghdr msg = {0};
    struct iovec iov[1];

    iov[0].iov_base = const_cast<Request*>(req);
    iov[0].iov_len = sizeof(Request);
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    char control_buf[CMSG_SPACE(sizeof(int))];
    std::memset(control_buf, 0, sizeof(control_buf));
    msg.msg_control = control_buf;
    msg.msg_controllen = sizeof(control_buf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &efd_to_send, sizeof(int));

    return sendmsg(socket_fd, &msg, 0);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <priority> <command> <value>\n";
        return 1;
    }

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
    req.command  = std::atoi(argv[2]);
    req.value    = std::atoi(argv[3]);

    std::cout << "[Client] Sending Request { priority: " << req.priority 
              << ", command: " << req.command 
              << ", value: " << req.value << " } with eventfd #" << efd << "..." << std::endl;

    ssize_t bytes_sent = send_request_with_fd(client_fd, &req, efd);
    if (bytes_sent <= 0) {
        perror("[Client] sendmsg error");
        close(client_fd);
        close(efd);
        return 1;
    }

    std::cout << "[Client] Request sent. Waiting for async notifications and final answer..." << std::endl;

    struct pollfd fds[2];
    
    fds[0].fd = client_fd;
    fds[0].events = POLLIN;

    fds[1].fd = efd;
    fds[1].events = POLLIN;

    bool answer_received = false;

    while (!answer_received) {
        int poll_count = poll(fds, 2, -1);
        if (poll_count == -1) {
            perror("[Client] poll error");
            break;
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
            } else {
                std::cout << "[Client] Received Answer { type: " << ans.type 
                          << ", value: " << ans.value << " }" << std::endl;
            }
            answer_received = true;
        }

        if (fds[0].revents & (POLLHUP | POLLERR)) {
            if (!answer_received) {
                std::cerr << "[Client] Connection closed by server." << std::endl;
            }
            break;
        }
    }

    close(client_fd);
    close(efd);
    return 0;
}
#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <csignal>
#include <sys/eventfd.h>

#include "request.h"
#include "common.h"

#include <queue>
#include <mutex>
#include <vector>
#include <algorithm>

#include <thread>
#include <semaphore.h>
#include <chrono>

int worker_count = 2; 
int server_fd = -1;

struct ClientRequest {
    Request request;
    int client_fd;
    int eventfd_fd;

    bool operator<(const ClientRequest& other) const {
        return this->request.priority < other.request.priority;
    }
};

std::priority_queue<ClientRequest> client_queue;
std::mutex client_queue_mutex;
sem_t client_queue_semaphore;

std::vector<int> active_eventfds;
std::mutex active_eventfds_mutex;

void notify_client(int efd) {
    if (efd < 0) return;
    uint64_t val = 1;
    if (write(efd, &val, sizeof(val)) != sizeof(val)) {
        perror("[Server] Failed to write to eventfd");
    } else {
        std::cout << "[Server] Async notification sent to eventfd #" << efd << std::endl;
    }
}

void cleanup_client_resources(int client_fd, int eventfd_fd) {
    if (eventfd_fd != -1) {
        {
            std::lock_guard<std::mutex> lock(active_eventfds_mutex);
            auto it = std::find(active_eventfds.begin(), active_eventfds.end(), eventfd_fd);
            if (it != active_eventfds.end()) {
                active_eventfds.erase(it);
            }
        }
        close(eventfd_fd);
    }
    if (client_fd != -1) {
        close(client_fd);
    }
}

// Приём Request и eventfd
ssize_t read_request_with_fd(int client_fd, Request* req, int* out_eventfd) {
    struct msghdr msg = {0};
    struct iovec iov[1];

    iov[0].iov_base = req;
    iov[0].iov_len = sizeof(Request);
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    char control_buf[CMSG_SPACE(sizeof(int))];
    std::memset(control_buf, 0, sizeof(control_buf));
    msg.msg_control = control_buf;
    msg.msg_controllen = sizeof(control_buf);

    ssize_t bytes = recvmsg(client_fd, &msg, 0);
    if (bytes <= 0) {
        return bytes;
    }

    *out_eventfd = -1;
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        std::memcpy(out_eventfd, CMSG_DATA(cmsg), sizeof(int));
    }

    return bytes;
}

void handle_timer_request(ClientRequest req) {
    std::cout << "[Timer Thread] Scheduled timer for " << req.request.value 
              << " seconds (client_fd #" << req.client_fd 
              << ", eventfd #" << req.eventfd_fd << ")" << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(req.request.value));

    if (req.eventfd_fd != -1) {
        notify_client(req.eventfd_fd);
    }

    Answer answer;
    answer.type = 0;
    answer.value = req.request.value;
    write(req.client_fd, &answer, sizeof(answer));

    cleanup_client_resources(req.client_fd, req.eventfd_fd);
    std::cout << "[Timer Thread] Timer finished and client #" << req.client_fd << " closed." << std::endl;
}

void worker(int id) {
    while (true) {
        sem_wait(&client_queue_semaphore);
        ClientRequest request;
        {
            std::lock_guard<std::mutex> lock(client_queue_mutex);
            request = client_queue.top();
            client_queue.pop();
        }
        std::cout << "Worker #" << id << " picked up client:" << std::endl 
                  << "\tsockfd #" << request.client_fd << std::endl 
                  << "\teventfd #" << request.eventfd_fd << std::endl 
                  << "\tpriority " << request.request.priority << std::endl; 

        if (request.request.command == COMMAND_TIMER) {
            std::thread(handle_timer_request, request).detach();
            continue; 
        }

        Answer answer;
        answer.type = 0;
        answer.value = 0;

        switch (request.request.command) {
            case COMMAND_NOP: {
            } break;
            case COMMAND_WAIT: {
                sleep(request.request.value);
            } break;
            case COMMAND_ECHO: {
                answer.value = request.request.value;
            } break;
            case COMMAND_NEG: {
                answer.value = -request.request.value;
            } break;
            default: {
                answer.type = -1;
            }
        }

        write(request.client_fd, &answer, sizeof(answer));

        cleanup_client_resources(request.client_fd, request.eventfd_fd);
    }
}

void cleanup_and_exit(int signal) {
    std::cout << "\n[Server] Shutting down..." << std::endl;
    if (server_fd != -1) {
        close(server_fd);
    }
    
    {
        std::lock_guard<std::mutex> lock(active_eventfds_mutex);
        for (int efd : active_eventfds) {
            close(efd);
        }
        active_eventfds.clear();
    }

    unlink(socket_path);
    sem_destroy(&client_queue_semaphore);
    exit(0);
}

int main() {
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);

    if (sem_init(&client_queue_semaphore, 0, 0) == -1) {
        perror("semaphore init error");
        return 1;
    }

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket error");
        sem_destroy(&client_queue_semaphore);
        return 1;
    }

    unlink(socket_path);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
        perror("bind error");
        close(server_fd);
        sem_destroy(&client_queue_semaphore);
        return 1;
    }

    if (listen(server_fd, 5) == -1) {
        perror("listen error");
        close(server_fd);
        unlink(socket_path);
        sem_destroy(&client_queue_semaphore);
        return 1;
    }

    std::cout << "[Server] Listening on " << socket_path << std::endl;

    std::vector<std::jthread> workers;
    for (int i = 0; i < worker_count; i++) {
        workers.push_back(std::jthread(worker, i));
    }

    while (true) {
        std::cout << "[Server] Waiting for client connection..." << std::endl;
        
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd == -1) {
            perror("accept error");
            continue;
        }

        std::cout << "[Server] Client #" << client_fd << " connected" << std::endl;

        ClientRequest request;
        request.client_fd = client_fd;
        
        ssize_t bytes = read_request_with_fd(client_fd, &request.request, &request.eventfd_fd);
        
        if (bytes == -1) {
            perror("read error");
            close(client_fd);
            continue;
        }

        if (bytes != sizeof(Request)) {
            perror("protocol violation");
            close(client_fd);
            if (request.eventfd_fd != -1) close(request.eventfd_fd);
            continue;
        }

        if (request.eventfd_fd != -1) {
            std::lock_guard<std::mutex> lock(active_eventfds_mutex);
            active_eventfds.push_back(request.eventfd_fd);
        }
        
        {
            std::lock_guard<std::mutex> lock(client_queue_mutex);
            client_queue.push(request);
        }
        sem_post(&client_queue_semaphore);
    }

    close(server_fd);
    unlink(socket_path);
    sem_destroy(&client_queue_semaphore);

    return 0;
}
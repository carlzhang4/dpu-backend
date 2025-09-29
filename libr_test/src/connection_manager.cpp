#include <assert.h>
#include "connection_manager.hpp"
#include "util.hpp"
#include "mr.h"
void socket_init(NetParam &net_param) {
    if (net_param.sock_port == 0) {
        net_param.sock_port = 6666;
    }
    std::cout << "socket port: " << net_param.sock_port << std::endl;

    if (net_param.nodeId == 0) {
        printf("\n************************************\n");
        printf("* Waiting for client to connect... *\n");
        printf("************************************\n");
        fflush(stdout);
        addrinfo hints{};
        addrinfo *server_address{ nullptr };
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        assert(getaddrinfo(NULL, std::to_string(net_param.sock_port).c_str(), &hints, &server_address) >= 0);
        auto sockfd = socket(server_address->ai_family, server_address->ai_socktype, server_address->ai_protocol);
        assert(sockfd > 0);
        int reuse = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        int bind_result = bind(sockfd, server_address->ai_addr, server_address->ai_addrlen);
        if (bind_result != 0) {
            perror("bind() failed"); // 输出具体错误（如 "Address already in use"）
            assert(bind_result == 0);
        }

        //assert(bind(sockfd, server_address->ai_addr, server_address->ai_addrlen) == 0);
        free(server_address);
        assert(listen(sockfd, 128) == 0);
        for (int i = 0;i < net_param.numNodes - 1;i++) {//numNodes-1 nodes
            int connfd = accept(sockfd, NULL, 0);
            assert(connfd >= 0);
            int nodeId = 0;
            assert(read(connfd, static_cast<void *>(&nodeId), sizeof(nodeId)) == sizeof(nodeId));
            LOG_I("connected by %d", nodeId);
            fflush(stdout);
            net_param.sockfd[nodeId] = connfd;
        //     char* buf_send = (char*)malloc(20);
        // memcpy(buf_send, "thanks for connect", 20);
        // std::cout << "send message to client: " << buf_send << std::endl;
        // int bytes_sent = send(connfd, buf_send, 20, 0);
        // if (bytes_sent < 0) {
        //     perror("send");
        //     std::cout << "error infor: " << strerror(errno) << std::endl;
        // }else {
        //     std::cout << "send message "<< bytes_sent<<" bytes to client"<< std::endl;
        // }
        }
        
    } else {//client
        sleep(1);
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *server_address{ nullptr };
        assert(getaddrinfo(net_param.serverIp.c_str(), std::to_string(net_param.sock_port).c_str(), &hints, &server_address) >= 0);
        int sockfd = socket(server_address->ai_family, server_address->ai_socktype, server_address->ai_protocol);
        assert(sockfd > 0);
        assert(connect(sockfd, server_address->ai_addr, server_address->ai_addrlen) == 0);
        freeaddrinfo(server_address);
        int nodeId = net_param.nodeId;
        assert(send(sockfd, static_cast<void *>(&nodeId), sizeof(nodeId), 0) == sizeof(nodeId));
        net_param.sockfd[0] = sockfd;
        // char* buf_recv = (char*)malloc(20);
        // std::cout << "waiting for message from server" << std::endl;
        // int err= recv(sockfd, buf_recv, 20, 0);
        // if (err < 0) {
        //     perror("recv");
        //     std::cout << "error infor: " << strerror(errno) << std::endl;
        // }
        // std::cout << "recv message from server: " << buf_recv << std::endl;
        
    }
}
void exchange_data(NetParam &net_param, char *data, int size) {
    LOG_D("exchange data size:%d", size);
    size_t dummy;
    (void)dummy;
    if (net_param.nodeId == 0) {
        for (int i = 1;i < net_param.numNodes;i++) {
            dummy = read(net_param.sockfd[i], data + i * size, size);
        }
        for (int i = 1;i < net_param.numNodes;i++) {
            dummy = write(net_param.sockfd[i], data, size * net_param.numNodes);
        }
    } else {
        dummy = write(net_param.sockfd[0], data, size);
        dummy = read(net_param.sockfd[0], data, size * net_param.numNodes);
    }
}

void exchange_vhca_data(NetParam &net_param, vhca_resource *resources, size_t resources_number) {
    LOG_D("exchange vhca data %ld resources", resources_number);

    size_t dummy;
    (void)dummy;
    if (net_param.nodeId == 0) {
        for (int i = 1;i < net_param.numNodes;i++) {
            dummy = write(net_param.sockfd[i], resources, resources_number * sizeof(vhca_resource));
        }
    } else {
        dummy = read(net_param.sockfd[0], resources, resources_number * sizeof(vhca_resource));
    }
}
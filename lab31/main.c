#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "cache.h"
#include <stdbool.h>
#include "proxyServer.h"

extern int STOPPED_PROGRAMM;

void sigIntHandler(int sig){
    STOPPED_PROGRAMM = 1;
}

extern cache_t cache;

int main(int argc, char **argv){
    if (2 != argc) {
        fprintf(stderr, "Usage: %s listen_port\n", argv[0]);
        return EXIT_SUCCESS;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR){
        perror("main: signal error");
        return EXIT_FAILURE;
    }

    if (signal(SIGINT, sigIntHandler) == SIG_ERR){
        perror("SERVER STOP");
        return EXIT_FAILURE;
    }

    int port;
    if (0 != parse_port(argv[1], &port)){
        return EXIT_FAILURE;
    }

    int listen_sockfd = open_socket(port);
    if (-1 == listen_sockfd){
        return EXIT_FAILURE;
    }

    socks_poll_loop(listen_sockfd);
    close(listen_sockfd);
    return EXIT_SUCCESS;
}
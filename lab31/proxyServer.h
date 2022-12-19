#ifndef LAB31_SERVERSOCKER_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <errno.h>

#include "client.h"
#include "errorPrinter.h"

#define LAB31_SERVERSOCKER_H

#define MAX_NUMBER_OF_CLIENTES 1024

int parse_port(char *port_str, int *port);
int open_socket(int port);

int socks_poll_loop(int server_sockfd);

#endif //LAB31_SERVERSOCKER_H
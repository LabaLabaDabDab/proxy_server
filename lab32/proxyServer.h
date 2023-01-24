#ifndef LAB31_SERVERSOCKER_H

#include "client.h"
#include "errorPrinter.h"

#define LAB31_SERVERSOCKER_H

#define MAX_NUMBER_OF_CLIENTES 50

int parse_port(char *port_str, int *port);
int open_socket(int port);
void *client_thread(void *param);
void *http_thread(void *param);

int socks_poll_loop(int server_sockfd);

#endif //LAB31_SERVERSOCKER_H
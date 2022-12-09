#include "proxyServer.h"
#include "client.h"

client_list_t client_list = {.head = NULL};
http_list_t http_list = {.head = NULL};
cache_t cache;

volatile int STOPPED_PROGRAMM = 0;

int parse_port(char *port_str, int *port){
    errno = 0;
    char *endptr = "";
    long num = strtol(port_str, &endptr, 10);   // в endptr сохраняем "неправильные символы"
    if (0 != errno){ //если превысили значение long
        perror("Can't convert given number");
        return -1;
    }
    if (0 != strcmp(endptr, "")){
        fprintf(stderr, "Number contains invalid symbols\n");
        return -1;
    }
    *port = num;
    return 0;
}

int open_socket(int port){
    int sockfd = socket(AF_INET, SOCK_STREAM, 0); //create socket
    if (-1 == sockfd){
        perror("open_socket: socket error");
        return -1;
    }

    struct sockaddr_in server_adr;
    memset(&server_adr, 0, sizeof (struct sockaddr_in));
    server_adr.sin_family = AF_INET; //ipv4 address
    server_adr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_adr.sin_port = htons(port); //listening on this port         //1011 11 13  

    if (-1 == (bind(sockfd, (struct sockaddr *)&server_adr, sizeof (struct sockaddr_in)))){      //binding the socket to the address
        perror("open_socket: bind error");
        close(sockfd);
        return -1;
    }

    if (-1 == listen(sockfd, SOMAXCONN)){       //start listening
        perror("open socket: listen error");
        close(sockfd);
        return -1;
    }
    //write(STDOUT_FILENO,"server: waiting for connections..\n", 34);

    return sockfd;
}

int socks_poll_loop(int server_sockfd){

    if (0 != cache_init(&cache)){
        return EXIT_FAILURE;
    }

    int SERVERFD_SIZE = MAX_NUMBER_OF_CLIENTES * 2 + 1; //максимальное кол-во возможных подключенных клиентов и их соединений + деск сервера
    struct pollfd serverFd[SERVERFD_SIZE]; 
    for (size_t i = 0; i < SERVERFD_SIZE; i++){
        serverFd[i].fd = -1;
    }
    
    int new_connected_client_fd = 0;
    size_t all_accepted_clients = 0;
    int tracked_desc_number = 1; //server

    //каждую итерацию цикла: 0 - прокси, последующие элементы клиенты и запросы

    while(!STOPPED_PROGRAMM){
        serverFd[0].fd = all_accepted_clients >= MAX_NUMBER_OF_CLIENTES ? -1 : server_sockfd; //если кол во подсоединенных клиентов больше назначенного, то мы ставим -1
                                                                                              // чтобы poll игнорировал новые подключения
        serverFd[0].events = POLLIN;

        int index = 1;

        {
            client_t *cur_client = client_list.head;
            while (NULL != cur_client){
                client_t *next = cur_client->next; //заранее сохраняем указатель на сл клиент

                if(IS_ERROR_OR_DONE_STATUS(cur_client->status)){
                    client_remove(cur_client, &client_list);
                    cur_client = next;
                    continue;
                }

                client_update_http_info(cur_client);
                check_finished_writing_to_client(cur_client);
                serverFd[index].fd = cur_client->sockfd;
                serverFd[index].events = POLLIN; //ждем чтение
                if ((cur_client->status == DOWNLOADING && cur_client->bytes_written < cur_client->http_entry->data_size) ||
                            (cur_client->status == GETTING_FROM_CACHE && cur_client->bytes_written < cur_client->cache_node->size)) {
                    serverFd[index].events |= POLLOUT; //ждем записи
                }
                index++;
                cur_client = next;
            }
            
            http_t *cur_http = http_list.head;
            while(NULL != cur_http){
                http_t *next = cur_http->next;
                if(http_check_disconnect(cur_http)){
                    remove_http(cur_http, &http_list, &cache);
                    cur_http = next;
                    continue;
                }

                serverFd[index].events = 0; //очищаем от мусора

                serverFd[index].fd = cur_http->sockfd;

                if (!IS_ERROR_OR_DONE_STATUS(cur_http->status)){  //если запрос
                    
                    serverFd[index].events |= POLLIN; //ждем чтение
                }
                
                if(cur_http->status == AWAITING_REQUEST){
                    serverFd[index].events |= POLLOUT;  //ждем запись
                }

                index++;
                cur_http = next;
            }
        }
 
        int poll_status;
        poll_status = poll(serverFd, index, 1);
        //printf("%d\n", poll_status);
        
        if (-1 == poll_status){
            print_error("proxyServer error: poll()\n", poll_status);
            printf("SERVER HAS STOPPED\n");
            break;
        }

        if (0 == poll_status){
            continue;;
        }
        //смотрим на ошибки у сервера
        //если принятый сокет недоступен       //POLLERR - ошибки в дискприторе      //POLLHUP - завис         //POLLNVAL - файловый дескриптор не открыт
        if(serverFd[0].revents & POLLERR || serverFd[0].revents & POLLHUP || serverFd[0].revents & POLLNVAL){
            printf("proxyServer error: poll(): accept socket is unavailable\n");
            printf("SERVER HAS STOPPED\n");
            break;
        }

        index = 1;

        {
            client_t *cur_client = client_list.head;
            while(NULL != cur_client){
                client_t *next = cur_client->next;
                if(!IS_ERROR_OR_DONE_STATUS(cur_client->status) && serverFd[index].revents & POLLIN){
                    client_read_data(cur_client, &http_list, &cache);
                }

                if (((cur_client->status == DOWNLOADING && cur_client->http_entry->status == DOWNLOADING && cur_client->bytes_written < cur_client->http_entry->data_size) ||
                    (cur_client->status == GETTING_FROM_CACHE && cur_client->bytes_written < cur_client->cache_node->size)) && serverFd[index].revents & POLLOUT){
                    write_to_client(cur_client);
                }
                index++;
                cur_client = next;
            }

            http_t *cur_http = http_list.head;
            while (cur_http != NULL) {
                http_t *next = cur_http->next;
                if (!IS_ERROR_OR_DONE_STATUS(cur_http->status) && serverFd[index].revents & POLLIN){
                    http_read_data(cur_http, &cache);
                }
                if (cur_http->status == AWAITING_REQUEST && serverFd[index].revents & POLLOUT){
                    http_send_request(cur_http);
                }
                index++;
                cur_http = next;
            }
        }
        
        //если есть новое подключение
        if (serverFd[0].revents & POLLIN){
            printf("Listening proxy socket accept new incoming connections...\n");
            //poll чекает заблокируется ли акспепт или нет
            //если не заблокируется то вернетуся управление poll (в ревентс POLLIN)
            new_connected_client_fd = accept(server_sockfd, NULL, NULL);
            
            if(-1 == new_connected_client_fd){
                perror("PROXY ACCEPT ERROR: Error while executing a accept\n");
                printf("SERVER HAS STOPPED\n");
                break;
            }
            if (ERROR_LOG) fprintf(stderr, "new client connected\n");
            create_client(&client_list, new_connected_client_fd);
            all_accepted_clients++;            
        }
    }
}
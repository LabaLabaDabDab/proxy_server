#ifndef LAB31_STATES_H
#define LAB31_STATES_H

#include <stddef.h>

#define BUF_SIZE 32000

#define GETTING_FROM_CACHE 2 //только для клиента
#define DOWNLOADING 1
#define AWAITING_REQUEST 0
#define SOCK_DONE -1
#define SOCK_ERROR -2

//#define DROP_HTTP_NO_CLIENTS

#define IS_ERROR_OR_DONE_STATUS(STATUS) ((STATUS) < 0)
#define IS_ERROR_STATUS(STATUS) ((STATUS) == SOCK_ERROR)
#define STR_EQ(STR1, STR2) (strcmp(STR1, STR2) == 0)

#define HTTP_NO_HEADERS -1

#define HTTP_CODE_UNDEFINED -1
#define HTTP_CODE_NONE 0

#define HTTP_RESPONSE_CONTENT_LENGTH 2
#define HTTP_RESPONSE_CHUNKED 1
#define HTTP_RESPONSE_NONE 0

int strings_equal_by_length(const char *str1, size_t len1, const char *str2, size_t len2);
int convert_number(char *str, int *number);
int get_number_from_string_by_length(const char *str, size_t length);

#endif 
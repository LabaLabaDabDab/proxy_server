#include "states.h"

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

int strings_equal_by_length(const char *str1, size_t len1, const char *str2, size_t len2){ //сравниваем по длине потому что распарсенные не содеражт \0
    if (len1 != len2) 
        return 0;
    if (NULL == str1 || NULL == str2) 
        return 0;
    for (size_t i = 0; i < len1; i++) {
        if (str1[i] != str2[i]) 
            return 0;
    }
    return 1;
}

int convert_number(char *str, int *number){
    errno = 0;
    char *endptr = "";
    long num = strtol(str, &endptr, 10);

    if (0 != errno) {
        perror("Can't convert given number");
        return -1;
    }
    if (0 != strcmp(endptr, "")) {
        fprintf(stderr, "Number contains invalid symbols\n");
        return -1;
    }

    *number = (int)num;
    return 0;
}

int get_number_from_string_by_length(const char *str, size_t length){
    char buf1[length + 1];
    memcpy(buf1, str, length);
    buf1[length] = '\0';
    int num = -1;
    convert_number(buf1, &num);
    return num;
}
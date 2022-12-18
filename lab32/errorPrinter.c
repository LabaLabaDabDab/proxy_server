#include "errorPrinter.h"

void print_error(const char *prefix, int code) {
    char buf[256];

    if (0 != strerror_r(code, buf, sizeof(buf))) {
        strcpy(buf, "(unable to generate error!)");
    }

    fprintf(stderr, "%s: %s\n", prefix, buf);
}

void print_error2(const char *prefix1, const char *prefix2, int code){
    char buf[256];

    if (0 != strerror_r(code, buf, sizeof(buf))) {
        strcpy(buf, "(unable to generate error!)");
    }
    
    fprintf(stderr, "%s: %s: %s\n", prefix1, prefix2, buf);
}
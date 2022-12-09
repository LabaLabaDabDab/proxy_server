#include "errorPrinter.h"

void print_error(const char *prefix, int code) {
    char buf[256];
    if (0 != strerror_r(code, buf, sizeof(buf))) {
        strcpy(buf, "(unable to generate error!)");
    }
    fprintf(stderr, "%s: %s\n", prefix, buf);
}
cmake_minimum_required(VERSION 3.9)
project(PROXY-1 C)

set(CMAKE_C_STANDARD 99)

set(CMAKE_C_FLAGS "-Os")

add_executable(lab31.out main.c picohttpparser.h picohttpparser.c cache.h cache.c http.h http.c client.h client.c states.h states.c errorPrinter.h errorPrinter.c proxyServer.h proxyServer.c)

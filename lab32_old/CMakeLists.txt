cmake_minimum_required(VERSION 3.9)
project(PROXY-1 C)

set(CMAKE_C_STANDARD 99)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(lab32.out main.c picohttpparser.h picohttpparser.c cache.h cache.c http.h http.c client.h client.c states.h states.c errorPrinter.h errorPrinter.c proxyServer.h proxyServer.c)
target_link_libraries(lab32.out PRIVATE Threads::Threads)

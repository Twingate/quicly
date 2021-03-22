#pragma once

#if defined(_WIN32)
    #include <Winsock2.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/udp.h>
    #include <netdb.h>
    #include <sys/time.h>
    #include <pthread.h>
#endif

/**
 * @file platform_socket.h
 * @brief Platform abstraction for BSD socket APIs (POSIX / Winsock2)
 *
 * Include this instead of <sys/socket.h>, <arpa/inet.h>, etc.
 * Provides consistent types and function names across platforms.
 */

#pragma once

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>

    // Winsock uses SOCKET (unsigned), POSIX uses int
    typedef SOCKET socket_t;
    #define INVALID_SOCKET_VALUE INVALID_SOCKET

    // Winsock close/error equivalents
    inline int platform_close_socket(socket_t s) { return closesocket(s); }
    inline int platform_socket_error() { return WSAGetLastError(); }
    #define PLATFORM_EAGAIN WSAEWOULDBLOCK
    #define PLATFORM_EINPROGRESS WSAEWOULDBLOCK
    #define PLATFORM_SHUT_RDWR SD_BOTH

    // Winsock uses char* for setsockopt/getsockopt data, POSIX uses void*
    #define SETSOCKOPT_CAST(x) reinterpret_cast<const char*>(x)
    #define GETSOCKOPT_CAST(x) reinterpret_cast<char*>(x)

    // SO_RCVTIMEO/SO_SNDTIMEO take DWORD (milliseconds) on Windows
    inline void platform_set_socket_timeout(socket_t s, int opt, int seconds) {
        DWORD ms = static_cast<DWORD>(seconds * 1000);
        setsockopt(s, SOL_SOCKET, opt, reinterpret_cast<const char*>(&ms), sizeof(ms));
    }

    // Non-blocking mode
    inline void platform_set_nonblocking(socket_t s, bool enable) {
        u_long mode = enable ? 1 : 0;
        ioctlsocket(s, FIONBIO, &mode);
    }

    // Winsock requires startup/cleanup
    inline void platform_socket_init() {
        static bool initialized = false;
        if (!initialized) {
            WSADATA wsaData;
            WSAStartup(MAKEWORD(2, 2), &wsaData);
            initialized = true;
        }
    }

    inline void platform_socket_cleanup() {
        WSACleanup();
    }

    #ifndef _SSIZE_T_DEFINED
    #define _SSIZE_T_DEFINED
    typedef int ssize_t;
    #endif

    // send/recv on Winsock use char*, POSIX uses void*
    #define SEND_CAST(x) reinterpret_cast<const char*>(x)
    #define RECV_CAST(x) reinterpret_cast<char*>(x)

#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>

    typedef int socket_t;
    #define INVALID_SOCKET_VALUE (-1)

    inline int platform_close_socket(socket_t s) { return close(s); }
    inline int platform_socket_error() { return errno; }
    #define PLATFORM_EAGAIN EAGAIN
    #define PLATFORM_EINPROGRESS EINPROGRESS
    #define PLATFORM_SHUT_RDWR SHUT_RDWR

    #define SETSOCKOPT_CAST(x) (x)
    #define GETSOCKOPT_CAST(x) (x)

    inline void platform_set_socket_timeout(socket_t s, int opt, int seconds) {
        struct timeval tv;
        tv.tv_sec = seconds;
        tv.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, opt, &tv, sizeof(tv));
    }

    inline void platform_set_nonblocking(socket_t s, bool enable) {
        int flags = fcntl(s, F_GETFL, 0);
        if (enable)
            fcntl(s, F_SETFL, flags | O_NONBLOCK);
        else
            fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
    }

    inline void platform_socket_init() {}
    inline void platform_socket_cleanup() {}

    #define SEND_CAST(x) (x)
    #define RECV_CAST(x) (x)
#endif

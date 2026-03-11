/*
 * **** liblogtap.c - source code for liblogtap.so ****
 *
 * This library is designed to be preloaded using the LD_PRELOAD
 * Linux environment variable to intercept stdout and stderr on
 * the communication between the targeted app and the kernel.
 *
 * That way it's possible to intercept log output of apps in
 * docker or kubernetes containers and to process it in a sidecar
 * container.
 *
 * To intercept stdout and/or stderr of third party images or
 * custom images where you cannot, don't want to or are not allowed
 * to modify the image, you may use an init container to inject
 * this library. If you need halp doing so, feel free to ask the
 * maintainer of this repository.
 *
 * ----
 *
 * This file is part of https://github.com/katalyticIT/liblogtap and is
 * licensed under GPL 3.0. See LICENSE file for details.
 *
 */


#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <pthread.h>
#include <errno.h>
#include <stdbool.h>

// pointer to funktion for "real" write syscalls
typedef ssize_t (*write_func_t)(int, const void*, size_t);
static write_func_t real_write = NULL;

typedef ssize_t (*writev_func_t)(int, const struct iovec *, int);
static writev_func_t real_writev = NULL;

// configuration
static int  tap_stdout = 0;
static int  tap_stderr = 0;
static bool debug_mode = false;
static bool suppress_stdout    = false;
static int  reconnect_interval = 30;

static char target_type[16]  = "file";
static char target_path[256] = "/tmp/liblogtap.log";

// runtime status
static int    target_fd = -1;                         // file descriptor
static time_t next_connect_time = 0;                  // timer for reconnecting
static bool   initial_connection_established = false; // Tracks the initial connection status
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// helper function for internal debug output;
// DO NOT USE IN PRODUCTION ENVIRONMENT!!!
// (Means don't set LLT_DEBUG_MODE=true)
static void debug_log(const char *msg) {
    if (debug_mode && !suppress_stdout && real_write) {
        char buf[512];
        int len = snprintf(buf, sizeof(buf), "[liblogtap DEBUG PID:%d] %s\n", getpid(), msg);
        real_write(STDOUT_FILENO, buf, len);
    }
}


// Initialisation on loading of library
__attribute__((constructor)) static void init_liblogtap() {
    real_write  = ( write_func_t)dlsym(RTLD_NEXT, "write");
    real_writev = (writev_func_t)dlsym(RTLD_NEXT, "writev");

    // Additional for Python apps: force unbuffered output by envvar; the third
    // parameter ('1') means that the envvar gets overwritten if already set.
    setenv("PYTHONUNBUFFERED", "1", 1);

    // read LLT envvars
    const char *env_tap = getenv("LLT_TAP_INTO");
    if (env_tap) {
        int val = atoi(env_tap);
        if (val == 1 || val == 3) tap_stdout = 1;
        if (val == 2 || val == 3) tap_stderr = 1;
    }

    const char *env_suppress = getenv("LLT_SUPPRESS_STDOUT");
    if (env_suppress && strcmp(env_suppress, "true") == 0) {
        suppress_stdout = true;
    }

    const char *env_debug = getenv("LLT_DEBUG_MODE");
    if (env_debug && strcmp(env_debug, "true") == 0) {
        debug_mode = true;
    }

    const char *env_reconnect = getenv("LLT_TARGET_RECONNECT");
    if (env_reconnect) {
        reconnect_interval = atoi(env_reconnect);
    }

    const char *env_target = getenv("LLT_TARGET");
    if (env_target) {
        if (strncmp(env_target, "socket:", 7) == 0) {
            strncpy(target_type, "socket", sizeof(target_type));
            strncpy(target_path, env_target + 7, sizeof(target_path) - 1);
        } else if (strncmp(env_target, "file:", 5) == 0) {
            strncpy(target_type, "file", sizeof(target_type));
            strncpy(target_path, env_target + 5, sizeof(target_path) - 1);
        }
    }

    debug_log("liblogtap active (debug mode) - injected PYTHONUNBUFFERED=1");
}

// Lazy Connection with Fast-Retry for Sidecars
static void try_connect() {
    time_t now = time(NULL);
    if (now < next_connect_time) return;

    // Check for time to wait: 5s on startup race condition, else $LLT_TARGET_RECONNECT
    int wait_time = initial_connection_established ? reconnect_interval : 5;

    // if target 'file' is defined, open the file (default=/tmp/liblogtap.log; see above)
    if (strcmp(target_type, "file") == 0) {
        target_fd = open(target_path, O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK, 0666);
        if (target_fd >= 0) {
            initial_connection_established = true;
            debug_log("Successfully opened target file.");
        } else {
            char debug_msg[128];
            snprintf(debug_msg, sizeof(debug_msg), "Failed to open target file. Retrying in %ds.", wait_time);
            debug_log(debug_msg);
            next_connect_time = now + wait_time;
        }
    }
    // otherwise open the socket
    else if (strcmp(target_type, "socket") == 0) {
        target_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (target_fd >= 0) {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, target_path, sizeof(addr.sun_path) - 1);

            if (connect(target_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0 || errno == EINPROGRESS) {
                initial_connection_established = true;
                debug_log("Successfully connected to target socket.");
            } else {
                char debug_msg[128];
                snprintf(debug_msg, sizeof(debug_msg), "Failed to connect to target socket. Retrying in %ds.", wait_time);
                debug_log(debug_msg);

                close(target_fd);
                target_fd = -1;
                next_connect_time = now + wait_time;
            }
        }
    }
}

// ---------------------------------------------------------
// 1. Overwrite write() syscall
// ---------------------------------------------------------
ssize_t write(int fd, const void *buf, size_t count) {
    if (!real_write) real_write = (write_func_t)dlsym(RTLD_NEXT, "write");

    bool is_intercepted = ((fd == STDOUT_FILENO && tap_stdout) || (fd == STDERR_FILENO && tap_stderr));

    if (is_intercepted) {
        if (pthread_mutex_trylock(&lock) == 0) {
            if (target_fd == -1) try_connect();

            # if file/socket is open, write data there
            if (target_fd != -1) {
                ssize_t ret = real_write(target_fd, buf, count);
                if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { // whoops - somtehing went wrong ...
                    close(target_fd);
                    target_fd = -1;
                    next_connect_time = time(NULL) + reconnect_interval;
                    debug_log("Write to target failed. Connection closed. Dropping to normal reconnect interval.");
                }
            }
            pthread_mutex_unlock(&lock);
        }
        if (suppress_stdout) return count;  // in suppress mode: tell the app everything went fine, but write nothing
    }
    return real_write(fd, buf, count);
}

// ---------------------------------------------------------
// 2. Overwrite writev() syscall
// ---------------------------------------------------------
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    if (!real_writev) real_writev = (writev_func_t)dlsym(RTLD_NEXT, "writev");

    bool is_intercepted = ((fd == STDOUT_FILENO && tap_stdout) || (fd == STDERR_FILENO && tap_stderr));

    if (is_intercepted) {
        if (pthread_mutex_trylock(&lock) == 0) {
            if (target_fd == -1) try_connect();

            # if file/socket is open, write data there
            if (target_fd != -1) {
                ssize_t ret = real_writev(target_fd, iov, iovcnt);
                if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { // whoops - somtehing went wrong ...
                    close(target_fd);
                    target_fd = -1;
                    next_connect_time = time(NULL) + reconnect_interval;
                    debug_log("Writev to target failed. Connection closed. Dropping to normal reconnect interval.");
                }
            }
            pthread_mutex_unlock(&lock);
        }

        if (suppress_stdout) { // if in suppress mode:
            ssize_t total = 0;
            for (int i = 0; i < iovcnt; i++) {
                total += iov[i].iov_len;
            }
            return total;      // tell the app everything went fine, but write nothing

        }
    }
    return real_writev(fd, iov, iovcnt);
}

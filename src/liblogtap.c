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
#include <sys/syscall.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdatomic.h>

// Macro to handle syscall differences between older/newer kernels or architectures
#ifdef SYS_open
#define DO_SYS_OPEN(path, flags, mode) syscall(SYS_open, path, flags, mode)
#else
#define DO_SYS_OPEN(path, flags, mode) syscall(SYS_openat, AT_FDCWD, path, flags, mode)
#endif

// --- Function Pointers ---
static ssize_t (*real_write)(int, const void*, size_t) = NULL;
static ssize_t (*real_writev)(int, const struct iovec *, int) = NULL;
static int (*real_open)(const char*, int, ...) = NULL;
static int (*real_open64)(const char*, int, ...) = NULL;
static int (*real_openat)(int, const char*, int, ...) = NULL;
static int (*real_close)(int) = NULL;

// --- Thread-safe Status Variables ---
static atomic_int tap_file_state = 0; // 0 = unknown, -1 = not opened, 1 = opened
static atomic_int tap_file_fd = -1;

static atomic_flag dlsym_lock = ATOMIC_FLAG_INIT;
static atomic_int symbols_loaded = 0;
static atomic_flag scan_done = ATOMIC_FLAG_INIT;

// --- Global Configuration ---
static struct timespec start_time;
static long long passive_time_ms = 1000;

static int tap_stdout = 0;
static int tap_stderr = 0;
static bool suppress_stdout = false;
static char tap_file_path[512] = "";
static bool suppress_tap_file = false;
static bool debug_mode = false;
static int reconnect_interval = 30;

static char target_type[16] = "file";
static char target_path[256] = "/tmp/liblogtap.log";
static int target_fd = -1;
static time_t next_connect_time = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool initial_connection_established = false;

// --- Helper Functions ---

/**
 * Outputs debug messages directly to STDERR_FILENO.
 * CRITICAL: This function uses direct kernel syscalls (`SYS_write`) instead of 
 * standard C library functions (like `printf` or `write`). This prevents our own 
 * debug logs from accidentally triggering our `write` hooks, which would cause an infinite loop.
 */
static void debug_log(const char *msg) {
    if (debug_mode && !suppress_stdout) {
        char buf[512];
        int len = snprintf(buf, sizeof(buf), "[liblogtap DEBUG PID:%ld] %s\n", syscall(SYS_getpid), msg);
        syscall(SYS_write, STDERR_FILENO, buf, len); 
    }
}

/**
 * Checks if the library is still in its initial "passive" startup phase.
 * We use this to allow runtimes (like glibc or Python) to fully initialize 
 * their thread-local storage and internal states without interference from our hooks.
 */
static inline bool is_passive() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long elapsed = (now.tv_sec - start_time.tv_sec) * 1000LL + (now.tv_nsec - start_time.tv_nsec) / 1000000LL;
    return elapsed < passive_time_ms;
}

/**
 * Lazily loads the original system call pointers using `dlsym`.
 * CRITICAL: This is protected by an atomic flag (`dlsym_lock`). If multiple threads 
 * hit a hook simultaneously, only one will resolve the symbols. The others will receive 
 * `false` and bypass the hook to prevent race conditions and initialization deadlocks.
 */
static bool ensure_dlsym() {
    if (atomic_load(&symbols_loaded) == 1) return true;

    if (!atomic_flag_test_and_set(&dlsym_lock)) {
        real_write = dlsym(RTLD_NEXT, "write");
        real_writev = dlsym(RTLD_NEXT, "writev");
        real_open = dlsym(RTLD_NEXT, "open");
        real_open64 = dlsym(RTLD_NEXT, "open64");
        real_openat = dlsym(RTLD_NEXT, "openat");
        real_close = dlsym(RTLD_NEXT, "close");
        
        atomic_store(&symbols_loaded, 1);
        debug_log("Passive phase over. Functions intercepted successfully.");
        return true;
    }
    return false;
}

/**
 * Constructor executed automatically by the dynamic linker (ld.so) upon loading the library.
 * It marks the start time for the passive phase and reads all configuration from the environment.
 */
__attribute__((constructor)) static void bootstrap() {
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    // Injects PYTHONUNBUFFERED=1 into the environment. 
    // This forces Python to flush standard streams immediately, allowing us to intercept logs in real-time.
    setenv("PYTHONUNBUFFERED", "1", 1);
    
    // LLT_PASSIV_ON_START: Time in ms to wait before activating hooks. 
    // Prevents early-startup SegFaults and deadlocks with modern glibc/runtimes.
    const char *env_passive = getenv("LLT_PASSIV_ON_START");
    if (env_passive) passive_time_ms = atoll(env_passive);

    // LLT_TAP_INTO: Bitmask to define which standard streams to tap (1 = stdout, 2 = stderr, 3 = both).
    const char *env_tap = getenv("LLT_TAP_INTO");
    if (env_tap) { int val = atoi(env_tap); if (val&1) tap_stdout=1; if (val&2) tap_stderr=1; }
    
    // LLT_SUPPRESS_STDOUT: If "true", intercepted standard stream data is NOT written to the original console.
    if (getenv("LLT_SUPPRESS_STDOUT") && strcmp(getenv("LLT_SUPPRESS_STDOUT"), "true") == 0) suppress_stdout = true;
    
    // LLT_TAP_FILE: The absolute path of a specific log file to intercept (e.g., third-party app logs).
    if (getenv("LLT_TAP_FILE")) strncpy(tap_file_path, getenv("LLT_TAP_FILE"), sizeof(tap_file_path) - 1);
    else atomic_store(&tap_file_state, -1);

    // LLT_SUPPRESS_TAP_FILE: If "true", intercepted file writes are NOT written to the actual file on disk (saves disk space).
    if (getenv("LLT_SUPPRESS_TAP_FILE") && strcmp(getenv("LLT_SUPPRESS_TAP_FILE"), "true") == 0) suppress_tap_file = true;
    
    // LLT_DEBUG_MODE: Enables internal debug outputs from liblogtap.
    if (getenv("LLT_DEBUG_MODE") && strcmp(getenv("LLT_DEBUG_MODE"), "true") == 0) debug_mode = true;
    
    // LLT_TARGET_RECONNECT: Interval in seconds before attempting to reconnect to the target socket/file after a failure.
    if (getenv("LLT_TARGET_RECONNECT")) reconnect_interval = atoi(getenv("LLT_TARGET_RECONNECT"));
    
    // LLT_TARGET: Defines the destination for the intercepted logs (format: "socket:/path" or "file:/path").
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
    
    char boot_msg[128];
    snprintf(boot_msg, sizeof(boot_msg), "Library injected. Passive for %lld ms.", passive_time_ms);
    debug_log(boot_msg);
}

/**
 * Checks if a newly opened file descriptor matches our target tap file.
 * Uses Inode matching (`st_ino`, `st_dev`) instead of simple string comparison to reliably 
 * identify files even if the application uses relative paths or symlinks.
 */
static void check_if_target(int fd) {
    if (atomic_load(&tap_file_state) == 1) return; 
    if (tap_file_path[0] == '\0') return;

    struct stat target_st, fd_st;
    if (stat(tap_file_path, &target_st) == 0 && fstat(fd, &fd_st) == 0) {
        if (target_st.st_ino == fd_st.st_ino && target_st.st_dev == fd_st.st_dev) {
            atomic_store(&tap_file_fd, fd);
            atomic_store(&tap_file_state, 1);
            char msg[256]; snprintf(msg, sizeof(msg), "Target file matched via inode! (FD: %d)", fd); debug_log(msg);
        }
    }
}

/**
 * Fallback scanner: Executed once after the passive phase ends.
 * It checks if the target file was already opened by the application *while* our library 
 * was still in passive mode. Scans file descriptors 0-1023.
 */
static void scan_existing_fds() {
    if (atomic_flag_test_and_set(&scan_done)) return;
    if (tap_file_path[0] == '\0') return;

    struct stat target_st;
    if (stat(tap_file_path, &target_st) != 0) {
        atomic_store(&tap_file_state, -1);
        return;
    }

    for (int i = 0; i < 1024; i++) {
        struct stat fd_st;
        if (fstat(i, &fd_st) == 0) {
            if (target_st.st_ino == fd_st.st_ino && target_st.st_dev == fd_st.st_dev) {
                atomic_store(&tap_file_fd, i);
                atomic_store(&tap_file_state, 1);
                char msg[256]; snprintf(msg, sizeof(msg), "Target file discovered from passive phase! (FD: %d)", i); debug_log(msg);
                return;
            }
        }
    }
    atomic_store(&tap_file_state, -1);
}

/**
 * Manages the connection to the destination (socket or file) where intercepted logs are written.
 * Uses direct syscalls (`SYS_openat`) to avoid triggering our own hooks during setup.
 * Features a "Lazy Connection" strategy and handles automatic reconnects.
 */
static void try_connect() {
    time_t now = time(NULL);
    if (now < next_connect_time) return;

    int wait_time = initial_connection_established ? reconnect_interval : 5;

    if (strcmp(target_type, "file") == 0) {
        target_fd = syscall(SYS_openat, AT_FDCWD, target_path, O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK, 0666);
        if (target_fd >= 0) {
            initial_connection_established = true; debug_log("Successfully opened target file.");
        } else {
            next_connect_time = now + wait_time;
        }
    } 
    else if (strcmp(target_type, "socket") == 0) {
        target_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (target_fd >= 0) {
            struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX; strncpy(addr.sun_path, target_path, sizeof(addr.sun_path) - 1);
            if (connect(target_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0 || errno == EINPROGRESS) {
                initial_connection_established = true; debug_log("Successfully connected to target socket.");
            } else {
                syscall(SYS_close, target_fd); target_fd = -1; next_connect_time = now + wait_time;
            }
        }
    }
}

// ---------------------------------------------------------
// Intercepted Functions (Hooks)
// ---------------------------------------------------------

/**
 * Intercepts the standard open() syscall.
 */
int open(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    
    // If we are in the passive startup phase, OR if the original functions are currently 
    // being resolved by another thread (!ensure_dlsym()), we fall back to a direct kernel 
    // syscall. This guarantees we never block the application or cause a deadlock during startup.
    if (is_passive() || !ensure_dlsym()) {
        return DO_SYS_OPEN(pathname, flags, mode);
    }
    
    int fd = real_open(pathname, flags, mode);
    if (fd >= 0) check_if_target(fd);
    return fd;
}

/**
 * Intercepts the open64() syscall (used for large file support).
 */
int open64(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    
    if (is_passive() || !ensure_dlsym()) {
        return DO_SYS_OPEN(pathname, flags, mode);
    }
    
    int fd = real_open64(pathname, flags, mode);
    if (fd >= 0) check_if_target(fd);
    return fd;
}

/**
 * Intercepts the openat() syscall (modern standard used by most current runtimes like Python 3).
 */
int openat(int dirfd, const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    
    if (is_passive() || !ensure_dlsym()) {
        return syscall(SYS_openat, dirfd, pathname, flags, mode);
    }
    
    int fd = real_openat(dirfd, pathname, flags, mode);
    if (fd >= 0) check_if_target(fd);
    return fd;
}

/**
 * Intercepts the close() syscall to detect when our tapped file is closed by the application.
 */
int close(int fd) {
    if (is_passive() || !ensure_dlsym()) {
        return syscall(SYS_close, fd);
    }
    
    if (atomic_load(&tap_file_state) == 1 && fd == atomic_load(&tap_file_fd)) {
        atomic_store(&tap_file_fd, -1);
        atomic_store(&tap_file_state, -1);
        debug_log("Target file closed. Resetting state.");
    }
    return real_close(fd);
}

/**
 * Intercepts the standard write() syscall. 
 * Checks if the target file descriptor matches stdout, stderr, or our tapped log file.
 */
ssize_t write(int fd, const void *buf, size_t count) {
    if (is_passive() || !ensure_dlsym()) {
        return syscall(SYS_write, fd, buf, count);
    }
    
    if (atomic_load(&tap_file_state) == 0) {
        scan_existing_fds();
    }

    bool is_std_stream = ((fd == STDOUT_FILENO && tap_stdout) || (fd == STDERR_FILENO && tap_stderr));
    bool is_tapped_file = (atomic_load(&tap_file_state) == 1 && fd == atomic_load(&tap_file_fd));
    
    if (is_std_stream || is_tapped_file) {
        if (pthread_mutex_trylock(&lock) == 0) {
            if (target_fd == -1) try_connect();
            if (target_fd != -1) {
                ssize_t ret = syscall(SYS_write, target_fd, buf, count);
                if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    syscall(SYS_close, target_fd); 
                    target_fd = -1; 
                    next_connect_time = time(NULL) + reconnect_interval;
                    debug_log("Write to target failed. Connection closed.");
                }
            }
            pthread_mutex_unlock(&lock);
        }
        
        if ((is_std_stream && suppress_stdout) || (is_tapped_file && suppress_tap_file)) {
            return count; // Fakes a successful write to the application while dropping the actual disk/console write
        }
    }
    return real_write(fd, buf, count);
}

/**
 * Intercepts the writev() syscall (vectorized write).
 * Frequently used by modern frameworks, streaming libraries, or buffered I/O functions.
 */
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    if (is_passive() || !ensure_dlsym()) {
        return syscall(SYS_writev, fd, iov, iovcnt);
    }
    
    if (atomic_load(&tap_file_state) == 0) scan_existing_fds();

    bool is_std_stream = ((fd == STDOUT_FILENO && tap_stdout) || (fd == STDERR_FILENO && tap_stderr));
    bool is_tapped_file = (atomic_load(&tap_file_state) == 1 && fd == atomic_load(&tap_file_fd));

    if (is_std_stream || is_tapped_file) {
        if (pthread_mutex_trylock(&lock) == 0) {
            if (target_fd == -1) try_connect();
            if (target_fd != -1) {
                ssize_t ret = syscall(SYS_writev, target_fd, iov, iovcnt);
                if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    syscall(SYS_close, target_fd); target_fd = -1; next_connect_time = time(NULL) + reconnect_interval;
                }
            }
            pthread_mutex_unlock(&lock);
        }
        if ((is_std_stream && suppress_stdout) || (is_tapped_file && suppress_tap_file)) {
            ssize_t total = 0; for (int i = 0; i < iovcnt; i++) total += iov[i].iov_len; return total;
        }
    }
    return real_writev(fd, iov, iovcnt);
}

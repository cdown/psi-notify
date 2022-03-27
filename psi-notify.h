#include <stdbool.h>
#include <stdio.h>
#include <strings.h>
#include <time.h>

/* Data structures */

typedef enum ResourceType { RT_CPU, RT_MEMORY, RT_IO } ResourceType;
typedef enum AlertState {
    A_INACTIVE,
    A_ACTIVE,
    A_STABILISING,
    A_ERROR
} AlertState;

typedef struct {
    double some;
    double full;
} TimeResourcePressure;

typedef struct {
    TimeResourcePressure avg10;
    TimeResourcePressure avg60;
    TimeResourcePressure avg300;
} Pressure;

typedef struct {
    char *filename;
    char *human_name;
    unsigned int has_full;
    ResourceType type;
    Pressure thresholds;
} Resource;

typedef struct {
    Resource cpu;
    Resource memory;
    Resource io;
    time_t update_interval;
    bool log_pressures;
    int psi_dir_fd;
} Config;

typedef struct {
    NotifyNotification *notif;
    time_t remaining_intervals;
    AlertState last_state;
} Alert;

/* Utility macros and functions */

#define info(format, ...) printf("INFO: " format, __VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "WARN: " format, __VA_ARGS__)
#define die(format, ...)                                                       \
    do {                                                                       \
        fprintf(stderr, "FATAL: " format, __VA_ARGS__);                        \
        abort();                                                               \
    } while (0)

#define unreachable() die("%s\n", "Allegedly unreachable code reached\n")
#define expect(x)                                                              \
    do {                                                                       \
        if (!(x)) {                                                            \
            die("!(%s) at %s:%s:%d\n", #x, __FILE__, __func__, __LINE__);      \
        }                                                                      \
    } while (0)

#define snprintf_check(buf, len, fmt, ...)                                     \
    do {                                                                       \
        int needed = snprintf(buf, len, fmt, __VA_ARGS__);                     \
        expect(needed >= 0 && (size_t)needed < (len));                         \
    } while (0)

#define for_each_arr(i, items)                                                 \
    for (i = 0; i < sizeof(items) / sizeof(items[0]); i++)

#define streq(a, b) (strcmp((a), (b)) == 0)
#define strceq(a, b) (strcasecmp((a), (b)) == 0)
#define strnull(s) s ? s : "[null]"

static inline int blank_line_or_comment(const char *s) {
    while (isspace((unsigned char)*s)) {
        ++s;
    }
    return *s == '\0' || *s == '#';
}

static inline int parse_boolean(const char *s) {
    size_t i;
    const char *const truthy[] = {"1", "y", "yes", "true", "on"};
    const char *const falsy[] = {"0", "n", "no", "false", "off"};

    for_each_arr (i, truthy) {
        if (strceq(s, truthy[i])) {
            return 1;
        }
    }

    for_each_arr (i, falsy) {
        if (strceq(s, falsy[i])) {
            return 0;
        }
    }

    return -EINVAL;
}

static inline char *active_inactive(Alert *a) {
    return a->notif ? "active" : "inactive";
}

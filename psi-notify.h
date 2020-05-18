#include <stdbool.h>
#include <stdio.h>
#include <strings.h>

typedef enum ResourceType { RT_CPU, RT_MEMORY, RT_IO } ResourceType;

typedef struct {
    double some;
    double full;
} TimeResourcePressure;

typedef struct {
    TimeResourcePressure ten;
    TimeResourcePressure sixty;
    TimeResourcePressure three_hundred;
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
    unsigned int update_interval;
    bool log_pressures;
} Config;

#define info(format, ...) printf("INFO: " format, __VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "WARN: " format, __VA_ARGS__)
#define expect(x)                                                              \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "FATAL: !(%s) at %s:%s:%d\n", #x, __FILE__,        \
                    __func__, __LINE__);                                       \
            abort();                                                           \
        }                                                                      \
    } while (0)

#define snprintf_check(buf, len, fmt, ...)                                     \
    expect((size_t)snprintf(buf, len, fmt, __VA_ARGS__) < (len))

#define for_each_arr(i, items)                                                 \
    for (i = 0; i < sizeof(items) / sizeof(items[0]); i++)

#define streq(a, b) (strcmp((a), (b)) == 0)
#define strceq(a, b) (strcasecmp((a), (b)) == 0)

static inline int blank_line_or_comment(const char *s) {
    while (isspace((unsigned char)*s)) {
        ++s;
    }
    return *s == '\0' || *s == '#';
}

static inline int parse_boolean(const char *s) {
    size_t i;
    const char *const truthy[] = {"1", "yes", "true", "on"};
    const char *const falsy[] = {"0", "no", "false", "off"};

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

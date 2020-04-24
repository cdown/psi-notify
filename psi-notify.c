#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    float some;
    float full;
} TimeResourcePressure;

typedef struct {
    TimeResourcePressure ten;
    TimeResourcePressure sixty;
    TimeResourcePressure three_hundred;
} Pressure;

typedef struct {
    int available : 1;
    char *filename;
    char *name;
    Pressure pressure; /* only valid if available */
    Pressure thresholds;
} Resource;

typedef struct {
    Resource cpu;
    Resource memory;
    Resource io;
} Config;

char *get_pressure_file(char *resource) {
    char *path;

    path = malloc(PATH_MAX);

    if (!path) {
        perror("malloc");
        abort();
    }

    /*
     * If we have a logind seat for this user, use the pressure stats for that
     * seat's slice. Otherwise, use the system-global pressure stats.
     */
    (void)snprintf(path, PATH_MAX,
                   "/sys/fs/cgroup/user.slice/user-%d.slice/%s.pressure",
                   getuid(), resource);
    if (access(path, R_OK) == 0) {
        return path;
    }

    (void)snprintf(path, PATH_MAX, "/proc/pressure/%s", resource);
    if (access(path, R_OK) == 0) {
        return path;
    }

    free(path);
    return NULL;
}

void update_thresholds(Config *c) {
    /* TODO: get from config */
    c->cpu.thresholds.ten.some = 0.1f;
    c->memory.thresholds.sixty.some = 0.1f;
}

#define SET_NAMES(c, _name)                                                    \
    (c)->_name.filename = get_pressure_file(#_name);                           \
    (c)->_name.name = #_name;

Config *init_config(void) {
    Config *c;

    c = calloc(1, sizeof(Config));
    if (!c) {
        perror("calloc");
        abort();
    }

    SET_NAMES(c, cpu);
    SET_NAMES(c, memory);
    SET_NAMES(c, io);

    update_thresholds(c);

    return c;
}

void check_pressures(Config *c) {}

int main(int argc, char *argv[]) {
    Config *config = init_config();

    printf("%s\n", config->cpu.name);
    printf("%s\n", config->cpu.filename);

    while (1) {
        check_pressures(config);
        sleep(1);
    }
}

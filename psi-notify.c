#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    float some;
    float full;
} TimeResourcePressure;

typedef struct {
    int available : 1;
    char *filename;

    /* TRPs are only valid if available is true */
    TimeResourcePressure ten;
    TimeResourcePressure sixty;
    TimeResourcePressure three_hundred;
} ResourcePressure;

typedef struct {
    ResourcePressure cpu;
    ResourcePressure memory;
    ResourcePressure io;
} SystemPressure;

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

SystemPressure *init_system_pressure(void) {
    SystemPressure *sp;

    sp = calloc(1, sizeof(SystemPressure));
    if (!sp) {
        perror("calloc");
        abort();
    }

    sp->cpu.filename = get_pressure_file("cpu");
    sp->memory.filename = get_pressure_file("memory");
    sp->io.filename = get_pressure_file("io");

    return sp;
}

int main(int argc, char *argv[]) {
    SystemPressure *psi = init_system_pressure();
}

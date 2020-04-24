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
} ResourcePressure;

typedef struct {
    int available : 1;
    char *filename;
    ResourcePressure rp; /* only valid if available */
} FileResourcePressure;

typedef struct {
    FileResourcePressure cpu;
    FileResourcePressure memory;
    FileResourcePressure io;
} SystemPressure;

typedef struct {
    ResourcePressure cpu;
    ResourcePressure memory;
    ResourcePressure io;
} SystemThresholds;

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

SystemThresholds get_thresholds(void) {
    /* TODO: get from config */
    SystemThresholds thr;

    thr.cpu.ten.some = 0.1f;
    thr.memory.ten.some = 0.1f;

    return thr;
}

int main(int argc, char *argv[]) {
    SystemPressure *psi = init_system_pressure();
    SystemThresholds thr = get_thresholds();

    /*
     * TODO: Once PSI support unprivileged poll(), we should start using a real
     * event loop.
     *
     * https://lore.kernel.org/lkml/20200424153859.GA1481119@chrisdown.name/
     */
    while (1) {

        /* TODO: configurable by config */
        sleep(1);
    }

    printf("%s\n", psi->cpu.filename);
    printf("%f\n", thr.cpu.ten.some);
    printf("%f\n", thr.cpu.ten.full);
    printf("%f\n", thr.cpu.sixty.full);
}

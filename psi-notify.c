#include <errno.h>
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

/*
 * We don't care about total=, so that doesn't need consideration. Therefore
 * the max line len is len("some avg10=100.00 avg60=100.00 avg300=100.00").
 * However, we add a bit more for total= so that fgets can seek to the next
 * newline.
 *
 * We don't need to read in one go like old /proc files, since all of these are
 * backed by seq_file in the kernel.
 */
#define PRESSURE_LINE_LEN 64

/*
 * >0: Above thresholds
 *  0: Within thresholds
 * <0: Error
 */
int check_pressures(Resource *r, int has_full) {
    FILE *f;
    char line[PRESSURE_LINE_LEN];
    int ret;
    char *start;
    float ten, sixty, three_hundred;

    if (!r->filename) {
        return 0;
    }

    f = fopen(r->filename, "r");

    if (!f) {
        perror(r->filename);
        return -EINVAL;
    }

    start = fgets(line, sizeof(line), f);
    if (!start) {
        fprintf(stderr, "Premature EOF from %s\n", r->filename);
        return -EINVAL;
    }

    ret = sscanf(line, "%*s avg10=%f avg60=%f avg300=%f total=%*s", &ten,
                 &sixty, &three_hundred);
    if (ret != 3) {
        fprintf(stderr, "Can't parse 'some' from %s\n", r->filename);
        return -EINVAL;
    }

    if ((r->thresholds.ten.some && ten > r->thresholds.ten.some) ||
        (r->thresholds.sixty.some && sixty > r->thresholds.sixty.some) ||
        (r->thresholds.three_hundred.some &&
         three_hundred > r->thresholds.three_hundred.some)) {
        return 1;
    }

    if (!has_full) {
        return 0;
    }

    start = fgets(line, sizeof(line), f);
    if (!start) {
        fprintf(stderr, "Premature EOF from %s\n", r->filename);
        return -EINVAL;
    }

    ret = sscanf(line, "%*s avg10=%f avg60=%f avg300=%f total=%*s", &ten,
                 &sixty, &three_hundred);
    if (ret != 3) {
        fprintf(stderr, "Can't parse 'full' from %s\n", r->filename);
        return -EINVAL;
    }

    if ((r->thresholds.ten.full && ten > r->thresholds.ten.full) ||
        (r->thresholds.sixty.full && sixty > r->thresholds.sixty.full) ||
        (r->thresholds.three_hundred.full &&
         three_hundred > r->thresholds.three_hundred.full)) {
        return 1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    Config *config = init_config();

    /*
     * TODO: If discussion on unprivileged PSI poll() support upstream ends up
     * with patches, change this to use poll() and a real event loop.
     *
     * https://lore.kernel.org/lkml/20200424153859.GA1481119@chrisdown.name/
     */
    while (1) {
        if (check_pressures(&config->cpu, 0) > 0) {
            printf("CPU pressure high\n");
        }

        if (check_pressures(&config->memory, 1) > 0) {
            printf("Memory pressure high\n");
        }

        if (check_pressures(&config->io, 1) > 0) {
            printf("IO pressure high\n");
        }

        /* TODO: get from config */
        sleep(1);
    }
}

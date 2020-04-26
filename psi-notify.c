#include <ctype.h>
#include <errno.h>
#include <libnotify/notify.h>
#include <linux/limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#define expect(x)                                                              \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "FATAL: !(%s) at %s:%d\n", #x, __FILE__,           \
                    __LINE__);                                                 \
            abort();                                                           \
        }                                                                      \
    } while (0)

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
    char *filename;
    Pressure thresholds;
} Resource;

typedef struct {
    Resource cpu;
    Resource memory;
    Resource io;
    unsigned int update_interval;
} Config;

static int config_reload_pending = 0; /* SIGHUP */

static void sighup_handler(int sig) {
    expect(sig == SIGHUP);
    config_reload_pending = 1;
}

static void configure_sighup_handler(void) {
    const struct sigaction sighup = {
        .sa_handler = sighup_handler,
        .sa_flags = SA_RESTART,
    };
    expect(sigaction(SIGHUP, &sighup, NULL) >= 0);
}

static char *get_pressure_file(char *resource) {
    char *path;

    path = malloc(PATH_MAX);
    expect(path);

    /*
     * If we have a logind seat for this user, use the pressure stats for that
     * seat's slice on cgroup v2. Otherwise, use the system-global pressure
     * stats.
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

    fprintf(stderr,
            "Couldn't find any pressure file for resource %s, skipping\n",
            resource);
    free(path);
    return NULL;
}

#define CONFIG_LINE_MAX 256

static void update_threshold(Config *c, const char *line) {
    int ret;
    char resource[CONFIG_LINE_MAX], type[CONFIG_LINE_MAX],
        interval[CONFIG_LINE_MAX];
    float threshold;
    Resource *r;
    TimeResourcePressure *t;

    ret = sscanf(line, "%*s %s %s %s %f", resource, type, interval, &threshold);
    if (ret != 4) {
        fprintf(stderr, "Invalid threshold, ignoring: %s", line);
        return;
    }

    if (!threshold) {
        fprintf(stderr,
                "Zero threshold for %s::%s::%s disables checks, ignoring: %f\n",
                resource, type, interval, threshold);
        return;
    }

    if (strcmp(resource, "cpu") == 0) {
        r = &c->cpu;
    } else if (strcmp(resource, "memory") == 0) {
        r = &c->memory;
    } else if (strcmp(resource, "io") == 0) {
        r = &c->io;
    } else {
        fprintf(stderr, "Invalid resource in config, ignoring: '%s'\n",
                resource);
        return;
    }

    if (strcmp(interval, "avg10") == 0) {
        t = &r->thresholds.ten;
    } else if (strcmp(interval, "avg60") == 0) {
        t = &r->thresholds.sixty;
    } else if (strcmp(interval, "avg300") == 0) {
        t = &r->thresholds.three_hundred;
    } else {
        fprintf(stderr, "Invalid interval in config, ignoring: '%s'\n",
                interval);
        return;
    }

    if (strcmp(type, "some") == 0) {
        t->some = threshold;
    } else if (strcmp(type, "full") == 0) {
        if (strcmp(resource, "cpu") == 0) {
            fprintf(stderr, "full interval for CPU is bogus, ignoring\n");
            return;
        }
        t->full = threshold;
    } else {
        fprintf(stderr, "Invalid type in config, ignoring: '%s'\n", type);
        return;
    }
}

static int is_blank(const char *s) {
    while (isspace((unsigned char)*s))
        s++;
    return *s == '\0';
}

static void update_config(Config *c) {
    struct passwd *pw = getpwuid(getuid());
    char line[CONFIG_LINE_MAX];
    char config_path[PATH_MAX];
    FILE *f;

    expect(pw);

    /* defaults */
    memset(&c->cpu.thresholds, 0, sizeof(c->cpu.thresholds));
    memset(&c->memory.thresholds, 0, sizeof(c->memory.thresholds));
    memset(&c->io.thresholds, 0, sizeof(c->io.thresholds));
    c->update_interval = 10;

    (void)snprintf(config_path, PATH_MAX, "%s/.config/psi-notify", pw->pw_dir);

    f = fopen(config_path, "r");
    expect(f);

    while (fgets(line, sizeof(line), f)) {
        int ret;
        char lvalue[CONFIG_LINE_MAX];
        unsigned int rvalue;

        if (is_blank(line)) {
            continue;
        }

        if (line[0] == '#') {
            continue;
        }

        ret = sscanf(line, "%s", lvalue);
        if (ret != 1) {
            fprintf(stderr, "Invalid config line, ignoring: %s", line);
            continue;
        }

        if (strcmp(lvalue, "threshold") == 0) {
            update_threshold(c, line);
        } else if (strcmp(lvalue, "update") == 0) {
            ret = sscanf(line, "%s %d", lvalue, &rvalue);
            c->update_interval = rvalue;
        } else {
            fprintf(stderr, "Invalid config line, ignoring: %s", line);
            continue;
        }
    }
}

static Config *init_config(void) {
    Config *c;

    c = calloc(1, sizeof(Config));
    expect(c);

    c->cpu.filename = get_pressure_file("cpu");
    c->memory.filename = get_pressure_file("memory");
    c->io.filename = get_pressure_file("io");

    update_config(c);

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

#define COMPARE_THRESH(threshold, current) (threshold && current > threshold)

static int _check_pressures(FILE *f, Resource *r) {
    char *start;
    char line[PRESSURE_LINE_LEN];
    char type[CONFIG_LINE_MAX];
    float ten, sixty, three_hundred;
    int ret = 0;

    start = fgets(line, sizeof(line), f);
    if (!start) {
        fprintf(stderr, "Premature EOF from %s\n", r->filename);
        return -EINVAL;
    }

    ret = sscanf(line, "%s avg10=%f avg60=%f avg300=%f total=%*s", type, &ten,
                 &sixty, &three_hundred);
    if (ret != 4) {
        fprintf(stderr, "Can't parse from %s: %s\n", r->filename, line);
        return -EINVAL;
    }

    if (strcmp("some", type) == 0) {
        return COMPARE_THRESH(r->thresholds.ten.some, ten) ||
               COMPARE_THRESH(r->thresholds.sixty.some, sixty) ||
               COMPARE_THRESH(r->thresholds.three_hundred.some, three_hundred);
    } else if (strcmp("full", type) == 0) {
        return COMPARE_THRESH(r->thresholds.ten.full, ten) ||
               COMPARE_THRESH(r->thresholds.sixty.full, sixty) ||
               COMPARE_THRESH(r->thresholds.three_hundred.full, three_hundred);
    }

    fprintf(stderr, "Invalid type: %s\n", type);
    return -EINVAL;
}

/*
 * >0: Above thresholds
 *  0: Within thresholds
 * <0: Error
 */
static int check_pressures(Resource *r, int has_full) {
    FILE *f;
    int ret = 0;

    if (!r->filename) {
        return 0;
    }

    f = fopen(r->filename, "r");

    /* Not expect(), since the file might legitimately go away */
    if (!f) {
        perror(r->filename);
        ret = -EINVAL;
        goto out;
    }

    ret = _check_pressures(f, r);
    if (ret) {
        goto out;
    }

    if (!has_full) {
        ret = 0;
        goto out;
    }

    ret = _check_pressures(f, r);
    if (ret) {
        goto out;
    }

    ret = 0;

out:
    if (f)
        fclose(f);
    return ret;
}

#define TITLE_MAX 32

static void notify(const char *resource) {
    char *title = alloca(TITLE_MAX);
    NotifyNotification *n;

    expect(notify_is_initted());

    (void)snprintf(title, TITLE_MAX, "High %s pressure!", resource);

    n = notify_notification_new(
        title, "Consider reducing demand on this resource.", NULL);
    notify_notification_set_urgency(n, NOTIFY_URGENCY_CRITICAL);
    (void)notify_notification_show(n, NULL);
    g_object_unref(n);
}

static const char *strnull(const char *s) { return s ? s : "(null)"; }

int main(void) {
    Config *config = init_config();

    printf("Pressure paths:\n");
    printf("- CPU:    %s\n", strnull(config->cpu.filename));
    printf("- Memory: %s\n", strnull(config->memory.filename));
    printf("- I/O:    %s\n", strnull(config->io.filename));

    configure_sighup_handler();
    notify_init("psi-notify");

    /*
     * TODO: If discussion on unprivileged PSI poll() support upstream ends up
     * with patches, change this to use poll() and a real event loop.
     *
     * https://lore.kernel.org/lkml/20200424153859.GA1481119@chrisdown.name/
     */
    while (1) {
        if (config_reload_pending) {
            update_config(config);
            printf("Config reloaded.\n");
            config_reload_pending = 0;
        }

        if (check_pressures(&config->cpu, 0) > 0) {
            notify("CPU");
        }

        if (check_pressures(&config->memory, 1) > 0) {
            notify("memory");
        }

        if (check_pressures(&config->io, 1) > 0) {
            notify("I/O");
        }

        sleep(config->update_interval);
    }
}

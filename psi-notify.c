#include <ctype.h>
#include <errno.h>
#include <libnotify/notify.h>
#include <linux/limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef WANT_SD_NOTIFY
    #include <systemd/sd-daemon.h>
#else /* !WANT_SD_NOTIFY */
    #define sd_notify(reset_env, state)                                        \
        do {                                                                   \
        } while (0)
    #define sd_notifyf(reset_env, fmt, ...)                                    \
        do {                                                                   \
        } while (0)
#endif /* WANT_SD_NOTIFY */

#define info(format, ...) printf("INFO: " format, __VA_ARGS__)
#define warn(format, ...) printf("WARN: " format, __VA_ARGS__)
#define expect(x)                                                              \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "FATAL: !(%s) at %s:%s:%d\n", #x, __FILE__,        \
                    __func__, __LINE__);                                       \
            abort();                                                           \
        }                                                                      \
    } while (0)

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
} Config;

static volatile sig_atomic_t config_reload_pending = 0; /* SIGHUP */
static volatile sig_atomic_t run = 1;                   /* SIGTERM, SIGINT */

static NotifyNotification *active_notif[] = {
    [RT_CPU] = NULL,
    [RT_MEMORY] = NULL,
    [RT_IO] = NULL,
};

static void request_reload_config(int sig) {
    (void)sig;
    config_reload_pending = 1;
}

static void request_exit(int sig) {
    (void)sig;
    run = 0;
}

static void configure_signal_handlers(void) {
    const struct sigaction sa_exit = {
        .sa_handler = request_exit,
    };
    expect(
        sigaction(SIGHUP,
                  &(const struct sigaction){.sa_handler = request_reload_config,
                                            .sa_flags = SA_RESTART},
                  NULL) >= 0);
    expect(sigaction(SIGTERM, &sa_exit, NULL) >= 0);
    expect(sigaction(SIGINT, &sa_exit, NULL) >= 0);
}

static void alert_destroy(NotifyNotification *n) {
    (void)notify_notification_close(n, NULL);
    g_object_unref(G_OBJECT(n));
}

#define TITLE_MAX 22 /* len(b"High memory pressure!\0") */

static NotifyNotification *alert_user(const char *resource) {
    char title[TITLE_MAX];
    NotifyNotification *n;
    GError *err = NULL;

    expect(notify_is_initted());

    expect(snprintf(title, TITLE_MAX, "High %s pressure!", resource) > 0);
    n = notify_notification_new(
        title, "Consider reducing demand on this resource.", NULL);
    notify_notification_set_urgency(n, NOTIFY_URGENCY_CRITICAL);

    if (!notify_notification_show(n, &err)) {
        warn("Cannot display notification: %s\n", err->message);
        g_error_free(err);
        alert_destroy(n);
        n = NULL;
    }

    return n;
}

static void alert_destroy_all_active(void) {
    size_t i;
    for (i = 0; i < sizeof(active_notif) / sizeof(active_notif[0]); i++) {
        if (active_notif[i]) {
            NotifyNotification *n = active_notif[i];
            active_notif[i] = NULL;
            alert_destroy(n);
        }
    }
}

/* len(b"/sys/fs/cgroup/user.slice/user-2147483647.slice/memory.pressure\0") */
#define PRESSURE_PATH_MAX 64

static char *get_psi_filename(char *resource) {
    char *path;

    path = malloc(PRESSURE_PATH_MAX);
    expect(path);

    expect(snprintf(path, PRESSURE_PATH_MAX,
                    "/sys/fs/cgroup/user.slice/user-%d.slice/%s.pressure",
                    getuid(), resource) > 0);
    if (access(path, R_OK) == 0) {
        return path;
    }

    expect(snprintf(path, PRESSURE_PATH_MAX, "/proc/pressure/%s", resource) >
           0);
    if (access(path, R_OK) == 0) {
        return path;
    }

    warn("Couldn't find any pressure file for resource %s, skipping\n",
         resource);
    free(path);
    return NULL;
}

#define CONFIG_LINE_MAX 256

static void threshold_update(Config *c, const char *line) {
    char resource[CONFIG_LINE_MAX], type[CONFIG_LINE_MAX],
        interval[CONFIG_LINE_MAX];
    double threshold;
    Resource *r;
    TimeResourcePressure *t;

    /* line is clamped to CONFIG_LINE_MAX, so formats cannot be wider */
    if (sscanf(line, "%*s %s %s %s %lf", resource, type, interval,
               &threshold) != 4) {
        warn("Invalid threshold, ignoring: %s", line);
        return;
    }

    if (threshold < 0) {
        warn("Invalid threshold for %s::%s::%s, ignoring: %f\n", resource, type,
             interval, threshold);
        return;
    }

    if (strcmp(resource, "cpu") == 0) {
        r = &c->cpu;
    } else if (strcmp(resource, "memory") == 0) {
        r = &c->memory;
    } else if (strcmp(resource, "io") == 0) {
        r = &c->io;
    } else {
        warn("Invalid resource in config, ignoring: '%s'\n", resource);
        return;
    }

    if (strcmp(interval, "avg10") == 0) {
        t = &r->thresholds.ten;
    } else if (strcmp(interval, "avg60") == 0) {
        t = &r->thresholds.sixty;
    } else if (strcmp(interval, "avg300") == 0) {
        t = &r->thresholds.three_hundred;
    } else {
        warn("Invalid interval in config, ignoring: '%s'\n", interval);
        return;
    }

    if (strcmp(type, "some") == 0) {
        t->some = threshold;
    } else if (strcmp(type, "full") == 0) {
        if (strcmp(resource, "cpu") == 0) {
            warn("Full interval for %s is bogus, ignoring\n", resource);
            return;
        }
        t->full = threshold;
    } else {
        warn("Invalid type in config, ignoring: '%s'\n", type);
        return;
    }
}

static int is_blank(const char *s) {
    while (isspace((unsigned char)*s))
        s++;
    return *s == '\0';
}

static void config_reset_user_facing(Config *c) {
    c->update_interval = 5;

    /* -nan */
    memset(&c->cpu.thresholds, 0xff, sizeof(c->cpu.thresholds));
    memset(&c->memory.thresholds, 0xff, sizeof(c->memory.thresholds));
    memset(&c->io.thresholds, 0xff, sizeof(c->io.thresholds));
}

#define WATCHDOG_GRACE_PERIOD_SEC 5
#define SEC_TO_USEC 1000000
static void watchdog_update_usec(Config *c) {
    expect(c->update_interval > 0);
    sd_notifyf(0, "WATCHDOG_USEC=%d",
               (c->update_interval + WATCHDOG_GRACE_PERIOD_SEC) * SEC_TO_USEC);
}

static int config_update_from_file(Config *c) {
    struct passwd *pw = getpwuid(getuid());
    char line[CONFIG_LINE_MAX];
    char config_path[PATH_MAX];
    char *base_dir;
    FILE *f;
    int ret = 0;

    base_dir = getenv("XDG_CONFIG_DIR");

    if (base_dir) {
        expect(snprintf(config_path, PATH_MAX, "%s/psi-notify", base_dir) > 0);
    } else {
        base_dir = getenv("HOME");
        if (!base_dir) {
            if (pw) {
                base_dir = pw->pw_dir;
            } else {
                warn("%s\n",
                     "No $XDG_CONFIG_DIR, $HOME, or entry in /etc/passwd?");
                base_dir = "/";
            }
        }

        expect(snprintf(config_path, PATH_MAX, "%s/.config/psi-notify",
                        base_dir) > 0);
    }

    f = fopen(config_path, "re");

    if (f) {
        config_reset_user_facing(c);
    } else {
        if (config_reload_pending) {
            /* This was from a SIGHUP, so we already have a config. Keep it. */
            warn("Config reload request ignored, cannot open %s: %s\n",
                 config_path, strerror(errno));
            return -errno;
        }

        if (errno == ENOENT) {
            info("No config at %s, using defaults\n", config_path);
        } else {
            warn("Using default config, cannot open %s: %s\n", config_path,
                 strerror(errno));
        }

        config_reset_user_facing(c);

        c->cpu.thresholds.ten.some = 50.00;
        c->memory.thresholds.ten.some = 10.00;
        c->io.thresholds.ten.some = 10.00;

        ret = -errno;
        goto out_update_watchdog;
    }

    while (fgets(line, sizeof(line), f)) {
        char lvalue[CONFIG_LINE_MAX];
        unsigned int rvalue;
        size_t len = strlen(line);

        if (is_blank(line)) {
            continue;
        }

        if (line[0] == '#') {
            continue;
        }

        if (len == CONFIG_LINE_MAX - 1 && line[len - 1] != '\n') {
            int ch;
            warn("Config line is too long to be valid, ignoring: %s\n", line);
            while ((ch = fgetc(f)) != EOF && ch != '\n')
                ;
            continue;
        }

        if (sscanf(line, "%s", lvalue) != 1) {
            warn("Invalid config line, ignoring: %s", line);
            continue;
        }

        if (strcmp(lvalue, "threshold") == 0) {
            threshold_update(c, line);
        } else if (strcmp(lvalue, "update") == 0) {
            if (sscanf(line, "%s %u", lvalue, &rvalue) != 2) {
                warn("Invalid config line, ignoring: %s", line);
                continue;
            }
            if (rvalue <= 0) {
                warn("Ignoring <= 0 update interval: %d\n", rvalue);
                continue;
            }
            if (rvalue > 1800) {
                /* WATCHDOG_USEC must still fit in a uint */
                warn("Clamping update interval to 1800 from %d\n", rvalue);
                rvalue = 1800;
            }

            c->update_interval = rvalue;
        } else {
            warn("Invalid config line, ignoring: %s", line);
            continue;
        }
    }

    fclose(f);

out_update_watchdog:
    watchdog_update_usec(c);

    return ret;
}

static void config_init(Config *c) {
    memset(c, 0, sizeof(Config));

    c->cpu.filename = get_psi_filename("cpu");
    c->cpu.type = RT_CPU;
    c->cpu.human_name = "CPU";
    c->cpu.has_full = 0;

    c->memory.filename = get_psi_filename("memory");
    c->memory.type = RT_MEMORY;
    c->memory.human_name = "memory";
    c->memory.has_full = 1;

    c->io.filename = get_psi_filename("io");
    c->io.type = RT_IO;
    c->io.human_name = "I/O";
    c->io.has_full = 1;

    (void)config_update_from_file(c);
}

/*
 * 64 is len("some avg10=100.00 avg60=100.00 avg300=100.00") + a bit more to
 * make sure fgets() reads past total= and seeks up to \n.
 */
#define PRESSURE_LINE_LEN 64

#define COMPARE_THRESH(threshold, current)                                     \
    (threshold >= 0 && current > threshold)

static int pressure_check_single_line(FILE *f, Resource *r) {
    char *start;
    char line[PRESSURE_LINE_LEN];
    char type[PRESSURE_LINE_LEN];
    double ten, sixty, three_hundred;

    start = fgets(line, sizeof(line), f);
    if (!start) {
        warn("Premature EOF from %s\n", r->filename);
        return -EINVAL;
    }

    info("Current %s pressures: %s", r->human_name, line);

    if (sscanf(line, "%s avg10=%lf avg60=%lf avg300=%lf total=%*s", type, &ten,
               &sixty, &three_hundred) != 4) {
        warn("Can't parse from %s: %s\n", r->filename, line);
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

    warn("Invalid type: %s\n", type);
    return -EINVAL;
}

/* >0: above thresholds, 0: within thresholds, <0: error */
static int pressure_check(Resource *r) {
    FILE *f;
    int ret = 0;

    if (!r->filename) {
        return 0;
    }

    f = fopen(r->filename, "re");

    if (!f) {
        perror(r->filename);
        return -EINVAL;
    }

    ret = pressure_check_single_line(f, r);
    if (ret) {
        goto out_fclose;
    }

    if (!r->has_full) {
        ret = 0;
        goto out_fclose;
    }

    ret = pressure_check_single_line(f, r);
    if (ret) {
        goto out_fclose;
    }

    ret = 0;

out_fclose:
    fclose(f);
    return ret;
}

#define LOG_ALERT_STATE(r, state)                                              \
    expect(*r->human_name);                                                    \
    info("%c%s alert: %s\n", toupper(r->human_name[0]), r->human_name + 1,     \
         state)

/* 0 means already active, 1 means newly active. */
static int alert_user_if_new(Resource *r) {
    if (active_notif[r->type]) {
        /* We already have an active warning, nothing to do. */
        return 0;
    }

    LOG_ALERT_STATE(r, "active");
    active_notif[r->type] = alert_user(r->human_name);
    return 1;
}

/* 0 means already inactive, 1 means newly inactive. */
static int alert_stop(Resource *r) {
    NotifyNotification *n = active_notif[r->type];

    if (!n) {
        /* Already inactive, nothing to do. */
        return 0;
    }

    LOG_ALERT_STATE(r, "inactive");
    active_notif[r->type] = NULL;
    alert_destroy(n);

    return 1;
}

static void pressure_check_notify_if_new(Resource *r) {
    int ret = pressure_check(r);

    switch (ret) {
    case 0:
        alert_stop(r);
        break;
    case 1:
        alert_user_if_new(r);
        break;
    default:
        warn("Error getting %s pressure: %s\n", r->human_name, strerror(ret));
        break;
    }
}

#define SEC_TO_NSEC 1000000000

static void suspend_for_remaining_interval(Config *c, struct timespec *in) {
    struct timespec out, remaining;

    expect(clock_gettime(CLOCK_MONOTONIC, &out) == 0);

    if (out.tv_nsec - in->tv_nsec < 0) {
        remaining.tv_sec = out.tv_sec - in->tv_sec - 1;
        remaining.tv_nsec = out.tv_nsec - in->tv_nsec + SEC_TO_NSEC;
    } else {
        remaining.tv_sec = out.tv_sec - in->tv_sec;
        remaining.tv_nsec = out.tv_nsec - in->tv_nsec;
    }

    remaining.tv_sec = (c->update_interval - remaining.tv_sec - 1);
    remaining.tv_nsec = (SEC_TO_NSEC - remaining.tv_nsec);

    if (remaining.tv_nsec == SEC_TO_NSEC) {
        remaining.tv_sec += 1;
        remaining.tv_nsec = 0;
    }

    if (remaining.tv_sec >= c->update_interval) {
        warn("Timer elapsed %d seconds before we completed one event loop\n",
             c->update_interval);
        return;
    }

    expect(nanosleep(&remaining, NULL) == 0 || errno == EINTR);
}

int main(int argc, char *argv[]) {
    Config config;
    bool in_fuzzer = getenv("FUZZ");

    (void)argv;

    if (argc != 1) {
        warn("%s doesn't accept any arguments.\n", argv[0]);
        return 1;
    }

    expect(setvbuf(stdout, NULL, _IONBF, 0) == 0);
    config_init(&config);
    configure_signal_handlers();
    expect(notify_init("psi-notify"));

    /*
     * TODO: If discussion on unprivileged PSI poll() support upstream ends up
     * with patches, change this to use poll() and a real event loop.
     *
     * https://lore.kernel.org/lkml/20200424153859.GA1481119@chrisdown.name/
     */
    while (run) {
        struct timespec in;

        if (in_fuzzer) {
            /* Running under AFL, just run once. */
            run = 0;
        }

        expect(clock_gettime(CLOCK_MONOTONIC, &in) == 0);

        sd_notify(0, "READY=1\nWATCHDOG=1\n"
                     "STATUS=Checking current pressures...");
        pressure_check_notify_if_new(&config.cpu);
        pressure_check_notify_if_new(&config.memory);
        pressure_check_notify_if_new(&config.io);

        if (config_reload_pending) {
            sd_notify(0, "RELOADING=1\nSTATUS=Reloading config...");
            if (config_update_from_file(&config) == 0) {
                printf("Config reloaded.\n");
            }
            config_reload_pending = 0;
        } else if (run) {
            sd_notify(0, "STATUS=Waiting for next interval.");
            suspend_for_remaining_interval(&config, &in);
        }
    }

    sd_notify(0, "STOPPING=1\nSTATUS=Tearing down...");

    free(config.cpu.filename);
    free(config.memory.filename);
    free(config.io.filename);
    alert_destroy_all_active();
    notify_uninit();
}

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libnotify/notify.h>
#include <linux/limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include "psi-notify.h"

#ifdef WANT_SD_NOTIFY
    #include <systemd/sd-daemon.h>
#else /* !WANT_SD_NOTIFY */
    #define sd_notifyf(reset_env, fmt, ...)                                    \
        do {                                                                   \
        } while (0)
    #define sd_notify(reset_env, state) sd_notifyf(reset_env, "%s", state)
#endif /* WANT_SD_NOTIFY */

static volatile sig_atomic_t config_reload_pending = 0; /* SIGHUP */
static volatile sig_atomic_t run = 1;                   /* SIGTERM, SIGINT */

static Config cfg;
static char output_buf[512];
static Resource *all_res[] = {&cfg.cpu, &cfg.memory, &cfg.io};
static bool using_seat = false;

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
    if (!run) {
        /* Asked to quit twice now, skip teardown. */
        _exit(128 + sig);
    }

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

#define TITLE_MAX sizeof("High memory pressure!")

static NotifyNotification *alert_user(const char *resource) {
    char title[TITLE_MAX];
    NotifyNotification *n;
    GError *err = NULL;

    expect(notify_is_initted());

    snprintf_check(title, TITLE_MAX, "High %s pressure!", resource);
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
    for_each_arr (i, active_notif) {
        if (active_notif[i]) {
            NotifyNotification *n = active_notif[i];
            active_notif[i] = NULL;
            alert_destroy(n);
        }
    }
}

#define PRESSURE_FILE_PATH_MAX sizeof("memory.pressure")

static void get_psi_dir_and_filename(Resource *r, char *resource) {
    int dir_fd;
    char dir_path[PATH_MAX];
    char *path;

    path = malloc(PRESSURE_FILE_PATH_MAX);
    expect(path);

    snprintf_check(dir_path, PATH_MAX,
                   "/sys/fs/cgroup/user.slice/user-%d.slice", getuid());

    dir_fd = open(dir_path, O_RDONLY);
    if (dir_fd) {
        using_seat = true;
        r->dir_fd = dir_fd;
        snprintf_check(path, PRESSURE_FILE_PATH_MAX, "%s.pressure", resource);
        r->filename = path;
        return;
    }

    dir_fd = open("/proc/pressure", O_RDONLY);
    if (dir_fd) {
        r->dir_fd = dir_fd;
        snprintf_check(path, PRESSURE_FILE_PATH_MAX, "%s", resource);
        r->filename = path;
        return;
    }

    warn("Couldn't find any pressure file for resource %s, skipping\n",
         resource);
    free(path);
}

#define CONFIG_LINE_MAX 256

static void config_update_threshold(const char *line) {
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

    if (streq(resource, "cpu")) {
        r = &cfg.cpu;
    } else if (streq(resource, "memory")) {
        r = &cfg.memory;
    } else if (streq(resource, "io")) {
        r = &cfg.io;
    } else {
        warn("Invalid resource in config, ignoring: '%s'\n", resource);
        return;
    }

    if (streq(interval, "avg10")) {
        t = &r->thresholds.avg10;
    } else if (streq(interval, "avg60")) {
        t = &r->thresholds.avg60;
    } else if (streq(interval, "avg300")) {
        t = &r->thresholds.avg300;
    } else {
        warn("Invalid interval in config, ignoring: '%s'\n", interval);
        return;
    }

    if (streq(type, "some")) {
        t->some = threshold;
    } else if (streq(type, "full")) {
        if (streq(resource, "cpu")) {
            warn("Full interval for %s is bogus, ignoring\n", resource);
            return;
        }
        t->full = threshold;
    } else {
        warn("Invalid type in config, ignoring: '%s'\n", type);
        return;
    }
}

static void config_update_interval(const char *line) {
    int rvalue;

    if (sscanf(line, "%*s %d", &rvalue) != 1) {
        warn("Invalid config line, ignoring: %s", line);
        return;
    }

    if (rvalue < 0) {
        warn("Ignoring <0 update interval: %d\n", rvalue);
        return;
    }

    if (rvalue > 1800) {
        /* WATCHDOG_USEC must still fit in a uint */
        warn("Clamping update interval to 1800 from %d\n", rvalue);
        rvalue = 1800;
    }

    /* Signed at first to avoid %u shenanigans with negatives */
    cfg.update_interval = (unsigned int)rvalue;
}

static void config_update_log_pressures(const char *line) {
    char rvalue[CONFIG_LINE_MAX];
    int ret;

    if (sscanf(line, "%*s %s", rvalue) != 1) {
        warn("Invalid config line, ignoring: %s", line);
        return;
    }

    ret = parse_boolean(rvalue);
    if (ret < 0) {
        warn("Invalid bool for log_pressures, ignoring: %s\n", rvalue);
        return;
    }

    cfg.log_pressures = ret;
}

static void config_reset_user_facing(void) {
    cfg.update_interval = 5;
    cfg.log_pressures = false;

    /* -nan */
    memset(&cfg.cpu.thresholds, 0xff, sizeof(cfg.cpu.thresholds));
    memset(&cfg.memory.thresholds, 0xff, sizeof(cfg.memory.thresholds));
    memset(&cfg.io.thresholds, 0xff, sizeof(cfg.io.thresholds));
}

#define WATCHDOG_GRACE_PERIOD_SEC 5
#define SEC_TO_USEC 1000000
static void watchdog_update_usec(void) {
    sd_notifyf(0, "WATCHDOG_USEC=%d",
               (cfg.update_interval + WATCHDOG_GRACE_PERIOD_SEC) * SEC_TO_USEC);
}

static void config_get_path(char *out) {
    char *base_dir = getenv("XDG_CONFIG_DIR");
    const struct passwd *pw = getpwuid(getuid());

    if (base_dir) {
        snprintf_check(out, PATH_MAX, "%s/psi-notify", base_dir);
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

        snprintf_check(out, PATH_MAX, "%s/.config/psi-notify", base_dir);
    }
}

static int config_update_from_file(void) {
    char line[CONFIG_LINE_MAX];
    char config_path[PATH_MAX];
    FILE *f;
    int ret = 0;

    config_get_path(config_path);
    f = fopen(config_path, "re");

    if (f) {
        config_reset_user_facing();
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

        config_reset_user_facing();

        cfg.cpu.thresholds.avg10.some = 50.00;
        cfg.memory.thresholds.avg10.some = 10.00;
        cfg.io.thresholds.avg10.some = 10.00;

        ret = -errno;
        goto out_update_watchdog;
    }

    while (fgets(line, sizeof(line), f)) {
        char lvalue[CONFIG_LINE_MAX];
        size_t len = strlen(line);

        if (blank_line_or_comment(line)) {
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

        if (streq(lvalue, "threshold")) {
            config_update_threshold(line);
        } else if (streq(lvalue, "update")) {
            config_update_interval(line);
        } else if (streq(lvalue, "log_pressures")) {
            config_update_log_pressures(line);
        } else {
            warn("Invalid config line, ignoring: %s", line);
            continue;
        }
    }

    fclose(f);

out_update_watchdog:
    watchdog_update_usec();

    return ret;
}

static void config_init() {
    memset(&cfg, 0, sizeof(Config));

    get_psi_dir_and_filename(&cfg.cpu, "cpu");
    cfg.cpu.type = RT_CPU;
    cfg.cpu.human_name = "CPU";
    cfg.cpu.has_full = 0;

    get_psi_dir_and_filename(&cfg.memory, "memory");
    cfg.memory.type = RT_MEMORY;
    cfg.memory.human_name = "memory";
    cfg.memory.has_full = 1;

    get_psi_dir_and_filename(&cfg.io, "io");
    cfg.io.type = RT_IO;
    cfg.io.human_name = "I/O";
    cfg.io.has_full = 1;

    (void)config_update_from_file();
}

/*
 * 64 is len("some avg10=100.00 avg60=100.00 avg300=100.00") + a bit more to
 * make sure fgets() reads past total= and seeks up to \n.
 */
#define PRESSURE_LINE_LEN 64
#define PRESSURE_LINE_LEN_STR "%63s"

#define COMPARE_THRESH(threshold, current)                                     \
    (threshold >= 0 && current > threshold)

static int pressure_check_single_line(FILE *f, const Resource *r) {
    char type[PRESSURE_LINE_LEN];
    double avg10, avg60, avg300;

    if (fscanf(f,
               PRESSURE_LINE_LEN_STR
               " avg10=%lf avg60=%lf avg300=%lf total=%*s",
               type, &avg10, &avg60, &avg300) != 4) {
        warn("Can't parse pressures from %s\n", r->filename);
        return -EINVAL;
    }

    if (cfg.log_pressures) {
        info("Current %s pressures: %s avg10=%.2f avg60=%.2f avg300=%.2f\n",
             r->human_name, type, avg10, avg60, avg300);
    }

    if (streq(type, "some")) {
        return COMPARE_THRESH(r->thresholds.avg10.some, avg10) ||
               COMPARE_THRESH(r->thresholds.avg60.some, avg60) ||
               COMPARE_THRESH(r->thresholds.avg300.some, avg300);
    } else if (streq(type, "full")) {
        return COMPARE_THRESH(r->thresholds.avg10.full, avg10) ||
               COMPARE_THRESH(r->thresholds.avg60.full, avg60) ||
               COMPARE_THRESH(r->thresholds.avg300.full, avg300);
    }

    warn("Invalid type: %s\n", type);
    return -EINVAL;
}

/* >0: above thresholds, 0: within thresholds, <0: error */
static int pressure_check(const Resource *r) {
    FILE *f;
    int fd;
    int ret = 0;

    if (!r->filename) {
        return 0;
    }

    fd = openat(r->dir_fd, r->filename, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror(r->filename);
        return -EINVAL;
    }

    f = fdopen(fd, "r"); /* O_CLOEXEC is passed through */

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
static int alert_user_if_new(const Resource *r) {
    if (active_notif[r->type]) {
        return 0;
    }

    LOG_ALERT_STATE(r, "active");
    active_notif[r->type] = alert_user(r->human_name);
    return 1;
}

/* 0 means already inactive, 1 means newly inactive. */
static int alert_stop(const Resource *r) {
    NotifyNotification *n = active_notif[r->type];

    if (!n) {
        return 0;
    }

    LOG_ALERT_STATE(r, "inactive");
    active_notif[r->type] = NULL;
    alert_destroy(n);

    return 1;
}

static void pressure_check_notify_if_new(const Resource *r) {
    int ret = pressure_check(r);

    switch (ret) {
        case 0:
            alert_stop(r);
            break;
        case 1:
            alert_user_if_new(r);
            break;
        default:
            warn("Error getting %s pressure: %s\n", r->human_name,
                 strerror(abs(ret)));
            break;
    }
}

#define SEC_TO_NSEC 1000000000

static void suspend_for_remaining_interval(const struct timespec *in) {
    struct timespec out, remaining;

    if (cfg.update_interval == 0) {
        return;
    }

    expect(clock_gettime(CLOCK_MONOTONIC, &out) == 0);

    if (out.tv_nsec - in->tv_nsec < 0) {
        remaining.tv_sec = out.tv_sec - in->tv_sec - 1;
        remaining.tv_nsec = out.tv_nsec - in->tv_nsec + SEC_TO_NSEC;
    } else {
        remaining.tv_sec = out.tv_sec - in->tv_sec;
        remaining.tv_nsec = out.tv_nsec - in->tv_nsec;
    }

    remaining.tv_sec = (cfg.update_interval - remaining.tv_sec - 1);
    remaining.tv_nsec = (SEC_TO_NSEC - remaining.tv_nsec);

    if (remaining.tv_nsec == SEC_TO_NSEC) {
        remaining.tv_sec += 1;
        remaining.tv_nsec = 0;
    }

    if (remaining.tv_sec >= cfg.update_interval) {
        warn("Timer elapsed %d seconds before we completed one event loop\n",
             cfg.update_interval);
        return;
    }

    expect(nanosleep(&remaining, NULL) == 0 || errno == EINTR);
}

/* If running under AFL, just run the code and exit. Returns 1 if fuzzing. */
static int check_fuzzers(void) {
    char *fuzz_pressure_file = getenv("FUZZ_PRESSURES");

    if (fuzz_pressure_file) {
        Resource r;
        memset(&r, 0, sizeof(r));
        r.filename = fuzz_pressure_file;
        r.human_name = "FUZZ";
        (void)pressure_check(&r);
        return 1;
    }

    if (getenv("FUZZ_CONFIGS")) {
        config_init();
        free(cfg.cpu.filename);
        free(cfg.memory.filename);
        free(cfg.io.filename);
        return 1;
    }

    return 0;
}

#define print_single_thresh(res, time, type)                                   \
    expect(*res->human_name);                                                  \
    if (res->thresholds.time.type >= 0)                                        \
    printf("        - %c%s %s %s: %.2f\n", toupper(res->human_name[0]),        \
           res->human_name + 1, #time, #type, res->thresholds.time.type)

static void print_config(void) {
    size_t i;
    char *header = "Config";

    if (config_reload_pending) {
        header = "Config reloaded. New config after reload";
    }

    info("%s:\n\n", header);

    printf("      Log pressures: %s\n", cfg.log_pressures ? "true" : "false");
    printf("      Update interval: %ds\n\n", cfg.update_interval);

    printf("      Thresholds:\n");
    for_each_arr (i, all_res) {
        const Resource *r = all_res[i];
        print_single_thresh(r, avg10, some);
        print_single_thresh(r, avg10, full);
        print_single_thresh(r, avg60, some);
        print_single_thresh(r, avg60, full);
        print_single_thresh(r, avg300, some);
        print_single_thresh(r, avg300, full);
    }

    printf("\n");
}

int main(int argc, char *argv[]) {
    unsigned long num_iters = 0;

    (void)argv;

    if (argc != 1) {
        warn("%s doesn't accept any arguments.\n", argv[0]);
        return 1;
    }

    if (check_fuzzers()) {
        /* We're just fuzzing, exit after that's done. */
        return 0;
    }

    config_init();
    expect(setvbuf(stdout, output_buf, _IOLBF, sizeof(output_buf)) == 0);
    configure_signal_handlers();
    expect(notify_init("psi-notify"));

    if (using_seat) {
        info("%s\n",
             "Using pressures from current user's systemd-logind seat.");
    } else {
        info("%s\n", "Using system-global resource pressures.");
    }

    print_config();

    /*
     * TODO: If discussion on unprivileged PSI poll() support upstream ends up
     * with patches, change this to use poll() and a real event loop.
     *
     * https://lore.kernel.org/lkml/20200424153859.GA1481119@chrisdown.name/
     */
    while (run) {
        size_t i;
        struct timespec in;

        expect(clock_gettime(CLOCK_MONOTONIC, &in) == 0);

        sd_notify(0, "READY=1\nWATCHDOG=1\n"
                     "STATUS=Checking current pressures...");

        for_each_arr (i, all_res) { pressure_check_notify_if_new(all_res[i]); }

        if (config_reload_pending) {
            sd_notify(0, "RELOADING=1\nSTATUS=Reloading config...");
            if (config_update_from_file() == 0) {
                print_config();
            }
            config_reload_pending = 0;
        } else if (run) {
            sd_notify(0, "STATUS=Waiting for next interval.");
            suspend_for_remaining_interval(&in);
        }

        ++num_iters;
    }

    info("Terminating after %lu intervals elapsed.\n", num_iters);
    sd_notify(0, "STOPPING=1\nSTATUS=Tearing down...");

    free(cfg.cpu.filename);
    free(cfg.memory.filename);
    free(cfg.io.filename);
    alert_destroy_all_active();
    notify_uninit();
}

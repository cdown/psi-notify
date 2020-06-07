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
static const time_t expiry_sec = 10;
static const double alert_clear_penalty = 5.0;

#define DEFAULT_ALERT_STATE                                                    \
    { NULL, 0, A_INACTIVE }
static Alert active_notif[] = {
    [RT_CPU] = DEFAULT_ALERT_STATE,
    [RT_MEMORY] = DEFAULT_ALERT_STATE,
    [RT_IO] = DEFAULT_ALERT_STATE,
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
        if (active_notif[i].notif) {
            NotifyNotification *n = active_notif[i].notif;
            active_notif[i].notif = NULL;
            alert_destroy(n);
        }
    }
}

#define PRESSURE_FILE_PATH_MAX sizeof("memory.pressure")

static int get_psi_dir_fd(void) {
    int dir_fd;
    char dir_path[PATH_MAX];

    snprintf_check(dir_path, PATH_MAX,
                   "/sys/fs/cgroup/user.slice/user-%d.slice", getuid());

    if ((dir_fd = open(dir_path, O_RDONLY)) > 0) {
        using_seat = true;
        return dir_fd;
    } else if ((dir_fd = open("/proc/pressure", O_RDONLY)) > 0) {
        return dir_fd;
    }

    return -EINVAL;
}

static char *get_psi_filename(char *resource) {
    char *path;

    expect(cfg.psi_dir_fd > 0);

    path = malloc(PRESSURE_FILE_PATH_MAX);
    expect(path);

    if (using_seat) {
        snprintf_check(path, PRESSURE_FILE_PATH_MAX, "%s.pressure", resource);
    } else {
        snprintf_check(path, PRESSURE_FILE_PATH_MAX, "%s", resource);
    }

    return path;
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
            warn("Full interval for %s is bogus, ignoring.\n", resource);
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
        warn("Clamping update interval to 1800 from %d.\n", rvalue);
        rvalue = 1800;
    }

    /* Signed at first to avoid %u shenanigans with negatives */
    cfg.update_interval = (time_t)rvalue;
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
    sd_notifyf(0, "WATCHDOG_USEC=%lld",
               ((long long)cfg.update_interval + WATCHDOG_GRACE_PERIOD_SEC) *
                   SEC_TO_USEC);
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

static int config_update_from_file(FILE **override_config) {
    char line[CONFIG_LINE_MAX];
    char config_path[PATH_MAX] = "";
    FILE *f;
    int ret = 0;

    if (override_config) {
        f = *override_config;
    } else {
        config_get_path(config_path);
        f = fopen(config_path, "re");
    }

    if (f) {
        config_reset_user_facing();
    } else {
        if (config_reload_pending) {
            /* This was from a SIGHUP, so we already have a config. Keep it. */
            warn("Config reload request ignored, cannot open %s: %s\n",
                 config_path, strerror(errno));
            return -errno;
        }

        if (*config_path) {
            if (errno == ENOENT) {
                info("No config at %s, using defaults.\n", config_path);
            } else {
                warn("Using default config, cannot open %s: %s\n", config_path,
                     strerror(errno));
            }
        }

        config_reset_user_facing();

        cfg.cpu.thresholds.avg10.some = 50.00;
        cfg.memory.thresholds.avg10.some = 10.00;
        cfg.io.thresholds.avg10.full = 15.00;

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

static int config_init(FILE **override_config) {
    int psi_dir_fd;
    static bool already_ran = false;

    expect(!already_ran);
    already_ran = true;

    memset(&cfg, 0, sizeof(Config));

    psi_dir_fd = get_psi_dir_fd();
    if (psi_dir_fd < 0) {
        warn("%s\n", "No pressure dir found. "
                     "Are you using kernel >=4.20 with CONFIG_PSI=y?");
        return psi_dir_fd;
    }
    cfg.psi_dir_fd = psi_dir_fd;

    cfg.cpu.filename = get_psi_filename("cpu");
    cfg.cpu.type = RT_CPU;
    cfg.cpu.human_name = "CPU";
    cfg.cpu.has_full = 0;

    cfg.memory.filename = get_psi_filename("memory");
    cfg.memory.type = RT_MEMORY;
    cfg.memory.human_name = "memory";
    cfg.memory.has_full = 1;

    cfg.io.filename = get_psi_filename("io");
    cfg.io.type = RT_IO;
    cfg.io.human_name = "I/O";
    cfg.io.has_full = 1;

    (void)config_update_from_file(override_config);

    return 0;
}

/*
 * 64 is len("some avg10=100.00 avg60=100.00 avg300=100.00") + a bit more to
 * make sure fgets() reads past total= and seeks up to \n.
 */
#define PRESSURE_LINE_LEN 64
#define PRESSURE_LINE_LEN_STR "%63s"

#define COMPARE_THRESH(threshold, current)                                     \
    (threshold >= 0 && current > threshold)

#define MIN_PSI 1.0

static double psi_penalty(double orig_psi) {
    const double penalised_psi = orig_psi - alert_clear_penalty;

    if (penalised_psi < MIN_PSI) {
        /* Too small to make granular volatility decisions. */
        return MIN_PSI;
    }

    return penalised_psi;
}

static AlertState pressure_check_single_line(FILE *f, const Resource *r) {
    char type[PRESSURE_LINE_LEN];
    double avg10, avg60, avg300;

    if (fscanf(f,
               PRESSURE_LINE_LEN_STR
               " avg10=%lf avg60=%lf avg300=%lf total=%*s",
               type, &avg10, &avg60, &avg300) != 4) {
        warn("Can't parse pressures from %s\n", strnull(r->filename));
        return A_ERROR;
    }

    if (cfg.log_pressures) {
        info("Current %s pressures: %s avg10=%.2f avg60=%.2f avg300=%.2f\n",
             strnull(r->human_name), type, avg10, avg60, avg300);
    }

    if (streq(type, "some")) {
        if (COMPARE_THRESH(r->thresholds.avg10.some, avg10) ||
            COMPARE_THRESH(r->thresholds.avg60.some, avg60) ||
            COMPARE_THRESH(r->thresholds.avg300.some, avg300)) {
            return A_ACTIVE;
        }

        if (COMPARE_THRESH(psi_penalty(r->thresholds.avg10.some), avg10) ||
            COMPARE_THRESH(psi_penalty(r->thresholds.avg60.some), avg60) ||
            COMPARE_THRESH(psi_penalty(r->thresholds.avg300.some), avg300)) {
            return A_STABILISING;
        }

        return A_INACTIVE;
    } else if (streq(type, "full")) {
        if (COMPARE_THRESH(r->thresholds.avg10.full, avg10) ||
            COMPARE_THRESH(r->thresholds.avg60.full, avg60) ||
            COMPARE_THRESH(r->thresholds.avg300.full, avg300)) {
            return A_ACTIVE;
        }

        if (COMPARE_THRESH(psi_penalty(r->thresholds.avg10.full), avg10) ||
            COMPARE_THRESH(psi_penalty(r->thresholds.avg60.full), avg60) ||
            COMPARE_THRESH(psi_penalty(r->thresholds.avg300.full), avg300)) {
            return A_STABILISING;
        }

        return A_INACTIVE;
    }

    warn("Invalid type: %s\n", type);
    return A_ERROR;
}

static int openat_psi(const char *fn) {
    int fd;

    fd = openat(cfg.psi_dir_fd, fn, O_RDONLY | O_CLOEXEC);
    if (fd > 0) {
        return fd;
    }

    /* Maybe the cgroup or proc filesystem backing this disappeared? */
    warn("PSI dir (%s) seems to have gone away, reopening\n",
         using_seat ? "logind seat" : "global");
    close(cfg.psi_dir_fd);

    cfg.psi_dir_fd = get_psi_dir_fd();
    if (cfg.psi_dir_fd < 0) {
        die("%s\n", "PSI dir disappeared and can't be found again, exiting");
    }

    fd = openat(cfg.psi_dir_fd, fn, O_RDONLY | O_CLOEXEC);
    if (fd > 0) {
        return fd;
    }

    return -EINVAL;
}

/* 2: grace threshold, 1: above thresholds, 0: within thresholds, <0: error */
static AlertState pressure_check(const Resource *r, FILE *override_file) {
    FILE *f;
    int fd;
    AlertState ret;
    char p_buf[PRESSURE_LINE_LEN * 2]; /* Avoiding slow _IO_doallocbuf */

    if (!r->filename && !override_file) {
        return A_INACTIVE;
    }

    if (override_file) {
        f = override_file;
        expect(f);
    } else {
        fd = openat_psi(r->filename);

        if (fd < 0) {
            perror(r->filename);
            return A_ERROR;
        }

        f = fdopen(fd, "r"); /* O_CLOEXEC is passed through */
        expect(f);
        expect(setvbuf(f, p_buf, _IOFBF, sizeof(p_buf)) == 0);
    }

    ret = pressure_check_single_line(f, r);
    if (ret != A_INACTIVE) {
        goto out_fclose;
    }

    if (!r->has_full) {
        goto out_fclose;
    }

    ret = pressure_check_single_line(f, r);

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
    time_t remaining_intervals;

    if (active_notif[r->type].last_state == A_ACTIVE) {
        return 0;
    }

    LOG_ALERT_STATE(r, "active");

    remaining_intervals = expiry_sec / cfg.update_interval;
    if (remaining_intervals < 1) {
        remaining_intervals = 1;
    }

    /* A_STABILISING -> A_ACTIVE reuses the existing notification */
    if (!active_notif[r->type].notif) {
        active_notif[r->type].notif = alert_user(r->human_name);
    }

    active_notif[r->type].remaining_intervals = remaining_intervals;
    return 1;
}

/* 0 means already stabilising, 1 means newly stabilising. */
static int alert_stabilising(const Resource *r) {
    if (active_notif[r->type].last_state == A_STABILISING) {
        return 0;
    }

    if (active_notif[r->type].last_state == A_ACTIVE) {
        LOG_ALERT_STATE(r, "stabilising");
    }

    return 1;
}

/* 0 means already inactive, 1 means newly inactive, 2 means stabilising. */
static int alert_stop(const Resource *r) {
    NotifyNotification *n = active_notif[r->type].notif;

    if (active_notif[r->type].last_state == A_INACTIVE) {
        return 0;
    }

    if (--active_notif[r->type].remaining_intervals) {
        /* Still got some more iterations to go before this can be closed. */
        alert_stabilising(r);
        return 2;
    }

    LOG_ALERT_STATE(r, "inactive");
    active_notif[r->type].notif = NULL;
    alert_destroy(n);

    return 1;
}

static void pressure_check_notify_if_new(const Resource *r) {
    AlertState ret = pressure_check(r, NULL);
    bool time_stabilising = false;

    switch (ret) {
        case A_INACTIVE:
            time_stabilising = alert_stop(r) == 2;
            break;
        case A_ACTIVE:
            alert_user_if_new(r);
            break;
        case A_STABILISING:
            /* Grace period where we are hands-off, to avoid volatility. */
            alert_stabilising(r);
            break;
        case A_ERROR:
            /* Already warned inside pressure_check(). */
            return;
        default:
            unreachable();
    }

    if (time_stabilising) {
        ret = A_STABILISING;
    }

    active_notif[r->type].last_state = ret;
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
        warn("Timer elapsed %lld seconds before we completed one event loop.\n",
             (long long)cfg.update_interval);
        return;
    }

    expect(nanosleep(&remaining, NULL) == 0 || errno == EINTR);
}

/* If running under AFL, just run the code and exit. Returns 1 if fuzzing. */
static int check_fuzzers(void) {
    const char *const fuzz_config_file = getenv("FUZZ_CONFIGS");
    const char *const fuzz_pressure_file = getenv("FUZZ_PRESSURES");

    if (fuzz_pressure_file) {
        Resource r;
        FILE *f = fopen(fuzz_pressure_file, "re");

        expect(f);
        memset(&r, 0, sizeof(r));
        (void)pressure_check(&r, f);
        return 1;
    }

    if (fuzz_config_file) {
        FILE *f = fopen(fuzz_config_file, "re");

        expect(f);
        expect(config_init(&f) == 0);
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
    printf("      Update interval: %llds\n\n", (long long)cfg.update_interval);

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

#ifndef UNIT_TEST
int main(int argc, char *argv[]) {
    unsigned long num_iters = 0;

    (void)argv;

    if (argc != 1) {
        die("%s doesn't accept any arguments.\n", argv[0]);
    }

    if (check_fuzzers()) {
        /* We're just fuzzing, exit after that's done. */
        return 0;
    }

    if (config_init(NULL) != 0) {
        return 1;
    }

    expect(setvbuf(stdout, output_buf, _IOLBF, sizeof(output_buf)) == 0);
    configure_signal_handlers();
    expect(notify_init("psi-notify"));

    if (using_seat) {
        info("%s\n",
             "Using pressures from the current user's systemd-logind seat.");
    } else {
        info("%s\n", "Using system-global resource pressures.");
    }

    print_config();
    info("%s\n", "Pressure monitoring started.");

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
            if (config_update_from_file(NULL) == 0) {
                print_config();
            }
            config_reload_pending = 0;
        } else if (run) {
            sd_notifyf(
                0,
                "STATUS=Waiting. Current alerts: CPU: %s, memory: %s, I/O: %s",
                active_inactive(&active_notif[RT_CPU]),
                active_inactive(&active_notif[RT_MEMORY]),
                active_inactive(&active_notif[RT_IO]));

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
#endif /* UNIT_TEST */

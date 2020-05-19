#define UNIT_TEST

#include <math.h>
#include <stdio.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

#include "../psi-notify.c" /* put it in the same translation unit */

#pragma GCC diagnostic pop

#define COL_NORMAL "\x1B[0m"
#define COL_GREEN "\x1B[32m"
#define COL_RED "\x1B[31m"

#define t_assert(test)                                                         \
    do {                                                                       \
        if (!(test)) {                                                         \
            printf("  %s[FAIL] %s%s\n", COL_RED, #test, COL_NORMAL);           \
            return false;                                                      \
        }                                                                      \
        printf("  %s[PASS] %s%s\n", COL_GREEN, #test, COL_NORMAL);             \
    } while (0)

#define t_run(test)                                                            \
    do {                                                                       \
        printf("%s:\n", #test);                                                \
        bool ret = test();                                                     \
        printf("\n");                                                          \
        if (!ret) {                                                            \
            return ret;                                                        \
        }                                                                      \
    } while (0)

static bool test_config_parse_basic(void) {
    const char *raw_config = "# Comment at beginning of line\n"
                             "  # Comment with some spaces\n\n    \n"
                             "update 3 # c\n"
                             "threshold cpu some avg10 50.00 #c\n"
                             "threshold memory full avg60 10.00 #c\n"
                             "threshold io full avg300 100.00 #c\n"
                             "log_pressures yes #c";
    FILE *f = fmemopen((void *)raw_config, strlen(raw_config), "r");

    memset(&cfg, 0, sizeof(Config));
    config_update_from_file(&f);

    t_assert(cfg.update_interval == 3);
    t_assert(cfg.log_pressures);

    t_assert(cfg.cpu.thresholds.avg10.some == 50.00);
    t_assert(cfg.memory.thresholds.avg60.full == 10.00);
    t_assert(cfg.io.thresholds.avg300.full == 100.00);

    t_assert(isnan(cfg.cpu.thresholds.avg60.some));

    return true;
}

static bool test_config_parse_init_no_file_uses_defaults(void) {
    FILE *f = NULL;
    memset(&cfg, 0, sizeof(Config));
    config_update_from_file(&f);

    t_assert(cfg.cpu.thresholds.avg10.some == 50.00);
    t_assert(cfg.memory.thresholds.avg10.some == 10.00);
    t_assert(cfg.io.thresholds.avg10.some == 10.00);

    return true;
}

static bool run_tests(void) {
    t_run(test_config_parse_basic);
    t_run(test_config_parse_init_no_file_uses_defaults);
    return true;
}

int main(void) { return run_tests() ? EXIT_SUCCESS : EXIT_FAILURE; }

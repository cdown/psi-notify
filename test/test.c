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

#define t_assert(message, test)                                                \
    do {                                                                       \
        if (!(test)) {                                                         \
            printf("%s[FAIL] %s%s\n", COL_RED, message, COL_NORMAL);           \
            return false;                                                      \
        }                                                                      \
        printf("%s[PASS] %s%s\n", COL_GREEN, message, COL_NORMAL);             \
    } while (0)

#define t_run(test)                                                            \
    do {                                                                       \
        bool ret = test();                                                     \
        if (!ret) {                                                            \
            return ret;                                                        \
        }                                                                      \
    } while (0)

static bool test_config_parse_basic(void) {
    const char *raw_config = "update 3\n"
                             "threshold cpu some avg10 50.00\n"
                             "threshold memory full avg60 10.00\n"
                             "threshold io full avg300 100.00\n"
                             "log_pressures yes";
    FILE *f = fmemopen((void *)raw_config, strlen(raw_config), "r");

    memset(&cfg, 0, sizeof(Config));
    config_update_from_file(&f);

    t_assert("set update_interval", cfg.update_interval == 3);
    t_assert("set log_pressures", cfg.log_pressures);

    t_assert("set cpu thresh", cfg.cpu.thresholds.avg10.some == 50.00);
    t_assert("set memory thresh", cfg.memory.thresholds.avg60.full == 10.00);
    t_assert("set io thresh", cfg.io.thresholds.avg300.full == 100.00);

    t_assert("other thresh not set", isnan(cfg.cpu.thresholds.avg60.some));

    return true;
}

static bool run_tests(void) {
    t_run(test_config_parse_basic);
    return true;
}

int main(void) { return run_tests() ? EXIT_SUCCESS : EXIT_FAILURE; }

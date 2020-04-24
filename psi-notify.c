#include <stdio.h>
#include <stdlib.h>

typedef struct {
    float some;
    float full;
} TimeResourcePressure;

typedef struct {
    int available : 1;

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

SystemPressure *init_system_pressure(void) {
    return calloc(1, sizeof(SystemPressure));
}

int main(int argc, char *argv[]) {
    SystemPressure *psi = init_system_pressure();
}

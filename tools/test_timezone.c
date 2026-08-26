#include <stdio.h>
#include "wedge/app.h"
int main(void) {
    struct { const char *label; long long utc; int want; } cases[] = {
        {"2026-03-08 09:59Z  one minute before spring forward", 1772963940LL, -480},
        {"2026-03-08 10:00Z  spring forward",                   1772964000LL, -420},
        {"2026-08-26 09:00Z  summer",                           1787734800LL, -420},
        {"2026-11-01 08:59Z  one minute before fall back",      1793523540LL, -420},
        {"2026-11-01 09:00Z  fall back",                        1793523600LL, -480},
        {"2026-12-30 00:00Z  winter",                           1798588800LL, -480},
        {"2027-03-14 09:59Z  before 2027 spring forward",       1805018340LL, -480},
        {"2027-03-14 10:00Z  2027 spring forward",              1805018400LL, -420},
        {"2027-11-07 08:59Z  before 2027 fall back",            1825577940LL, -420},
        {"2027-11-07 09:00Z  2027 fall back",                   1825578000LL, -480},
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        int got = wg_pacific_offset_minutes(cases[i].utc);
        int ok = got == cases[i].want;
        if (!ok) fails++;
        printf("  %s %-48s want %d got %d\n", ok?"ok  ":"FAIL", cases[i].label, cases[i].want, got);
    }
    printf(fails ? "\n%d FAILED\n" : "\nall timezone checks passed\n", fails);
    return fails ? 1 : 0;
}

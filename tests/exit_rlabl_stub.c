// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* Hand-written "external, non-RPG subroutine" test fixture for EXIT/RLABL
 * (program linkage's EXIT/RLABL). Linked separately from rpgc's own output --
 * see tests/run_tests.sh's Section N -- matching the plan's note that this
 * is the one phase the project can't verify without a non-RPG stub. */
#include "../runtime/rpg_ext_subr.h"

/* Called by tests/exit_rlabl.rpg's `EXIT SUBRA`. Its two RLABL'd parameters
 * are a numeric field X (in) and Y (out); self-checks the attribute
 * descriptors RLABL built and, only if they match what the .rpg source
 * declared, writes Y = X * 2 through the raw address. */
int SUBRA(void **parms, const struct rpg_ext_attr *attrs, int n) {
    if (n != 2) return 1;
    if (attrs[0].type != 'Z' || attrs[0].length != 5 || attrs[0].decimals != 0 ||
        attrs[0].count != 1)
        return 1;
    if (attrs[1].type != 'Z' || attrs[1].length != 5 || attrs[1].decimals != 0 ||
        attrs[1].count != 1)
        return 1;

    int *x = (int *)parms[0];
    int *y = (int *)parms[1];
    *y = (*x) * 2;
    return 0;
}

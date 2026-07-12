// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * rpg_ext_subr.h -- ABI for external ("non-RPG") subroutines invoked by the
 *                   RPG II EXIT/RLABL operation family.
 *
 * Program linkage: EXIT calls a subroutine the compiler did not
 * itself generate -- a hand-written C function, compiled and linked in
 * separately, matching the project's existing extern "C" convention
 * (rpg_runtime.c). RLABL declares the parameters passed to it.
 *
 * The manual (123972-124056) describes the attribute array RLABL builds as a
 * flat buffer of zoned-decimal bytes (1-byte type char, 4-byte zoned length,
 * 2-byte zoned decimal count, 4-byte zoned element count) -- a hardware-era
 * encoding meant for assembler/COBOL subroutines on a real System/36. This
 * project has no such subroutine to stay binary-compatible with and no
 * second language front end, so this header instead publishes a plain C
 * struct carrying the same four facts: a deliberate simplification for this
 * project's own usability, not a faithful byte-for-byte port.
 * ========================================================================== */
#ifndef RPG_EXT_SUBR_H
#define RPG_EXT_SUBR_H

#ifdef __cplusplus
extern "C" {
#endif

/* One RLABL'd parameter's attribute descriptor. */
struct rpg_ext_attr {
    char type;     /* 'C' character, 'Z' zoned numeric */
    int  length;   /* field byte length (0 if not statically known) */
    int  decimals; /* decimal positions (0 for character) */
    int  count;    /* array/table element count (1 for a scalar field) */
};

/* An external subroutine's signature. `parms[i]` is the raw address of the
 * i-th RLABL'd field/array/table/indicator, in RLABL order; `attrs[i]` is
 * its attribute descriptor; `n` is the RLABL count. A numeric field's
 * address always points at a plain 4-byte int (this compiler stores every
 * numeric field as a native i32 scaled by `decimals`, regardless of its
 * declared digit count); a character field's address points at `length`
 * raw bytes. The return value is currently unobserved by the caller (EXIT
 * carries no resulting-indicator columns per the manual) but is reserved
 * for future use -- a well-behaved subroutine returns 0. */
typedef int (*rpg_ext_subr_fn)(void **parms, const struct rpg_ext_attr *attrs,
                               int n);

#ifdef __cplusplus
}
#endif

#endif /* RPG_EXT_SUBR_H */

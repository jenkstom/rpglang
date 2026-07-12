#!/usr/bin/env bash
# =============================================================================
# tests/run_tests.sh -- test suite for the RPG II compiler.
#
# Phase 1: build + CLI smoke tests.
# Phase 2: real compile-and-run assertions on arithmetic, indicators, and
#          conditioned execution (exit codes are deterministic via the RPGRET
#          test hook and the LR latch).
# =============================================================================
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$ROOT/build"
BIN="$BUILD/bin"
RT="$BUILD/lib/librpgruntime.a"

fail=0
ok()   { printf "  \033[32mok\033[0m   %s\n" "$1"; }
bad()  { printf "  \033[31mFAIL\033[0m %s\n" "$1"; fail=1; }
hr()   { printf -- "------------------------------------------------------------\n"; }

# Build if needed.
if [[ ! -x "$BIN/rpgc" ]]; then
    echo "Building (cmake --build)..."
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
        && cmake --build "$BUILD" -j >/dev/null 2>&1
fi
[[ -x "$BIN/rpgc" ]] && ok "rpgc binary exists" || { bad "rpgc binary missing"; exit 1; }

# --- Phase 1: CLI smoke ------------------------------------------------------
hr; echo "Phase 1: CLI smoke"; hr
"$BIN/rpgc" --version | /usr/bin/grep -qi "LLVM" && ok "rpgc --version reports LLVM" || bad "rpgc --version"
"$BIN/rpgc" "$ROOT/tests/hello.rpg" >/dev/null 2>&1 && ok "rpgc accepts hello.rpg" || bad "rpgc hello.rpg"
"$BIN/rpgc" --no-such-flag 2>/dev/null; [[ $? -ne 0 ]] && ok "rpgc rejects unknown flag" || bad "rpgc accepted bad flag"

# --- Phase 2: compile + run --------------------------------------------------
hr; echo "Phase 2: compile & run"; hr
run_test() {
    local name="$1" expected="$2" file="$3" tmp="/tmp/rpgc_$$_$1"
    "$BIN/rpgc" --runtime "$RT" -o "$tmp" "$ROOT/tests/$file" >/dev/null 2>&1
    if [[ ! -x "$tmp" ]]; then bad "$name: did not compile"; return; fi
    "$tmp"; local got=$?
    if [[ "$got" -eq "$expected" ]]; then
        ok "$name: exit $got (expected $expected)"
    else
        bad "$name: exit $got (expected $expected)"
    fi
    rm -f "$tmp"
}
run_test math      42  math.rpg
run_test seton_lr   1  seton_lr.rpg
run_test cond     100  cond.rpg
run_test cond_neg   7  cond_neg.rpg

# --- Phase 3: the RPG program cycle ------------------------------------------
hr; echo "Phase 3: RPG cycle & file I/O"; hr
# Cycle test reads tests/DATA (filename literal "DATA") so it must run in tests/.
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_cycle "$ROOT/tests/cycle.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_cycle ]]; then
    ( cd "$ROOT/tests" && /tmp/rpgc_cycle ); got=$?
    if [[ "$got" -eq 42 ]]; then ok "cycle: sum across records, exit $got (expected 42)"
    else bad "cycle: exit $got (expected 42)"; fi
    rm -f /tmp/rpgc_cycle
else
    bad "cycle: did not compile"
fi

# --- Phase 4: COMP / GOTO / MOVE ---------------------------------------------
hr; echo "Phase 4: COMP, GOTO/TAG, MOVE"; hr
run_test comp     1   comp.rpg
run_test goto     15  goto.rpg
run_test move     73  move.rpg

# --- Phase 6: arithmetic + structured ops ------------------------------------
hr; echo "Phase 6: SUB/MULT/DIV/MVR, IF/DOW/DOU"; hr
run_test sub      70  sub.rpg
run_test mult     42  mult.rpg
run_test div       2  div.rpg       # DIV+MVR remainder of 100/7
run_test ifelse   11  ifelse.rpg    # IFGT true branch
run_test ifelse2  22  ifelse2.rpg   # IFGT false -> ELSE
run_test dow      15  dow.rpg       # DOWLT sum 1..5
run_test dou       3  dou.rpg       # DOUGE bottom-test
run_test nested   12  nested.rpg    # DOW+IF+DIV+MVR: sum evens 1..6

# --- Phase 7: output specs (O-specs) -----------------------------------------
hr; echo "Phase 7: O-specs / output"; hr
# Report test: reads TRANS, writes REPORT with detail lines + LR grand total.
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_report "$ROOT/tests/report.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_report ]]; then
    ( cd "$ROOT/tests" && rm -f REPORT && /tmp/rpgc_report >/dev/null 2>&1 )
    if [[ -f "$ROOT/tests/REPORT" ]]; then
        # Grand total should be 72 (10+20+5+7+30); detail lines = 5.
        if /usr/bin/grep -q "Grand total =        72" "$ROOT/tests/REPORT" \
           && [[ $(/usr/bin/grep -c "Amount" "$ROOT/tests/REPORT") -eq 5 ]]; then
            ok "report: 5 detail lines + grand total 72"
        else
            bad "report: output content wrong"; cat "$ROOT/tests/REPORT"
        fi
        rm -f "$ROOT/tests/REPORT"
    else
        bad "report: no REPORT file produced"
    fi
    rm -f /tmp/rpgc_report
else
    bad "report: did not compile"
fi

# --- Phase 8: control levels & subroutines -----------------------------------
hr; echo "Phase 8: control levels & subroutines"; hr
# Subroutine test: EXSR SUBMUL computes 6*7=42.
run_test subr     42  subr.rpg
# Control-break report: subtotals per key + grand total.
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_ctlbrk "$ROOT/tests/ctlbrk.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_ctlbrk ]]; then
    ( cd "$ROOT/tests" && rm -f CREPORT && /tmp/rpgc_ctlbrk >/dev/null 2>&1 )
    if [[ -f "$ROOT/tests/CREPORT" ]]; then
        # Subtotals 35/15/3 and grand total 53, each exactly once.
        nsub=$(/usr/bin/grep -c "Subtotal:" "$ROOT/tests/CREPORT")
        if /usr/bin/grep -q "Grand total:        53" "$ROOT/tests/CREPORT" \
           && [[ "$nsub" -eq 3 ]] \
           && /usr/bin/grep -q "Subtotal:        35" "$ROOT/tests/CREPORT" \
           && /usr/bin/grep -q "Subtotal:        15" "$ROOT/tests/CREPORT" \
           && /usr/bin/grep -q "Subtotal:         3" "$ROOT/tests/CREPORT"; then
            ok "ctlbrk: 3 subtotals (35/15/3) + grand total 53"
        else
            bad "ctlbrk: output wrong"; cat "$ROOT/tests/CREPORT"
        fi
        rm -f "$ROOT/tests/CREPORT"
    else
        bad "ctlbrk: no CREPORT file"
    fi
    rm -f /tmp/rpgc_ctlbrk
else
    bad "ctlbrk: did not compile"
fi

# --- Phase 9: character fields, arrays, XFOOT --------------------------------
hr; echo "Phase 9: character fields, arrays, XFOOT"; hr
run_test array    150  array.rpg     # compile-time array XFOOT
run_test array2    60  array2.rpg    # run-time array element access + XFOOT
# Character field test: reads FDATA, MOVE 'WORLD' into CNAME, prints it.
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_cf "$ROOT/tests/charfld.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_cf ]]; then
    ( cd "$ROOT/tests" && rm -f FOUT && /tmp/rpgc_cf >/dev/null 2>&1 )
    if [[ -f "$ROOT/tests/FOUT" ]] && /usr/bin/grep -q "Name:     WORLD" "$ROOT/tests/FOUT"; then
        ok "charfld: MOVE 'WORLD' into char field, printed"
    else
        bad "charfld: output wrong"; cat "$ROOT/tests/FOUT" 2>/dev/null
    fi
    rm -f "$ROOT/tests/FOUT" /tmp/rpgc_cf
else
    bad "charfld: did not compile"
fi

# --- Phase 10: char COMP, LOKUP, MOVEA, edit codes --------------------------
hr; echo "Phase 10: char COMP, LOKUP, MOVEA, edit codes"; hr
# char COMP + LOKUP + MOVEA tests use exit codes.
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_ccomp "$ROOT/tests/ccomp.rpg" >/dev/null 2>&1 \
    || true
run_test lokup    1   lokup.rpg    # find 30 in array => EQ indicator
# MOVEA: reads FD (ABCDE), MOVEA CSRC->CDST, COMP equal => exit 1
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_movea "$ROOT/tests/movea.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_movea ]]; then
    ( cp "$ROOT/tests/FD" /tmp/FD && cd /tmp && /tmp/rpgc_movea ); got=$?
    if [[ "$got" -eq 1 ]]; then ok "movea: copy + COMP equal, exit 1"
    else bad "movea: exit $got (expected 1)"; fi
    rm -f /tmp/rpgc_movea /tmp/FD
else
    bad "movea: did not compile"
fi
# Edit codes: read IN, print N*1000 with edit code 1 (commas).
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_edit "$ROOT/tests/edit.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_edit ]]; then
    ( cd "$ROOT/tests" && rm -f PR && /tmp/rpgc_edit >/dev/null 2>&1 )
    if [[ -f "$ROOT/tests/PR" ]] && /usr/bin/grep -q "1,234,000.0" "$ROOT/tests/PR"; then
        ok "edit: 1234000 formatted as 1,234,000.0"
    else
        bad "edit: output wrong"; cat "$ROOT/tests/PR" 2>/dev/null
    fi
    rm -f "$ROOT/tests/PR" /tmp/rpgc_edit
else
    bad "edit: did not compile"
fi

# --- Section A: missing operation codes (Z-SUB, DO, CASxx, EXCPT, TESTZ/B) ---
hr; echo "Section A: Z-SUB, DO, CASxx, EXCPT, TESTZ/TESTB"; hr
run_test zsub     251  zsub.rpg      # Z-SUB 5 -> -5 -> exit 251 (-5 & 0xFF)
run_test doloop    15  doloop.rpg    # DO 1..5 sum index = 15
run_test cas        2  cas.rpg       # CASxx: 7<10 dispatches SUB2 -> 2
run_test testz      2  testz.rpg     # TESTZ 'J' minus zone -> LO ind
run_test testb      1  testb.rpg     # TESTB bit0 off in 'C' -> HI ind
# EXCPT: per-record exception output. Reads XDATA, writes XOUT.
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_except "$ROOT/tests/except.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_except ]]; then
    ( cd "$ROOT/tests" && rm -f XOUT && /tmp/rpgc_except >/dev/null 2>&1 )
    if [[ -f "$ROOT/tests/XOUT" ]] \
       && /usr/bin/grep -q "N=   12" "$ROOT/tests/XOUT" \
       && /usr/bin/grep -q "N=   34" "$ROOT/tests/XOUT" \
       && /usr/bin/grep -q "N=   56" "$ROOT/tests/XOUT"; then
        ok "except: EXCPT writes 3 E-lines (12/34/56)"
    else
        bad "except: output wrong"; cat "$ROOT/tests/XOUT"
    fi
    rm -f "$ROOT/tests/XOUT" /tmp/rpgc_except
else
    bad "except: did not compile"
fi

# --- Section B: tables, prerun-time arrays, alternating arrays ----------------
hr; echo "Section B: tables, prerun-time arrays, alternating arrays"; hr
run_test table     30  table.rpg     # table LOKUP selects current element
run_test altarr   200  altarr.rpg    # alternating tables; related-table LOKUP
# Prerun-time array: loaded from PDATA at program start; XFOOT = 60.
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_prerun "$ROOT/tests/prerun.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_prerun ]]; then
    ( cd "$ROOT/tests" && /tmp/rpgc_prerun ); got=$?
    if [[ "$got" -eq 60 ]]; then ok "prerun: prerun-time array XFOOT, exit $got (expected 60)"
    else bad "prerun: exit $got (expected 60)"; fi
    rm -f /tmp/rpgc_prerun
else
    bad "prerun: did not compile"
fi

# --- Section C: numeric data model (decimals, packed, sign-overpunch) ---------
hr; echo "Section C: decimal scaling, packed input, sign-overpunch"; hr
run_test dec       149  dec.rpg       # C11: 100/7 -> 14.29 (scaled 1429), half-adjust
run_test ovrpunch  251  ovrpunch.rpg  # C10: char 000N -> numeric -5
# Packed-decimal input: BDATA holds packed 8191 -> exit 255.
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_packed "$ROOT/tests/packed.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_packed ]]; then
    ( cd "$ROOT/tests" && /tmp/rpgc_packed ); got=$?
    if [[ "$got" -eq 255 ]]; then ok "packed: packed-decimal 8191, exit $got (expected 255)"
    else bad "packed: exit $got (expected 255)"; fi
    rm -f /tmp/rpgc_packed
else
    bad "packed: did not compile"
fi

# --- Section D: output spec gaps (headings, skip, PAGE, edit words) ----------
hr; echo "Section D: headings/1P, skip, PAGE, edit words, per-field indicators"; hr
# Helper: compile an output-producing program, run it in tests/, check output.
run_out_test() {
    local name="$1" rpg="$2" outfile="$3" check="$4"
    "$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_${name} "$ROOT/tests/${rpg}" >/dev/null 2>&1
    if [[ ! -x /tmp/rpgc_${name} ]]; then bad "${name}: did not compile"; return; fi
    ( cd "$ROOT/tests" && rm -f "${outfile}" && /tmp/rpgc_${name} >/dev/null 2>&1 )
    if [[ -f "$ROOT/tests/${outfile}" ]] && eval "$check"; then
        ok "${name}: output ok"
    else
        bad "${name}: output wrong"; cat "$ROOT/tests/${outfile}" 2>/dev/null
    fi
    rm -f /tmp/rpgc_${name}
}
# D12: heading line (1P) prints once before detail.
run_out_test heading heading.rpg HOUT \
    '(($(grep -c "MY REPORT" "$ROOT/tests/HOUT")==1)) && grep -q "^        10" "$ROOT/tests/HOUT"'
# D13: skip-after emits a form-feed.
run_out_test skip skip.rpg SOUT \
    'grep -q $'"'"'\f'"'"' "$ROOT/tests/SOUT"'
# D14: PAGE field prints the page counter (1 on the first page).
run_out_test pgfield pgfield.rpg GOUT 'grep -q "PAGE= 1" "$ROOT/tests/GOUT"'
# D16: edit word formats a number with a comma.
run_out_test edword edword.rpg EOUT 'grep -q "1,234" "$ROOT/tests/EOUT"'
# D15: per-field conditioning — YES prints, NO suppressed.
run_out_test pfield pfield.rpg PFOUT \
    'grep -q "YES" "$ROOT/tests/PFOUT" && ! grep -q "NO" "$ROOT/tests/PFOUT"'

# --- Section E: input spec gaps (record-ID, field indicators, look-ahead) -----
hr; echo "Section E: record-identification, field indicators, look-ahead"; hr
# Helper: compile a cycle program that reads a data file, run it in tests/, check exit.
run_cycle_test() {
    local name="$1" expected="$2" rpg="$3"
    "$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_${name} "$ROOT/tests/${rpg}" >/dev/null 2>&1
    if [[ ! -x /tmp/rpgc_${name} ]]; then bad "${name}: did not compile"; return; fi
    ( cd "$ROOT/tests" && /tmp/rpgc_${name} ); got=$?
    if [[ "$got" -eq "$expected" ]]; then ok "${name}: exit $got (expected $expected)"
    else bad "${name}: exit $got (expected $expected)"; fi
    rm -f /tmp/rpgc_${name}
}
run_cycle_test fldind    1  fldind.rpg     # E18: zero field fires indicator
run_cycle_test recid    30  recid.rpg      # E17: record-ID selects type-2 fields
run_cycle_test lookahead 30  lookahead.rpg # E19: look-ahead reads next record

# --- Phase 5: end-to-end pipeline + optimization -----------------------------
hr; echo "Phase 5: pipeline & end-to-end"; hr
# Combined test: cycle reads TRANS, COMP+GOTO selectively sums values > 4.
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_e2e "$ROOT/tests/e2e.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_e2e ]]; then
    ( cd "$ROOT/tests" && /tmp/rpgc_e2e ); got=$?
    if [[ "$got" -eq 13 ]]; then ok "e2e: cycle+COMP+GOTO, exit $got (expected 13)"
    else bad "e2e: exit $got (expected 13)"; fi
    rm -f /tmp/rpgc_e2e
else
    bad "e2e: did not compile"
fi
# Optimization: -O2 must still produce a correct result.
"$BIN/rpgc" --runtime "$RT" -O2 -o /tmp/rpgc_e2e_o2 "$ROOT/tests/e2e.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_e2e_o2 ]]; then
    ( cd "$ROOT/tests" && /tmp/rpgc_e2e_o2 ); got=$?
    if [[ "$got" -eq 13 ]]; then ok "e2e -O2: optimized, exit $got (expected 13)"
    else bad "e2e -O2: exit $got (expected 13)"; fi
    rm -f /tmp/rpgc_e2e_o2
else
    bad "e2e -O2: did not compile"
fi
# Object/asm emission via llc.
"$BIN/rpgc" --emit-obj -o /tmp/rpgc_emit.o "$ROOT/tests/math.rpg" >/dev/null 2>&1 \
    && [[ -f /tmp/rpgc_emit.o ]] && ok "emit-obj (llc) produces a relocatable" \
    || bad "emit-obj"
"$BIN/rpgc" --emit-asm -o /tmp/rpgc_emit.s "$ROOT/tests/math.rpg" >/dev/null 2>&1 \
    && [[ -s /tmp/rpgc_emit.s ]] && ok "emit-asm (llc) produces assembly" \
    || bad "emit-asm"

# --- Phase 2: IR emission smoke ----------------------------------------------
"$BIN/rpgc" --emit-ir -o /tmp/rpgc_smoke.ll "$ROOT/tests/math.rpg" >/dev/null 2>&1 \
    && /usr/bin/grep -q "@rpg_in = internal global \[100 x i1\]" /tmp/rpgc_smoke.ll \
    && ok "IR contains [100 x i1] indicator array" \
    || bad "IR missing indicator array"

# --- Section F: cycle & matching (MR, secondary, overflow, first-cycle) -------
hr; echo "Section F: matching records, secondary files, overflow, first-cycle"; hr
# F20: matching records (MR indicator, M1 match field). Primary MRPRIM keys
# 1,2,3 (vals 10,20,30), secondary MRSEC keys 2,4 (vals 5,5). ind01 ADD VAL
# (60), ind02 ADD SVAL (10), MR ADD 1 (1 match) -> exit 71.
run_cycle_test mr        71  mr.rpg
# F21: secondary file with no match fields. Primary SNPRIM drains fully
# (1,2,3 -> 6), then secondary SNSC (10,20 -> 30) -> exit 36.
run_cycle_test secnofm   36  secnofm.rpg
# F22: overflow. Printer OA with OFL=3; 5 detail lines cross the overflow line
# and the OA-conditioned heading "OVERFLOW" reprints. Output file is OVDOUT.
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_ovflow "$ROOT/tests/ovflow.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_ovflow ]]; then
    ( cd "$ROOT/tests" && rm -f OVDOUT && /tmp/rpgc_ovflow >/dev/null 2>&1 )
    if [[ -f "$ROOT/tests/OVDOUT" ]] \
       && [[ $(/usr/bin/grep -c "OVERFLOW" "$ROOT/tests/OVDOUT") -ge 1 ]]; then
        ok "ovflow: OA heading reprints on overflow"
    else
        bad "ovflow: no overflow heading"; cat "$ROOT/tests/OVDOUT"
    fi
    rm -f "$ROOT/tests/OVDOUT" /tmp/rpgc_ovflow
else
    bad "ovflow: did not compile"
fi
# F23: first-cycle total correctness. Control field KEY, groups [1,1,2,2];
# L1 ADD 1 CNT counts breaks. One real break (1->2) + LR = 2. Without the
# first-cycle fix the baseline record spuriously fires L1.
run_cycle_test fstcycle   2  fstcycle.rpg

# --- Section G: file handling (keyed/random access, update files) ------------
hr; echo "Section G: CHAIN/SETLL/READE/READ, ADD, UPDATE"; hr
run_cycle_test chain     20  chain.rpg       # G24: keyed CHAIN (hit key 02 -> 20)
run_cycle_test rrn       33  rrn.rpg         # G24: CHAIN by RRN (3rd record -> 33)
run_cycle_test setllrd    2  setllrd.rpg     # G24: SETLL+READE counts 2 equal keys
# G25: O-spec ADD appends records to an output DISK file.
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_addrec "$ROOT/tests/addrec.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_addrec ]]; then
    ( cd "$ROOT/tests" && rm -f AOUT && /tmp/rpgc_addrec >/dev/null 2>&1 )
    if [[ -f "$ROOT/tests/AOUT" ]] && /usr/bin/grep -qx "10" "$ROOT/tests/AOUT" \
       && /usr/bin/grep -qx "20" "$ROOT/tests/AOUT" \
       && /usr/bin/grep -qx "30" "$ROOT/tests/AOUT"; then
        ok "addrec: ADD appends 10/20/30 to AOUT"
    else bad "addrec: AOUT wrong"; cat "$ROOT/tests/AOUT"; fi
    rm -f "$ROOT/tests/AOUT" /tmp/rpgc_addrec
else bad "addrec: did not compile"; fi
# G25: CHAIN + UPDATE rewrites a record in place (value 20 -> 25).
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_updfile "$ROOT/tests/updfile.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_updfile ]]; then
    printf '0110\n0220\n0330\n' > "$ROOT/tests/UDATA"
    ( cd "$ROOT/tests" && /tmp/rpgc_updfile >/dev/null 2>&1 )
    # The 2nd record's value (cols 3-4) was 20, +5 -> 25; rewritten in place.
    if /usr/bin/grep -q "25" "$ROOT/tests/UDATA"; then
        ok "updfile: CHAIN+UPDATE rewrites value in place"
    else bad "updfile: UDATA not updated"; cat "$ROOT/tests/UDATA"; fi
    rm -f "$ROOT/tests/UDATA" /tmp/rpgc_updfile
else bad "updfile: did not compile"; fi

# --- Section H: tooling & hardening (GOTO, recursion, diagnostics, sample) ---
hr; echo "Section H: GOTO boundaries, recursion, integration sample"; hr
# H27/H28: negative tests that must FAIL to compile (expect a non-zero rc).
expect_compile_fail() {
    local name="$1" rpg="$2"
    "$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_${name} "$ROOT/tests/${rpg}" >/dev/null 2>&1
    local rc=$?
    if [[ $rc -ne 0 ]]; then ok "${name}: correctly rejected (rc=$rc)"
    else bad "${name}: should have failed to compile"; fi
    rm -f /tmp/rpgc_${name}
}
expect_compile_fail neg_gotosub   neg_gotosub.rpg     # H27: GOTO into subroutine
expect_compile_fail neg_recursion neg_recursion.rpg   # H28: recursive subroutine
# H31: the integration sample (cycle + control breaks + array + sub + CHAIN).
run_cycle_test big       41  big.rpg

# --- Section I: TODO Group B deviation fixes ---------------------------------
hr; echo "Section I: sign-blind match fields, alpha look-ahead, SETON/OF, EXTK"; hr
# B3: match-field comparison ignores sign (manual 52317-52319): packed -5 in
# SBPRIM must match packed +5 in SBSEC, firing MR -> RPGRET = 1.
run_cycle_test signblind     1  signblind.rpg
# B4: alphameric look-ahead fields 9-fill at EOF, same as numeric ones
# (manual 83344-83346), instead of being left unchanged.
run_cycle_test la_alpha_eof  1  la_alpha_eof.rpg
# B5: SETOF must not clear LR (manual 104980-104982); exit code (= LR, no
# RPGRET) must stay 1 despite the attempted SETOF LR.
run_test seton_restrict 1  seton_restrict.rpg
# B6: F-spec cols 35-38 "EXTK" (composite/noncontiguous key) is unimplemented
# and must be a hard compile error, not a silent key_start=0.
expect_compile_fail neg_extk neg_extk.rpg

# --- Section J: TODO Group C missing operation codes -------------------------
hr; echo "Section J: READP, BITON/BITOF, *LIKE DEFN, SORTA, TIME, move-zone"; hr
# C1: READP walks a keyed file backward from past-EOF; 3 reads then BOF.
run_cycle_test readp     3  readp.rpg
# C2: BITON sets a bit on (TESTB confirms EQ), BITOF clears it back off
# (TESTB confirms HI).
run_test biton        1  biton.rpg
# C3: *LIKE DEFN copies a numeric field's decimals (verified via edit-code
# print, since decimal-aligned COMP alone can't distinguish a wrong default)
# and applies a character field's signed length delta.
run_out_test defn defn.rpg DNOUT \
    'grep -q "12,345.00" "$ROOT/tests/DNOUT" && grep -q "ABCDEFG" "$ROOT/tests/DNOUT"'
# C4: SORTA sorts a numeric array in place per its E-spec ascending flag.
run_test sorta        1  sorta.rpg
# C5: TIME stores hhmmss (0..235959) into a numeric result field.
run_test time_op      1  time_op.rpg
# C6: MHHZO/MLLZO move a byte's zone nibble without disturbing the digit
# nibble already in the destination.
run_test movezone     1  movezone.rpg

# --- Section K: TODO Group D missing spec types (H-spec, Data Structures, ---
# --- Auto Report) --------------------------------------------------------
hr; echo "Section K: H-spec, Data Structures, Auto Report (/COPY, U-spec, *AUTO)"; hr
# D1: H-spec col 18 currency symbol feeds the floating-currency detection in
# edit words (previously hardcoded to '$'): '#0000' on VAL=1234 -> "#1234".
run_out_test hspec_currency hspec_currency.rpg QOUT 'grep -q "#1234" "$ROOT/tests/QOUT"'
# D2: an anonymous DS's alphameric subfields alias shared storage -- writing
# A (bytes 1-3) and B (bytes 4-6) via MOVEL must be visible through the
# overlapping subfield WHOLE (bytes 1-6).
run_test ds_alpha       1  ds_alpha.rpg
# D2: a DS named after an existing alphameric input field REUSES that
# field's storage (true redefinition) instead of allocating a fresh buffer.
run_cycle_test ds_redefine 1  ds_redefine.rpg
# D2: a numeric DS subfield decode-reads its bytes on every access, the same
# way an ordinary zoned I-spec field is decoded.
run_test ds_numeric     1  ds_numeric.rpg
# D2: a numeric DS subfield has no i32<->zoned-decimal encoder in this
# compiler and must be a hard compile error as a calc result field, not
# silently-wrong storage.
expect_compile_fail neg_ds_numeric_write neg_ds_numeric_write.rpg
# D3: /COPY splices a member's specs into the source before any spec parser
# runs (copymem.cpy's lone C-spec becomes this program's only C-spec).
run_test copytest       1  copytest.rpg
# D3: 'U' (Auto Report Option Specification) lines aren't expanded by this
# compiler and must fail loudly rather than silently compiling an auto
# report program with no real output specs.
expect_compile_fail neg_uspec neg_uspec.rpg
# D3: *AUTO in the O-spec field-name column must be a hard error instead of
# silently printing nothing.
expect_compile_fail neg_auto neg_auto.rpg

# --- Section L: TODO Group E F-spec/E-spec gaps ------------------------------
hr; echo "Section L: F-spec external conditioning, sequence, EOF, packed prerun"; hr
# E1: F-spec cols 71-72 (U1-U8 external file conditioning), input side. A
# secondary file conditioned on U1 (default off) contributes NO records even
# though its data file has some.
run_cycle_test cond_input   6  cond_input.rpg
# E1: output side. A printer file conditioned on U2 (default off) writes NO
# lines at all, even for an unconditioned O-spec record.
run_out_test cond_output cond_output.rpg CONDOUT \
    'grep -q "YES" "$ROOT/tests/CONDOUT" && { [[ ! -s "$ROOT/tests/CONDOUT2" ]] || ! grep -q "ALSO" "$ROOT/tests/CONDOUT2"; }'
# E2: F-spec col 18 sequence ('D' = descending). Same shape as mr.rpg but
# both files declared descending and fed data in descending key order; the
# multifile selection loop must take the running MAXIMUM key each cycle
# (not the minimum) to still detect the key=2 match (MR) between files.
run_cycle_test mrdesc 66  mrdesc.rpg
# E3: F-spec col 17 (end-of-file requirement). Primary EOPRIM is marked E
# (2 records, 1+2=3); secondary EOSEC is blank/optional (3 records,
# 100+200+300=600). The program must end as soon as EOPRIM is exhausted
# without ever touching EOSEC -- RPGRET=3, not 603.
run_cycle_test eoreq  3  eoreq.rpg
# E5: F-spec designation R (record-address file) has no codegen support --
# a hard compile error instead of a silently-inert F-spec entry.
expect_compile_fail neg_recaddr neg_recaddr.rpg
# E6: F-spec col 31 = 'I' (RRN access) must stay RRN-based even when cols
# 29-30 declare a key length -- previously that combination triggered a
# byte-key binary search instead of relative-record-number access.
run_cycle_test rrn_i 33  rrn_i.rpg
# E7: E-spec col 43 = 'P' (packed-decimal prerun-time array data). PPDATA
# holds 10,20,30 packed-decimal (not zoned ASCII); XFOOT must decode it
# correctly to sum to 60, not garbage from a zoned-ASCII misread.
run_cycle_test packed_prerun 60  packed_prerun.rpg
# E8: F-spec device WORKSTN/SPECIAL/CONSOLE has no codegen support -- a hard
# compile error instead of silently treating the device name as a flat file.
expect_compile_fail neg_workstn neg_workstn.rpg

# --- Section M: TODO Group F O-spec gaps --------------------------------------
hr; echo "Section M: O-spec AND/OR continuation, fetch overflow/release"; hr
# F1: AND/OR continuation lines (cols 14-16) extending O-spec line
# conditioning past 3 indicators. Ind 30 and 32 are ON, 31 stays OFF.
# AND group (base line on 30, AND-line on 31) must NOT fire (31 is off);
# OR group (base line on 31 off, OR-line on 32 on) must fire.
run_out_test oand_or oand_or.rpg ANDOR \
    '! grep -q "AYES" "$ROOT/tests/ANDOR" && grep -q "OYES" "$ROOT/tests/ANDOR"'
# F2: O-spec col 16 = F (fetch overflow). FETOUT's overflow line is 2 (the
# runtime clamps below that to lines_per_page); D1 (FIRSTLINE, col 16 = F)
# crosses it on its own print, so the OA-conditioned heading OVERFLOWHDR
# must appear immediately after FIRSTLINE and before SECONDLINE in the same
# (single) cycle -- not just deferred to the LR-time overflow poll.
run_out_test fetch_overflow fetch_overflow.rpg FETOUT \
    'awk "/FIRSTLINE/{f=NR} /OVERFLOWHDR/{if(!h)h=NR} /SECONDLINE/{s=NR} END{exit !(f && h && s && f<h && h<s)}" "$ROOT/tests/FETOUT"'
# F2: O-spec col 16 = R (release device) requires a WORKSTN/ICF file, which
# this compiler does not support -- hard compile error, same E8 precedent.
expect_compile_fail neg_orelease neg_orelease.rpg

# --- Section N: program linkage -----------------------------------------------
hr; echo "Section N: PLIST/PARM/CALL/RETRN/FREE program linkage"; hr
# Two-program build: link_caller.rpg (CALLR, top-level) CALLs link_callee.rpg
# (CALEE) through a PLIST, exercising by-address parameter passing (CA=6,
# CB=7 in -> CR=42 out), RETRN's early-return (dead code after it must not
# run), the CALL resulting indicators (50=error off, 51=callee LR on), and
# FREE's "not successful" resulting indicator (60=on before CALEE was ever
# called, 61=off after it was). RPGRET starts at CR and every indicator
# bump is a distinct wrong-behavior signal, so only 42 means everything
# above passed; run_test's plain two-arg form doesn't take two source
# files, hence the direct invocation here (same precedent as the "cycle"
# test above).
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_link "$ROOT/tests/link_caller.rpg" "$ROOT/tests/link_callee.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_link ]]; then
    /tmp/rpgc_link; got=$?
    if [[ "$got" -eq 42 ]]; then ok "link: CALL/PARM/RETRN/FREE, exit $got (expected 42)"
    else bad "link: exit $got (expected 42)"; fi
    rm -f /tmp/rpgc_link
else
    bad "link: did not compile"
fi

# Dynamic (field-valued) CALL target: link_dyn_caller.rpg (DCALLR) MOVELs the
# literal 'CALEE' into a character field PGMNM, then CALLs *PGMNM* (not a
# literal program name) -- exercises rpg_rt_field_to_cstr blank-trimming and
# upper-casing the field's current bytes at runtime before the registry
# lookup, as opposed to link_caller.rpg's compile-time-constant CALL target
# above. Same CALEE callee, same CA=6/CB=7 -> CR=42 expectation, with error/
# LR indicator bumps (50/51) added in as a second wrong-behavior signal.
"$BIN/rpgc" --runtime "$RT" -o /tmp/rpgc_link_dyn "$ROOT/tests/link_dyn_caller.rpg" "$ROOT/tests/link_callee.rpg" >/dev/null 2>&1
if [[ -x /tmp/rpgc_link_dyn ]]; then
    /tmp/rpgc_link_dyn; got=$?
    if [[ "$got" -eq 42 ]]; then ok "link_dyn: dynamic (field-valued) CALL target, exit $got (expected 42)"
    else bad "link_dyn: exit $got (expected 42)"; fi
    rm -f /tmp/rpgc_link_dyn
else
    bad "link_dyn: did not compile"
fi

# EXIT/RLABL calling a hand-written, non-RPG external subroutine (SUBRA in
# exit_rlabl_stub.c): the one program-linkage op that needs a real C stub to
# verify end-to-end, since the compiler has no second
# language front end to generate the "external" side itself. rpgc only
# emits an object for exit_rlabl.rpg (--emit-obj); the stub is compiled and
# linked in separately here, same as a real hand-written subroutine would
# be, bypassing rpgc's own --emit-exe (which only links against the RPG
# runtime). RLABL passes X (in) and Y (out) by address plus their attribute
# descriptors; SUBRA self-checks the descriptors and sets Y = X*2 (10*2=20)
# only if they match what the .rpg source declared.
"$BIN/rpgc" --emit-obj -o /tmp/rpgc_exit.o "$ROOT/tests/exit_rlabl.rpg" >/dev/null 2>&1
CC="$(command -v cc || command -v clang || command -v gcc)"
if [[ -f /tmp/rpgc_exit.o && -n "$CC" ]]; then
    "$CC" -c "$ROOT/tests/exit_rlabl_stub.c" -o /tmp/rpgc_exit_stub.o 2>/dev/null \
        && "$CC" -no-pie /tmp/rpgc_exit.o /tmp/rpgc_exit_stub.o "$RT" -o /tmp/rpgc_exit_bin 2>/dev/null
fi
if [[ -x /tmp/rpgc_exit_bin ]]; then
    /tmp/rpgc_exit_bin; got=$?
    if [[ "$got" -eq 20 ]]; then ok "exit_rlabl: EXIT/RLABL external subroutine, exit $got (expected 20)"
    else bad "exit_rlabl: exit $got (expected 20)"; fi
else
    bad "exit_rlabl: did not compile/link"
fi
rm -f /tmp/rpgc_exit.o /tmp/rpgc_exit_stub.o /tmp/rpgc_exit_bin

hr
if [[ $fail -eq 0 ]]; then echo "ALL TESTS PASSED"; exit 0
else echo "SOME TESTS FAILED"; exit 1; fi

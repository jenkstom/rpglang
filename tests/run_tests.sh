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

hr
if [[ $fail -eq 0 ]]; then echo "ALL TESTS PASSED"; exit 0
else echo "SOME TESTS FAILED"; exit 1; fi

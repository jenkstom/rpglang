" Vim syntax file for the RPG II fixed-format language.
"
" RPG II is column-oriented: column 6 holds the form type (spec letter) and
" meaning lives in fixed column bands. Vim's column atoms (\%Nc / \%>Nc \%<Nc)
" are purpose-built for this, so this grammar is column-precise rather than
" just keyword-based.
"
" Token lists are derived from the compiler sources:
"   opcodes    -> compiler/src/cspec.cpp parse_op()  (lines 106-203)
"   indicators -> compiler/src/cspec.cpp ind_token() (lines 43-77)
"   columns    -> docs/SPEC_MAP.md
"
" Install:  copy or symlink this directory tree into ~/.vim/ , or (recommended)
" use a plugin manager:
"     Plug 'file:///path/to/rpglang/editors/vim'
" Then :syntax on. Matches *.rpg, *.rpgle, *.src, etc. (see ftdetect/).

if exists('b:current_syntax')
  finish
endif

" Case-insensitive everywhere: source is conventionally uppercase, but the
" compiler accepts lowercase too.
syntax case ignore

" --- Comments -------------------------------------------------------------
" Full-line comment: '*' at column 7 (after the spec letter at col 6).
" Matches the whole line and is defined first; the form-type rule below uses
" a negative lookahead (\%7c\* \@!) so it never fires on comment lines, which
" is more robust than relying on syn-priority alone.
syntax match rpg2Comment   /\%6c[FICLOEHL]\*.*$/
" A bare '*' comment line (no spec letter).
syntax match rpg2Comment   /^\s*\*.*/
" Column ruler line that some shops leave at the top of a member.
syntax match rpg2Ruler     /^.*[0-9+].*$/  contained transparent

" --- /COPY include directive (cols 7-11) ---------------------------------
syntax match rpg2Copy      /\%7c\/COPY\>/

" --- Form type at column 6 ------------------------------------------------
" Color the spec letter itself. The negative lookahead (\%7c\* \@!) excludes
" comment lines (C*/F*...) which are handled by rpg2Comment above.
syntax match rpg2FormType  /\%6c[FICLOEHL]\%7c\*\@!/

" --- Devices (F-spec col 40-46) and filenames (cols 7-14) -----------------
syntax keyword rpg2Device DISK PRINTER WORKSTN KEYBORD CRT SPECIAL CONSOLE
" Filename begins at col 7 (1-based).
syntax match rpg2File      /\%7c[A-Z@][A-Z0-9@#$*_]*/

" --- Operation codes ------------------------------------------------------
" Structured ops with a two-letter comparison suffix (cols 28-32).
syntax match rpg2Op        /\<IF\(EQ\|NE\|GT\|LT\|GE\|LE\)\?\>/
syntax match rpg2Op        /\<DOW\(EQ\|NE\|GT\|LT\|GE\|LE\)\?\>/
syntax match rpg2Op        /\<DOU\(EQ\|NE\|GT\|LT\|GE\|LE\)\?\>/
syntax match rpg2Op        /\<CAS\(EQ\|NE\|GT\|LT\|GE\|LE\)\?\>/

" KEY/SET may carry a 2-digit message-id suffix.
syntax match rpg2Op        /\<KEY\([0-9][0-9]\)\?\>/
syntax match rpg2Op        /\<SET\([0-9][0-9]\)\?\>/

" Dash-containing opcodes use :syn match (not keyword) because Vim keyword
" matching cannot span '-'. No trailing \> so the highlight stops cleanly at
" the opcode even when factor digits are glued on (e.g. "Z-ADD40").
syntax match rpg2Op        /\<Z-ADD/  contains=NONE
syntax match rpg2Op        /\<Z-SUB/  contains=NONE

" Plain opcodes (no dash): keyword matching is fine.
syntax keyword rpg2Op
      \ ZADD ZSUB SETON SETOF
      \ ADD SUB MULT DIV MVR COMP GOTO TAG
      \ MOVE MOVEL MOVEA DO ELSE END
      \ EXSR BEGSR ENDSR EXCPT XFOOT SQRT
      \ LOKUP LOOKUP TESTZ TESTB
      \ CHAIN SETLL READE READP READ
      \ BITON BITOF DEFN SORTA TIME
      \ MHHZO MHLZO MLHZO MLLZO
      \ PLIST PARM CALL EXIT RLABL RETRN FREE
      \ ACQ REL NEXT POST SHTDN DEBUG FORCE

" --- Control level (cols 7-8) --------------------------------------------
syntax keyword rpg2ControlLevel LR L0 L1 L2 L3 L4 L5 L6 L7 L8 L9 AN OR SR

" --- Indicators -----------------------------------------------------------
" Named indicators (safe to match anywhere).
syntax keyword rpg2Indicator LR L0 L1 L2 L3 L4 L5 L6 L7 L8 L9 MR 1P
      \ OA OB OC OD OE OF OG OV
      \ U1 U2 U3 U4 U5 U6 U7 U8
      \ H1 H2 H3 H4 H5 H6 H7 H8 H9
      \ KA KB KC KD KE KF KG KH KI KJ KK KL KM KN KO KP KQ KR KS KT KU KV KW KX KY

" General 01-99 only in the conditioning cols (9-17) and result cols (54-59)
" to avoid false positives on numeric factors/lengths. Each group is [N]II.
" cols 9-11
syntax match rpg2Indicator /\%9cN\?\(0[1-9]\|[1-8][0-9]\|9[0-9]\|L[0-9R]\|MR\|1P\|O[A-GV]\|U[1-8]\|H[1-9]\|K[A-Y]\)/
" cols 12-14
syntax match rpg2Indicator /\%12cN\?\(0[1-9]\|[1-8][0-9]\|9[0-9]\|L[0-9R]\|MR\|1P\|O[A-GV]\|U[1-8]\|H[1-9]\|K[A-Y]\)/
" cols 15-17
syntax match rpg2Indicator /\%15cN\?\(0[1-9]\|[1-8][0-9]\|9[0-9]\|L[0-9R]\|MR\|1P\|O[A-GV]\|U[1-8]\|H[1-9]\|K[A-Y]\)/
" result indicators cols 54-55 (HI), 56-57 (LO), 58-59 (EQ)
syntax match rpg2Indicator /\%54cN\?\(0[1-9]\|[1-8][0-9]\|9[0-9]\|L[0-9R]\|MR\|1P\|O[A-GV]\|U[1-8]\|H[1-9]\|K[A-Y]\)/
syntax match rpg2Indicator /\%56cN\?\(0[1-9]\|[1-8][0-9]\|9[0-9]\|L[0-9R]\|MR\|1P\|O[A-GV]\|U[1-8]\|H[1-9]\|K[A-Y]\)/
syntax match rpg2Indicator /\%58cN\?\(0[1-9]\|[1-8][0-9]\|9[0-9]\|L[0-9R]\|MR\|1P\|O[A-GV]\|U[1-8]\|H[1-9]\|K[A-Y]\)/

" --- Literals & specials --------------------------------------------------
syntax match rpg2Number    /\<[0-9]\+\>/
syntax region rpg2String   start=/'/ skip=/''/ end=/'/  oneline
" Special factor values *ENTRY, *LIKE, *ALL, *IN, ...
syntax match rpg2StarValue /\*\(ENTRY\|LIKE\|ALL\|IN\|BLANK\|ZERO\|ON\|OFF\|DATE\|DAY\|MONTH\|YEAR\|LDA\)\>/

" --- Default highlighting links -------------------------------------------
hi def link rpg2Comment        Comment
hi def link rpg2Ruler          SpecialComment
hi def link rpg2Copy           Include
hi def link rpg2FormType       Statement
hi def link rpg2Op             Keyword
hi def link rpg2ControlLevel   Label
hi def link rpg2Indicator      Special
hi def link rpg2Device         Type
hi def link rpg2File           Identifier
hi def link rpg2String         String
hi def link rpg2Number         Number
hi def link rpg2StarValue      Constant

let b:current_syntax = 'rpg2'

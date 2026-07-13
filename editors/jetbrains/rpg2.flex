// RPG II lexer for IntelliJ custom-language plugins (JFlex, Grammar-Kit).
//
// Maps RPG II source text to the tokens referenced by rpg2.bnf. Token lists
// are copied from the compiler sources so the highlighting matches what the
// compiler parses:
//   opcodes    -> compiler/src/cspec.cpp parse_op()  (lines 106-203)
//   indicators -> compiler/src/cspec.cpp ind_token() (lines 43-77)
//
// Generate the lexer: right-click this file in IntelliJ with Grammar-Kit
// installed, then "Run JFlex Generator". Adjust the package + class name to
// match your plugin (here: org.rpg2.lang.Rpg2Lexer). JFlex requires this
// header format exactly.

package org.rpg2.lang;

import com.intellij.lexer.FlexLexer;
import com.intellij.psi.tree.IElementType;
import org.rpg2.lang.psi.Rpg2Types;
import com.intellij.psi.TokenType;

%%

%class Rpg2Lexer
%implements FlexLexer
%unicode
%caseless
%function advance
%type IElementType
%eof{  return;
%eof}

// Whitespace and line endings.
CRLF       = \r|\n|\r\n
WHITE_SPACE = [ \t\f]

// Identifier-ish word in the factor/result bands.
IDENT      = [A-Za-z@][A-Za-z0-9@#$*_\-]*
// Numeric literal (also used for lengths/decimals/RRNs).
NUMBER     = [0-9]+
// Single-quoted string literal.
STRING     = '[^'\r\n]*'
// Column ruler line that some shops keep at the top of a member.
RULER      = \.+[0-9+].*

// A 2-char indicator token (01-99 + named forms).
IND2       = (0[1-9]|[1-8][0-9]|9[0-9]|L[0-9R]|MR|1P|O[A-GV]|U[1-8]|H[1-9]|K[A-Y])

// Keyword tables (case-insensitive via %caseless above). Longest-first so
// "Z-ADD" matches before "ADD".
OPCODE_PLAIN = Z-ADD|Z-SUB|ZADD|ZSUB|SETON|SETOF|ADD|SUB|MULT|DIV|MVR|
               COMP|GOTO|TAG|MOVE|MOVEL|MOVEA|DO|ELSE|END|EXSR|BEGSR|ENDSR|
               EXCPT|XFOOT|SQRT|LOKUP|LOOKUP|TESTZ|TESTB|CHAIN|SETLL|READE|
               READP|READ|BITON|BITOF|DEFN|SORTA|TIME|MHHZO|MHLZO|MLHZO|MLLZO|
               PLIST|PARM|CALL|EXIT|RLABL|RETRN|FREE|ACQ|REL|NEXT|POST|SHTDN|
               DEBUG|FORCE

STRUCTURED = IF|DOW|DOU|CAS
KEYSET     = KEY|SET
CMP_SUFFIX = EQ|NE|GT|LT|GE|LE
CONTROL    = LR|L[0-9]|AN|OR|SR
DEVICE     = DISK|PRINTER|WORKSTN|KEYBORD|CRT|SPECIAL|CONSOLE
STAR_VALUE = \*(ENTRY|LIKE|ALL|IN|BLANK|ZERO|ON|OFF|DATE|DAY|MONTH|YEAR|LDA)

%state AT_COL6, COL6_DONE

%%

// JFlex column counting is awkward, so we match the leading 5 spaces + the
// spec letter explicitly as the very first patterns. This is the cleanest way
// to anchor on "form type at column 6" in JFlex.

<YYINITIAL> {
  {CRLF}                                { return TokenType.WHITE_SPACE; }
  // Column ruler line.
  ^{RULER}                              { return Rpg2Types.RULER; }
  // Full-line comment: '*' at column 7 (after the 5-space prefix + a spec
  // letter OR a space). Consume to end of line.
  ^"     "{FORM_LETTER_OR_SPACE}"*"[^\r\n]*  { return Rpg2Types.COMMENT; }
  // /COPY directive beginning at column 7.
  ^"     /COPY"                         { return Rpg2Types.COPY; }
  // Form type at column 6: 5 spaces then the letter. We eat the leading
  // whitespace into the same token to keep the rule simple.
  ^"     "[FICLOEHL]                    { return Rpg2Types.FORM_TYPE; }

  {STAR_VALUE}                          { return Rpg2Types.STAR_VALUE; }
  {DEVICE}                              { return Rpg2Types.DEVICE; }
  {STRUCTURED}                          { return Rpg2Types.STRUCTURED; }
  {CMP_SUFFIX}                          { return Rpg2Types.CMP_SUFFIX; }
  {KEYSET}{2}{NUMBER}                   { return Rpg2Types.KEYSET; } // KEYnn/SETnn handled as KEYSET + NUMBER
  {KEYSET}                              { return Rpg2Types.KEYSET; }
  {OPCODE_PLAIN}                        { return Rpg2Types.OPCODE; }
  {CONTROL}                             { return Rpg2Types.CONTROL_LEVEL; }
  "N"?{IND2}                            { return Rpg2Types.INDICATOR; }
  {STRING}                              { return Rpg2Types.STRING; }
  {NUMBER}                              { return Rpg2Types.NUMBER; }
  {IDENT}                               { return Rpg2Types.FILENAME; }
  {WHITE_SPACE}+                        { return TokenType.WHITE_SPACE; }
  .                                     { return Rpg2Types.BAD_CHAR; }
}

// A macro used only by the comment rule above. JFlex expands it inline.
FORM_LETTER_OR_SPACE = [FICLOEHL ]

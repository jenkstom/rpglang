# JetBrains (IntelliJ IDEA / PyCharm / etc.) — RPG II syntax highlighting

A custom-language plugin for the IntelliJ Platform, built with
[Grammar-Kit](https://github.com/JetBrains/Grammar-Kit). Unlike the other
editors in this folder (single config files), a JetBrains plugin is a small
project: a `.bnf` grammar, a JFlex `.flex` lexer, a `plugin.xml`, and a build
via Gradle.

The files here are the **starter grammar and lexer** — enough to generate a
token-highlighting parser and register the language. They are derived from the
compiler's own token lists (`compiler/src/cspec.cpp`).

## Files

| File | Purpose |
|------|---------|
| `rpg2.bnf`       | Grammar-Kit BNF: token names, PSI rules, operator-precedence-ish structure. |
| `rpg2.flex`      | JFlex lexer: maps source text to the tokens the BNF references. |
| `rpg2_colors.xml`| Editor color scheme (text attributes) — sample only. |

## How to build a plugin from these

1. Create an IntelliJ Platform plugin project (IntelliJ → *New → Project →
   IDE Plugin*), or use the
   [IntelliJ Platform Plugin Template](https://github.com/JetBrains/intellij-platform-plugin-template).
2. Install the **Grammar-Kit** plugin in your dev IDE.
3. Copy `rpg2.bnf` and `rpg2.flex` into `src/main/java/.../rpg2/` (adjust the
   `package` line and class names in both files to match).
4. Right-click `rpg2.bnf` → *Generate Parser Code*. This produces the PSI
   classes and a `Rpg2Parser.java`.
5. Right-click `rpg2.flex` → *Run JFlex Generator*. This produces
   `Rpg2Lexer.java`.
6. Wire the lexer + parser into a `Rpg2ParserDefinition` and a
   `Rpg2SyntaxHighlighter` (see the `plugin.xml` + the IntelliJ SDK docs
   ["Custom Language Support"](https://plugins.jetbrains.com/docs/intellij/custom-language-support.html)).
7. `./gradlew runIde` to test in a sandbox IDE.

The grammar below is intentionally light (token-level structure, not a full
PSI mirroring the compiler's spec model). It is enough for **syntax
highlighting**; building a full IDE experience (folding, structure view,
references) would mirror `compiler/src/program.h`'s `Program` struct and is
out of scope for a starter kit.

## Token provenance

The keyword tables in `rpg2.flex` are copied from `compiler/src/cspec.cpp`
(`parse_op`, lines 106–203) and `ind_token` (lines 43–77) so they stay in sync
with what the compiler accepts.

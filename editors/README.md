# Editor syntax highlighting for RPG II

Colorization files for the RPG II fixed-format language implemented by this
compiler (`rpgc`). One folder per editor:

| Editor | Folder | Status |
|--------|--------|--------|
| VS Code / VSCodium / Cursor | [`vscode/`](vscode/) | Grammar + language config (extension) |
| Vim / Neovim | [`vim/`](vim/) | `ftdetect` + `syntax` (column-native via `\%Nc`) + transparent fixed-80 auto-split |
| Emacs | [`emacs/`](emacs/) | `rpg-mode.el` (`font-lock`) |
| Sublime Text / TextMate | [`sublime/`](sublime/) | `.sublime-syntax` |
| JetBrains (IDEA etc.) | [`jetbrains/`](jetbrains/) | Grammar-Kit `.bnf` + JFlex `.flex` |
| Kate / KWrite / KDevelop | [`kate/`](kate/) | `rpg2.xml` highlight definition |
| Notepad++ | [`notepad-plus-plus/`](notepad-plus-plus/) | User Defined Language (`.xml`) |

## Installation

Pick your editor below. The paths assume `$RPG` is the root of this repo
(the directory containing `compiler/`, `editors/`, `tools/`).

All paths below use Unix notation (`~/.vim`, `~/.config`); on Windows
substitute `%APPDATA%` (Notepad++, VS Code user extensions) or
`%USERPROFILE%` (Vim/Emacs) as noted.

### VS Code / VSCodium / Cursor

The extension is the three files under [`vscode/`](vscode/). Three install
options:

**Copy into your user extensions folder (quickest):**

```bash
# Find your extensions dir first:
code --locate-extension vscodevim.vim   # any installed ext shows the path
# Typical: ~/.vscode/extensions/
cp -r $RPG/editors/vscode ~/.vscode/extensions/rpglang.rpg2-syntax-0.1.0
```

Restart VS Code. `.rpg` / `.rpgle` / `.sqlrpgle` / `.rpg38` / `.src` files
now highlight. Verify with `View → Command Palette → Developer: Inspect
Editor Tokens and Scopes`.

**Symlink (for live editing of the grammar):**

```bash
ln -s "$RPG/editors/vscode" ~/.vscode/extensions/rpg2-syntax
```

**Package as a `.vsix`** (to share or publish):

```bash
cd $RPG/editors/vscode
npm install -g @vscode/vsce     # the official packaging tool
vsce package                    # produces rpg2-syntax-0.1.0.vsix
code --install-extension rpg2-syntax-0.1.0.vsix
```

### Vim / Neovim

The Vim support is three pieces: `ftdetect/` (file detection), `syntax/`
(the grammar), and `plugin/` (the fixed-80 auto-split). Install all three at
once.

**With a plugin manager (recommended):**

```vim
" vim-plug
Plug 'file:///home/you/src/rpglang', { 'rtp': 'editors/vim' }

" packer.nvim (Neovim)
use { '/home/you/src/rpglang', rtp = 'editors/vim' }
```

Then `:PlugInstall` / `:PackerSync`, and `:syntax on`.

**Manual install (Vim):**

```bash
cp -r $RPG/editors/vim/ftdetect/* ~/.vim/ftdetect/
cp -r $RPG/editors/vim/syntax/*   ~/.vim/syntax/
cp -r $RPG/editors/vim/ftplugin/* ~/.vim/ftplugin/
cp -r $RPG/editors/vim/plugin/*   ~/.vim/plugin/
```

**Manual install (Neovim):**

```bash
cp -r $RPG/editors/vim/ftdetect/* ~/.config/nvim/ftdetect/
cp -r $RPG/editors/vim/syntax/*   ~/.config/nvim/syntax/
cp -r $RPG/editors/vim/ftplugin/* ~/.config/nvim/ftplugin/
cp -r $RPG/editors/vim/plugin/*   ~/.config/nvim/plugin/
```

Make sure your config has `filetype plugin indent on` and `syntax on`. The
fixed-80 auto-split (see the section below) is handled by `plugin/rpg2.vim`
and needs no configuration.

### Emacs

```elisp
;; In your init file (~/.emacs.d/init.el or ~/.config/emacs/init.el):

;; 1. Put rpg2-mode.el on your load-path. Either copy it:
;;    cp $RPG/editors/emacs/rpg2-mode.el ~/.emacs.d/lisp/
;;    (add-to-list 'load-path "~/.emacs.d/lisp/")
;; ...or point load-path straight at the repo (live editing):
(add-to-list 'load-path "/home/you/src/rpglang/editors/emacs")

;; 2. The mode registers its own file extensions via `auto-mode-alist'
;;    when required, so all you need is:
(require 'rpg2-mode)
```

Open a `.rpg` file and `M-x rpg2-mode` (or it auto-activates via the
registered extensions). Run `M-x font-lock-fontify-buffer` if a buffer is
already open when you install.

### Sublime Text

Copy `RPG II.sublime-syntax` into your User packages folder:

```bash
# macOS
cp "$RPG/editors/sublime/RPG II.sublime-syntax" \
   ~/Library/Application\ Support/Sublime\ Text/Packages/User/

# Linux
cp "$RPG/editors/sublime/RPG II.sublime-syntax" \
   ~/.config/sublime-text/Packages/User/

# Windows (PowerShell)
Copy-Item "$RPG\editors\sublime\RPG II.sublime-syntax" `
          "$env:APPDATA\Sublime Text\Packages\User\"
```

Restart Sublime Text. The `.rpg` / `.rpgle` / etc. extensions are picked up
from the `file_extensions` block in the syntax file.

### Kate / KWrite / KDevelop

Copy the XML into the KDE syntax-highlighting directory:

```bash
# User install (no root needed)
mkdir -p ~/.local/share/org.kde.syntax-highlighting/syntax
cp "$RPG/editors/kate/rpg2.xml" \
   ~/.local/share/org.kde.syntax-highlighting/syntax/

# System-wide install (needs root)
sudo cp "$RPG/editors/kate/rpg2.xml" \
        /usr/share/org.kde.syntax-highlighting/syntax/
```

Restart Kate. Verify it loaded from the command line:

```bash
ksyntaxhighlighter6 -s rpg2 "$RPG/tests/hello.rpg"   # should print colored output
```

Or in Kate: F7 → reload-highlighting, then open a `.rpg` file.

### Notepad++

```text
1. Language → "Define your language..." → Import...
2. Pick: $RPG\editors\notepad-plus-plus\rpg2.xml
3. Restart Notepad++
4. Open a .rpg file → select "RPG II" from the Language menu
```

Or drop the file into place manually (Windows):

```text
%APPDATA%\Notepad++\userDefineLang.xml
```

If that file already exists, open both and merge the `<UserLang name="RPG II"
...>` block into the existing `<NotepadPlus><UserLang ...>` structure. The
file extensions (`rpg rpgle sqlrpgle rpg38 src`) are declared in the XML's
`ext=` attribute, but Notepad++ only auto-applies a UDL by extension if it's
the active language — otherwise select it from the Language menu.

### JetBrains (IntelliJ IDEA, PyCharm, etc.)

Unlike the others, JetBrains support is a plugin *project*, not a single
config file — you build it with Grammar-Kit. Full steps are in
[`jetbrains/README.md`](jetbrains/README.md); the short version:

```text
1. Create an "IDE Plugin" project (IntelliJ → New → Project → IDE Plugin),
   or clone the IntelliJ Platform Plugin Template.
2. Install the "Grammar-Kit" plugin in your dev IDE.
3. Copy rpg2.bnf and rpg2.flex into src/main/java/org/rpg2/lang/
   (adjust the package lines in both files to match).
4. Right-click rpg2.bnf → Generate Parser Code.
5. Right-click rpg2.flex  → Run JFlex Generator.
6. Wire the generated lexer + parser into a Rpg2ParserDefinition and
   Rpg2SyntaxHighlighter (see the IntelliJ SDK "Custom Language Support"
   docs).
7. ./gradlew runIde   # test in a sandbox IDE
8. ./gradlew buildPlugin  # produces a .zip you can install via
                          # Settings → Plugins → ⚙ → Install from Disk
```

This is a half-day task, not a copy-a-file task. The `.bnf`/`.flex` here are
a correct starter (token lists match the compiler), but expect to write the
`ParserDefinition` and `SyntaxHighlighter` glue yourself.

## Fixed 80-column files (no line terminators)

Many RPG II source members pulled off S/34, S/36, and AS/400 systems are
**fixed-length 80-column records with no CR/LF line terminators** — a 240-byte
file is three records run together, not three lines. This breaks *every*
syntax highlighter (and the compiler itself, which uses `std::getline`): the
whole file reads as one giant line, and column-anchored highlighting can only
find "column 6" once.

**No editor's highlighter can auto-split on a fixed column.** I verified this
rather than asserting it: Vim's `set wrap`/`textwidth`, VS Code's `wordWrap`,
Notepad++'s Word Wrap, and Kate's static word wrap are all **visual reflow** —
they reflow text on screen but don't create logical lines, so a grammar (which
matches against logical lines) still sees one line. A 240-byte / 3-record file
opened in Vim with `wrap` still reports `line count: 1`.

The fix is to give the file real line breaks. Two paths:

### 1. Pre-split with `rpg-clean` (works for all 7 editors)

The [`rpg-clean`](../docs/man/rpg-clean.1) tool (built alongside the compiler;
see `docs/BUILDING.md`) auto-detects and repairs this and several other forms
of mangled source -- EBCDIC, embedded 5250/C1 separator sequences, trailing
NUL padding, and fixed-80-no-newline records. It only fires a stage when its
detector confirms the issue is present, so it is safe to run on a whole
directory of mixed inputs:

```bash
# Fix a whole library in place. Already-clean files are skipped.
rpg-clean legacy/*.rpg

# Convert one file to stdout without touching the original
rpg-clean -c fixed80.rpg > split.rpg

# Non-80 record width
rpg-clean -w 96 prog.rpg

# Just check whether a file needs cleaning (exit 1 if it does, 2 if suspicious)
rpg-clean --check prog.rpg
```

Note: you no longer *need* to pre-split to compile. `rpgc` and `rpg-analyze`
now run this same cleanup automatically inside `load_source`, so
`rpgc fixed80.rpg` works directly. Pre-splitting is only needed so an editor's
syntax highlighter (which does not run the cleanup pipeline) sees real lines.

### 2. Transparent auto-split in Vim (only editor that supports it)

Vim is the one editor where you can intercept file I/O via `BufReadCmd`/
`BufWriteCmd` autocommands. [`vim/plugin/rpg2.vim`](vim/plugin/rpg2.vim) does
exactly this for `*.rpg`/`*.rpgle`/`*.src`/etc.:

- On **read**, if the file has no newlines, it splits it into 80-column lines
  in the buffer (so highlighting works). The file on disk is untouched.
- On **write**, it re-joins the buffer back to the fixed-width form, so the
  file round-trips byte-for-byte and other tools that expect the fixed form
  keep working. (Verified: identical SHA-256 before and after `:w`.)

This lives in `plugin/` (not `ftplugin/`) because `BufReadCmd` must register
at startup, *before* the first `:edit`.

### Per-editor summary

| Editor | Handles fixed-80? | How |
|--------|-------------------|-----|
| **Vim / Neovim** | **yes, transparently** | `vim/plugin/rpg2.vim` auto-splits on read, rejoins on write |
| VS Code | via pre-split | run `rpg-clean` first; VS Code's `wordWrap` is visual only |
| Emacs | via pre-split | `rpg-clean` first; or wrap `find-file` in your init |
| Sublime Text | via pre-split | `rpg-clean` first |
| JetBrains | via pre-split | `rpg-clean` first (a custom `FileType` stub could call it) |
| Kate | via pre-split | `rpg-clean` first; Kate's "static word wrap" is visual only |
| Notepad++ | via pre-split | `rpg-clean` first; UDL has no wrap-split, and Word Wrap is visual only |


## The one thing to know about RPG II

It is **column-oriented**, not token-oriented. Column 6 holds the *form type*
(the spec letter); meaning lives in fixed column bands. A grammar therefore has
two jobs:

1. Color the **form type** at column 6 (`F`/`I`/`C`/`O`/`E`/`H`/`L`).
2. Color **opcodes**, **indicators**, **strings**, and **comments** wherever
   they appear.

Editors that can anchor on a column (Vim via `\%6c`, Kate via `column="5"`,
VS Code/Sublime/Emacs via anchored regex) get precise form-type coloring.
Editors that can only do keyword matching (Notepad++ UDL) still get full
opcode/indicator/comment coloring — just without column precision.

> These grammars give you **color**, not **validation**. To flag a field name
> that spilled into the wrong column, use `rpg-analyze format --check`
> (see `TOOLS_IDEAS.md` §8.4).

## Canonical token reference

The lists below are extracted from the compiler sources so the highlighting
matches what actually parses. Source files (for regeneration):

- Opcodes — `compiler/src/cspec.cpp` (`parse_op`, ~lines 106–203)
- Indicators — `compiler/src/cspec.cpp` (`ind_token`, lines 43–77)
- Column layout — `docs/SPEC_MAP.md`

### Operation codes

Plain:

```
ADD  Z-ADD  ZADD  Z-SUB  ZSUB  SETON  SETOF  COMP  GOTO  TAG
MOVE  MOVEL  SUB  MULT  DIV  MVR  DO  ELSE  END  EXSR  BEGSR  ENDSR
EXCPT  XFOOT  SQRT  LOKUP  LOOKUP  MOVEA  TESTZ  TESTB
CHAIN  SETLL  READE  READP  READ  BITON  BITOF  DEFN  SORTA  TIME
MHHZO  MHLZO  MLHZO  MLLZO  PLIST  PARM  CALL  EXIT  RLABL  RETRN  FREE
ACQ  REL  NEXT  POST  SHTDN  DEBUG  FORCE
```

With two-letter comparison suffix (`IFxx` / `DOWxx` / `DOUxx` / `CASxx`,
suffix ∈ `EQ NE GT LT GE LE`):

```
IF  DOW  DOU  CAS
```

With numeric message-id suffix (`KEYnn` / `SETnn`, `nn` = 01–99):

```
KEY  SET
```

### Spec form types (column 6)

```
F  I  C  O  E  H  L
```

### Indicators

Named (safe to match as tokens everywhere):

```
LR  L0 L1 L2 L3 L4 L5 L6 L7 L8 L9  MR  1P
OA OB OC OD OE OF OG  OV
U1 U2 U3 U4 U5 U6 U7 U8
H1 H2 H3 H4 H5 H6 H7 H8 H9
KA KB KC KD KE KF KG KH KI KJ KK KL KM KN KO KP KQ KR KS KT KU KV KW KX KY
```

General `01`–`99` are column-specific (cols 9–17, 54–59); only the
column-aware grammars color them to avoid false positives on numeric factors.

### Other keywords

```
/COPY                     include directive (cols 7–11)
*ENTRY  *LIKE  *ALL       C-spec special factor values
DISK  PRINTER  WORKSTN  KEYBORD  CRT  SPECIAL  CONSOLE   F-spec devices
```

;;; rpg2-mode.el --- Major mode for the RPG II fixed-format language -*- lexical-binding: t; -*-

;; Author: rpglang project
;; Version: 0.1.0
;; License: GPL-3.0-or-later
;; Keywords: languages

;; Syntax highlighting for the RPG II language implemented by the `rpglang'
;; compiler (rpgc). RPG II is column-oriented: column 6 holds the form type
;; (spec letter) and meaning lives in fixed column bands. This mode uses
;; font-lock with column-anchored matchers for form types and conditioning
;; indicators, plus keyword-based matchers for opcodes.
;;
;; Install: drop this file on `load-path' and add
;;     (add-to-list 'auto-mode-alist '("\\.rpg\\'" . rpg2-mode))
;;     (add-to-list 'auto-mode-alist '("\\.rpgle\\'" . rpg2-mode))
;;     (add-to-list 'auto-mode-alist '("\\.sqlrpgle\\'" . rpg2-mode))
;;     (add-to-list 'auto-mode-alist '("\\.src\\'" . rpg2-mode))
;; to your init file, then `M-x rpg2-mode'.

;;; Code:

(require 'font-lock)

(defcustom rpg2-indent-offset 4
  "Indentation offset (unused by the fixed format; kept for parity)."
  :type 'integer
  :group 'rpg2)

;; Token lists are derived from the compiler sources:
;;   opcodes    -> compiler/src/cspec.cpp parse_op()  (lines 106-203)
;;   indicators -> compiler/src/cspec.cpp ind_token() (lines 43-77)
(defconst rpg2-opcodes
  '("ADD" "Z-ADD" "ZADD" "Z-SUB" "ZSUB" "SETON" "SETOF"
    "SUB" "MULT" "DIV" "MVR" "COMP" "GOTO" "TAG"
    "MOVE" "MOVEL" "MOVEA" "DO" "ELSE" "END"
    "EXSR" "BEGSR" "ENDSR" "EXCPT" "XFOOT" "SQRT"
    "LOKUP" "LOOKUP" "TESTZ" "TESTB"
    "CHAIN" "SETLL" "READE" "READP" "READ"
    "BITON" "BITOF" "DEFN" "SORTA" "TIME"
    "MHHZO" "MHLZO" "MLHZO" "MLLZO"
    "PLIST" "PARM" "CALL" "EXIT" "RLABL" "RETRN" "FREE"
    "ACQ" "REL" "NEXT" "POST" "SHTDN" "DEBUG" "FORCE"))

(defconst rpg2-control-levels
  '("LR" "L0" "L1" "L2" "L3" "L4" "L5" "L6" "L7" "L8" "L9" "AN" "OR" "SR"))

(defconst rpg2-named-indicators
  '("LR" "L0" "L1" "L2" "L3" "L4" "L5" "L6" "L7" "L8" "L9" "MR" "1P"
    "OA" "OB" "OC" "OD" "OE" "OF" "OG" "OV"
    "U1" "U2" "U3" "U4" "U5" "U6" "U7" "U8"
    "H1" "H2" "H3" "H4" "H5" "H6" "H7" "H8" "H9"
    "KA" "KB" "KC" "KD" "KE" "KF" "KG" "KH" "KI" "KJ" "KK" "KL" "KM"
    "KN" "KO" "KP" "KQ" "KR" "KS" "KT" "KU" "KV" "KW" "KX" "KY"))

(defconst rpg2-devices
  '("DISK" "PRINTER" "WORKSTN" "KEYBORD" "CRT" "SPECIAL" "CONSOLE"))

;; A general 2-char indicator token (covers 01-99 and the named ones).
(defconst rpg2-indicator-re
  (rx (optional "N")
      (or (seq "0" (any "1-9"))
          (seq (any "1-8") digit)
          (seq "9" (any "0-9"))
          "LR" "L0" (seq "L" (any "1-9")) "MR" "1P"
          (seq "O" (any "A-GV"))
          (seq "U" (any "1-8"))
          (seq "H" (any "1-9"))
          (seq "K" (any "A-Y")))))

(defun rpg2--col (col re)
  "Build a font-lock matcher anchored at 1-based column COL matching RE."
  `(,(rx-to-string `(seq line-start ,(append (list 'any) (make-list (1- col) " "))
                        (group (regexp ,re))))
    1 'font-lock-variable-name-face))

(defvar rpg2-font-lock-keywords
  `(
    ;; Full-line comment: a '*' at column 7 (index 6), after any spec letter
    ;; at col 6 (or a blank). Match first so it shadows keyword matching.
    (,(rx line-start
          (optional (any "FICLOEHL "))
          "*" (0+ any))
     0 'font-lock-comment-face t)
    ;; /COPY directive beginning at column 7.
    (,(rx line-start (6 " ") "/COPY" word-boundary)
     0 'font-lock-preprocessor-face t)
    ;; Form type at column 6.
    (,(rx line-start (5 " ") (group (any "FICLOEHL")))
     1 'font-lock-keyword-face t)

    ;; Devices (F-spec).
    (,(regexp-opt rpg2-devices 'words) . 'font-lock-type-face)
    ;; Plain opcodes.
    (,(regexp-opt rpg2-opcodes 'words) . 'font-lock-keyword-face)
    ;; Structured-op comparison suffixes.
    (,(rx word-start (or "IF" "DOW" "DOU" "CAS")
          (? (or "EQ" "NE" "GT" "LT" "GE" "LE")) word-end)
     . 'font-lock-keyword-face)
    ;; KEY/SET + optional 2-digit message id.
    (,(rx word-start (or "KEY" "SET") (? (= 2 digit)) word-end)
     . 'font-lock-keyword-face)
    ;; Control level (cols 7-8).
    (,(rx-to-string `(seq line-start (5 " ") (group (or ,@rpg2-control-levels)) word-boundary))
     1 'font-lock-label-face t)
    ;; Named indicators (keyword match anywhere).
    (,(regexp-opt rpg2-named-indicators 'words) . 'font-lock-variable-name-face)
    ;; Conditioning indicators in cols 9-17 (three [N]II groups).
    (,(rpg2--col 9  rpg2-indicator-re))
    (,(rpg2--col 12 rpg2-indicator-re))
    (,(rpg2--col 15 rpg2-indicator-re))
    ;; Result indicators cols 54-59.
    (,(rpg2--col 54 rpg2-indicator-re))
    (,(rpg2--col 56 rpg2-indicator-re))
    (,(rpg2--col 58 rpg2-indicator-re))
    ;; Special factor values.
    (,(rx word-start "*" (or "ENTRY" "LIKE" "ALL" "IN" "BLANK" "ZERO"
                             "ON" "OFF" "DATE" "DAY" "MONTH" "YEAR" "LDA") word-end)
     . 'font-lock-constant-face)
    ;; String literals.
    (,(rx "'" (0+ (not (any "'"))) "'") . 'font-lock-string-face)
    ;; Numbers.
    (,(rx word-start (1+ digit) word-end) . 'font-lock-constant-face))
  "Font-lock keywords for `rpg2-mode'.")

(defvar rpg2-mode-syntax-table
  (let ((st (make-syntax-table)))
    ;; ' is a string/punctuation char; handled via font-lock regex instead.
    st)
  "Syntax table for `rpg2-mode'.")

(defvar rpg2-mode-map
  (let ((m (make-sparse-keymap)))
    m)
  "Keymap for `rpg2-mode'.")

;;;###autoload
(define-derived-mode rpg2-mode prog-mode "RPG II"
  "Major mode for editing RPG II fixed-format source.

\\{rpg2-mode-map}"
  :syntax-table rpg2-mode-syntax-table
  (setq font-lock-defaults '(rpg2-font-lock-keywords nil t))
  (setq-local comment-start "*")
  (setq-local comment-start-skip "\\*")
  (setq-local font-lock-multiline nil))

;;;###autoload
(dolist (ext '("\\.rpg\\'" "\\.rpgle\\'" "\\.sqlrpgle\\'"
               "\\.rpg38\\'" "\\.src\\'"))
  (add-to-list 'auto-mode-alist (cons ext 'rpg2-mode)))

(provide 'rpg2-mode)
;;; rpg2-mode.el ends here

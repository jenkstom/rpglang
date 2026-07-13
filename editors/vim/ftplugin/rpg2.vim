" Vim filetype plugin for RPG II.
"
" Buffer-local settings only. The transparent fixed-width I/O (splitting
" no-newline 80-column members on read, rejoining on write) lives in
" plugin/rpg2.vim, because BufReadCmd must be registered at startup, before
" the first :edit — ftplugin/ runs too late for that.

if exists('b:did_ftplugin_rpg2')
  finish
endif
let b:did_ftplugin_rpg2 = 1

" Standard RPG II buffer settings.
setlocal commentstring=*\ %s
setlocal textwidth=0          " never auto-wrap; the fixed format is sacred
setlocal expandtab
setlocal tabstop=1

" Per-buffer record width, read by plugin/rpg2.vim's write handler. Set this
" BEFORE opening the file (e.g. via a modeline or an :autocmd) if your member
" is not 80 columns.
if !exists('b:rpg2_record_width')
  let b:rpg2_record_width = 80
endif

let b:undo_ftplugin = 'setlocal commentstring< textwidth< expandtab< tabstop<'

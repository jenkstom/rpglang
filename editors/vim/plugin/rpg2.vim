" Vim plugin: transparent fixed-width I/O for RPG II source members.
"
" Many RPG II members pulled off S/34, S/36, and AS/400 systems are
" fixed-length 80-column records with NO line terminators. Vim (and the
" rpglang compiler) read such a file as a single giant line, breaking the
" column-anchored syntax highlighting in syntax/rpg2.vim.
"
" This plugin registers BufReadCmd/BufWriteCmd/FileReadCmd autocommands for
" *.rpg / *.rpgle / *.src / *.sqlrpgle / *.rpg38 files. On read, it detects
" whether the file has no newlines; if so it splits it into record-width
" lines in the buffer (default width 80). On write, it re-joins the buffer
" back to the original fixed-width form, so the file round-trips byte-for-byte
" (modulo your edits) and other tools that expect the fixed form keep working.
"
" Files that already contain newlines are passed through to Vim's normal I/O
" untouched.
"
" Install: this belongs in plugin/ (not ftplugin/) so the autocommands are
" registered at startup, BEFORE the first :edit. With a plugin manager:
"     Plug 'file:///path/to/rpglang/editors/vim'
" or copy plugin/rpg2.vim to ~/.vim/plugin/rpg2.vim.
"
" Per-file width override:
"     let g:rpg2_record_width = 96          " global default (default 80)
" or set b:rpg2_record_width before reading.

" Guard against double-load.
if exists('g:loaded_rpg2_fixed_io')
  finish
endif
let g:loaded_rpg2_fixed_io = 1

if !exists('g:rpg2_record_width')
  let g:rpg2_record_width = 80
endif

" Extensions whose I/O we intercept. Keep in sync with ftdetect/rpg2.vim.
let s:pattern = '*.rpg,*.rpgle,*.sqlrpgle,*.rpg38,*.src'

" True if a path is a fixed-width member (readable, non-empty, no NL/CR).
function! s:is_fixed_width(path) abort
  if empty(a:path) || !filereadable(a:path)
    return 0
  endif
  let raw = readfile(a:path, 'b', 2)
  " readfile(...,'b',2) returns up to 2 elements split on NL. A fixed-width
  " file yields exactly one element (no NL anywhere) that is non-empty.
  return len(raw) == 1 && len(raw[0]) > 0
endfunction

" Split a blob into record-width lines, padding the trailing short record.
function! s:split_records(blob, width) abort
  let lines = []
  let n = strlen(a:blob)
  let i = 0
  while i < n
    let rec = strpart(a:blob, i, a:width)
    if strlen(rec) < a:width
      let rec = rec . repeat(' ', a:width - strlen(rec))
    endif
    call add(lines, rec)
    let i += a:width
  endwhile
  return lines
endfunction

" Read handler: called instead of Vim's default file read for matching files.
function! s:read_cmd(path) abort
  let width = exists('b:rpg2_record_width') ? b:rpg2_record_width : g:rpg2_record_width
  let fixed = s:is_fixed_width(a:path)
  let b:rpg2_fixed_on_disk = fixed
  let b:rpg2_record_width = width
  if fixed
    let blob = join(readfile(a:path, 'b'), "\n")
    let lines = s:split_records(blob, width)
    silent %delete _
    call setline(1, lines)
  else
    " Already has newlines: let Vim's default read handle it.
    " (We still own BufReadCmd, so do the read ourselves.)
    execute 'keepjumps read ++edit' fnameescape(a:path)
    silent 1delete _
  endif
  setlocal nomodified
endfunction

" Write handler: if the file was fixed-width on disk, rejoin to that form.
function! s:write_cmd(path) abort
  let width = exists('b:rpg2_record_width') ? b:rpg2_record_width : g:rpg2_record_width
  if get(b:, 'rpg2_fixed_on_disk', 0)
    let lines = getline(1, line('$'))
    let blob = ''
    for l in lines
      " pad short lines; the fixed form has no terminators
      let blob .= l . repeat(' ', max([0, width - len(l)]))
    endfor
    call writefile([blob], a:path, 'bs')
  else
    let lines = getline(1, line('$'))
    call writefile(lines, a:path, 's')
  endif
  setlocal nomodified
endfunction

augroup rpg2_fixed_io
  autocmd!
  execute 'autocmd BufReadCmd'  s:pattern 'call s:read_cmd(expand("<afile>:p"))'
  execute 'autocmd FileReadCmd' s:pattern 'call s:read_cmd(expand("<afile>:p"))'
  execute 'autocmd BufWriteCmd' s:pattern 'call s:write_cmd(expand("<afile>:p"))'
augroup END

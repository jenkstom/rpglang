" RPG II file detection. Source member extensions used by the rpglang
" project and by IBM shops porting source off midrange systems.
au BufRead,BufNewFile *.rpg            setfiletype rpg2
au BufRead,BufNewFile *.rpgle          setfiletype rpg2
au BufRead,BufNewFile *.sqlrpgle       setfiletype rpg2
au BufRead,BufNewFile *.rpg38          setfiletype rpg2
au BufRead,BufNewFile *.src            setfiletype rpg2

set pagination off
target remote /tmp/system-prewarm.sock
# nm 当前暂存的 Chrome：MaybePrewarmForDefaultSearchEngine=0xa600190。
# 每次重新构建 Chrome 后都要重新验证；PIE 偏移为 0x40000000。
hbreak *0x4a600190
commands
silent
printf "OMNIBOX_PREWARM trigger=%u client=%p\n", $esi, $rdi
bt 12
delete breakpoints
detach
quit
end
continue

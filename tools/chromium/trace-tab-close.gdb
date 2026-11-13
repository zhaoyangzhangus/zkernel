set pagination off
target remote /tmp/system-keys.sock
# 已根据 out/liteos-debian/chrome 的 nm 重新验证，PIE 偏移为 0x40000000。
hbreak *0x4a40ae60
commands
silent
printf "TAB_CLOSE_MOUSE_PRESSED button=%p event=%p\n", $rdi, $rsi
disable 1
continue
end
hbreak *0x4a40aea0
commands
silent
printf "TAB_CLOSE_MOUSE_RELEASED button=%p event=%p\n", $rdi, $rsi
disable 2
continue
end
hbreak *0x4adaf740
commands
silent
printf "TAB_CLOSE_CALLBACK tab=%p event=%p\n", $rdi, $rsi
disable 3
continue
end
hbreak *0x4a8cc8c0
commands
silent
printf "TAB_CLOSE_MODEL model=%p index=%d flags=%u\n", $rdi, $esi, $edx
delete breakpoints
detach
quit
end
continue

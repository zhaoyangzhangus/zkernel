set pagination off
target remote /tmp/system-keys.sock
# 已对 c012e8b51b31e1eb 进行验证，客户机 PIE 偏移为 0x40000000。
hbreak *0x45514d50
commands
silent
if *(unsigned short*)($rsi+12) == 1
printf "KEY code=%u value=%d window=%p\n", *(unsigned int*)($rsi+16), *(int*)($rsi+20), $rdi
end
continue
end
hbreak *0x496080f0
commands
silent
printf "TOGGLE controller=%p user=%d\n", $rdi, $rsi
continue
end
hbreak *0x45515870
commands
silent
printf "SET_FULLSCREEN window=%p fullscreen=%d\n", $rdi, $rsi
continue
end
hbreak *0x45515e90
commands
silent
printf "RESTORE window=%p\n", $rdi
continue
end
continue

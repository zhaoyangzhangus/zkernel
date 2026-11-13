set pagination off
target remote /tmp/system-keys.sock
# 当前 Chrome 符号；PIE 偏移为 0x40000000。每个阶段只命中一次。
hbreak *0x4cdd64c0
commands
silent
printf "PAINT_TIMER source=%p\n", $rdi
disable 1
continue
end
hbreak *0x4d072b40
commands
silent
printf "PAINT_NEEDS_COMMIT host=%p forced=%u\n", $rdi, $esi
bt 8
disable 2
continue
end
hbreak *0x4d6f55a0
commands
silent
printf "PAINT_SCHEDULE_DRAW compositor=%p\n", $rdi
disable 3
continue
end
hbreak *0x4eb64ba0
commands
silent
printf "PAINT_END device=%p\n", $rdi
disable 4
continue
end
continue

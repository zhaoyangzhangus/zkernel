set pagination off
target remote /tmp/system-keys.sock
# 当前暂存的 Chrome：Window::UpdateVisualState=0xd6dc9e0，PIE=0x40000000。
# 反汇编中的仅失败分支位于偏移 0x114 的检查之后。在该分支设置断点，而不是每帧都停下：
# 正常渲染不能受到调试器暂停影响。
# Chrome 重建后重新验证函数和分支地址。
hbreak *0x4d6dc9f9
commands
silent
printf "DESTROYING_WINDOW_VISUAL_UPDATE window=%p cr3=%p caller=%p\n", $rbx, $cr3, *(void**)($rbp+8)
set $frame = $rbp
set $depth = 0
while $frame > 0 && $frame < 0x800000000000 && $depth < 30
printf "USER_FRAME %p return=%p\n", $frame, *(void**)($frame+8)
set $next = *(unsigned long long*)$frame
if $next <= $frame || $next-$frame > 1048576
set $frame = 0
else
set $frame = $next
end
set $depth = $depth+1
end
delete breakpoints
detach
quit
end
continue

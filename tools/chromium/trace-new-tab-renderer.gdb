set pagination off
target remote /tmp/system-keys.sock
# 当前 Chrome 的 nm 符号：RenderProcessHostImpl::Init + PIE 偏移 0x40000000。
hbreak *0x47f4d5d0
commands
silent
printf "NEW_TAB_RENDERER_INIT rip=%p\n", $rip
set $frame = $rbp
set $depth = 0
while $frame > 0 && $frame < 0x800000000000 && $depth < 20
printf "USER_FRAME %p return=%p\n", $frame, *(void**)($frame+8)
set $next = *(unsigned long long*)$frame
if $next <= $frame
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

set pagination off
target remote /tmp/system-address-fork.sock
# 当前 Chrome nm：RenderProcessHostImpl::Init=0x7f4d5d0，PIE 偏移为 0x40000000。
# Chrome 每次重建后都要重新验证。
hbreak *0x47f4d5d0
commands
silent
printf "ADDRESS_RENDERER_INIT rip=%p rsp=%p rbp=%p\n", $rip, $rsp, $rbp
x/24gx $rsp
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

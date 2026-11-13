set pagination off
target remote /tmp/system-render.sock
# 已根据当前 Chrome 构建验证；PIE 偏移为 0x40000000。
hbreak *0x4fe93600
commands
silent
printf "WidgetBase::WasShown cr3=%#lx this=%#lx\n", $cr3, $rdi
continue
end
hbreak *0x4fe92a10
commands
silent
printf "WidgetBase::SetCompositorVisible cr3=%#lx this=%#lx visible=%d\n", $cr3, $rdi, $esi
continue
end
hbreak *0x4d7a8440
commands
silent
printf "AsyncLayerTreeFrameSink::SubmitCompositorFrame cr3=%#lx this=%#lx\n", $cr3, $rdi
continue
end
continue
delete breakpoints
detach
quit

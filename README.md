# zkernel

最小 x86_64 UEFI 内核实验项目。kernel entry 后首先建立自己的 4-level page
tables 并切换 CR3；PMM/VM 初始化都发生在内核自有页表之后。

## 构建

```sh
make
make run
make clean
```

只保留调试构建，内核 `printf` 通过 QEMU debugcon `0xE9` 输出宿主终端。

## PMM

初始容量统计包含 LoaderCode/Data、BootServicesCode/Data 和 Conventional。
当前实际 seed 仍只使用 Conventional。4K(PTE) : 2M(PDE) 按物理容量规划为 1:7。

## Paging

`mm/paging.c` 建立内核自己的 4-level page tables，并执行第一次 CR3 takeover。

kernel entry 后立即调用 `paging_early_takeover()`，在 PMM 初始化之前完成：

```text
all usable RAM
  LoaderCode/Data
  BootServicesCode/Data
  Conventional
        │
        ├─ identity:   VA = PA
        └─ direct map: VA = VM_DIRECT_MAP_BASE + PA
```

Early paging 不再预估页表规模，也不一次预留 bootstrap block。每缺一个
4K table page 就直接调用 `boot_alloc_pages(bi, 1, ...)`，实际使用多少页就分配多少页。

`boot_alloc_pages()` 利用 UEFI memory map 的物理地址升序，从 map 末尾反向找到
第一个能容纳请求的 Conventional descriptor，并从其高地址端向下分配；descriptor
的 `number_of_pages` 随分配缩短。因此 early 页表页永久归 paging 使用，之后的
`pmm_init()` 不会再次看到这些页。

每个 RAM range 都按 `1G -> 2M -> 4K` 贪心映射；CPU 不支持 1G page 时自动退化到
`2M -> 4K`。MMIO、framebuffer、Runtime Services 不作为普通 WB RAM direct-map。

PMM 初始化之后，VM 扫描当前页表中的 direct-map 窗口，只把真正
`PRESENT` 的连续映射登记为 `VM_REGION_DIRECT_MAP`。物理 hole 既没有
PTE，也不会被 direct-map region 占用，因此这些 VA 仍可供其它 kernel
映射使用。

identity map 目前只作为启动迁移层保留；等 kernel/stack/BOOT_INFO/PMM pointer
全部切到 high-half direct-map 地址后再删除。

PML4[511] 保留为 recursive page-table window（512 GiB）。接管 CR3 后，
paging 可以不依赖页表页本身的 identity/direct 映射而遍历当前 PML4/PDPT/PD/PT，
并为运行时缺失的中间页表从 PMM 4K pool 补页。

## Page fault

第一版 #PF 只处理 VM 已登记且带 `VM_ATTR_LAZY` 的匿名 RAM region。
not-present fault 时读取 CR2，用 `vm_query()` 判断 fault VA 是否属于已分配 region，
检查 RW/USER 等基本权限后，从 PMM 分配一个 4K frame、清零并填入当前页表。
handler 返回后由 `iretq` 重试原指令。

MMIO/framebuffer/DMA/direct-map/reserved 不走 demand-zero；protection fault、
reserved-bit fault 也不会被本处理器吞掉。NX 和 mapped-page 回收后续实现。

## VM

VM 管理整个 kernel 虚拟地址空间，同时管理 free space 和 allocated region metadata。

```text
free_root
  augmented RB-tree
  subtree_max = 子树最大空洞
  + address-sorted doubly list

used_root
  RB-tree
  保存每个已占用 region：
    start/end
    type
    attrs
```

region type 当前包含 generic/kernel/heap/stack/DMA/MMIO/framebuffer/ACPI/direct-map/reserved/user。

region attrs 保存 RWX、user、guard、pinned、lazy，以及 WB/WC/UC/WT/WP/UC- cache policy。
当前 direct-map region 已参与页表布局；其它动态 region 的通用 map/unmap 接口仍待实现。

接口：

```c
vm_alloc(... type, attrs, &va)
vm_reserve(... type, attrs)
vm_query(space, any_va_inside_region, &info)
vm_free(space, region_start)
```

`vm_query()` 可以通过 region 内任意 VA 查出它属于哪一个区间以及该区间类型和属性。
`vm_free()` 不再需要调用者重复传 size，因为 used region 自己保存范围。

VM 是所有 kernel VA 的统一所有者：固定地址区域（kernel/direct-map/MMIO 等）通过
`vm_reserve()` 登记，动态区域（heap/stack/DMA 等）通过 `vm_alloc()` 分配，不再为用途建立独立 VA allocator。

kernel VM 管理完整高 canonical half：
`0xFFFF800000000000..0xFFFFFFFFFFFFFFFF`（128 TiB）。内部 range 使用相对 base 的 offset，避免完整高半区的 exclusive end=2^64 无法用 `uint64_t` 表示。

## 调试

F5 只有一个后台 task；GDB 连接后自动 continue，没有用户断点就不停。


### Framebuffer mapping

GOP framebuffer 物理地址保存在 `BOOT_INFO.fb_phys_base`，大小使用
UEFI GOP 的 `FrameBufferSize`。kernel VM 初始化后为 framebuffer
分配独立的 `VM_REGION_FRAMEBUFFER` 虚拟区，并用 4K PTE 显式映射；
映射完成后 `BOOT_INFO.fb_base` 为可直接访问的 kernel VA。内核先初始化
IA32_PAT，再把 framebuffer 映射为 WC。


## PAT

每个 x86_64 CPU 都调用 `pat_init_cpu()` 安装统一 IA32_PAT：

```text
0 WB
1 WC
2 UC-
3 UC
4 WT
5 WP
6 WB
7 UC
```

VM 支持 `VM_ATTR_CACHE_WB/WC/UC/WT/WP/UC_MINUS`。paging 层把它们编码成
PAT/PCD/PWT；4K PTE 的 PAT 位是 bit 7，2M/1G leaf 的 PAT 位是 bit 12。
`pat_page_flags()` 已同时支持两种页大小编码。IA32_PAT 是 per-CPU MSR，
以后 AP bring-up 时每个 AP 都必须执行 `pat_init_cpu()`。


## IDT / interrupt vectors

IDT 现在安装完整 256 个 gate。每个 vector 都有独立汇编 stub，统一保存 15 个
通用寄存器，并把 CPU exception 的 error code 规范成同一 frame 后进入
`idt_dispatch()`。#PF 继续接到 demand-paging handler；其它未处理 exception
会打印 vector/error/RIP 后停机。

设备中断向量采用统一分配器：

```c
uint8_t vector;
idt_vector_alloc(&vector);             /* 0x20..0xEF */
idt_handler_register(vector, fn, ctx);

/* teardown */
idt_handler_unregister(vector);
idt_vector_free(vector);
```

`0x00..0x1F` 固定给 CPU exceptions；`0xF0..0xFF` 预留给 kernel
timer/IPI/spurious，其中 `0xFF` 作为 spurious vector。驱动也可以用
`idt_vector_alloc_at()` 在动态池内申请指定向量。handler 注册表按 acquire/release
发布，向量 bitmap 使用原子 CAS，后续 SMP 可继续沿用。

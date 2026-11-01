# zkernel Frame Pool — standalone x86-64 UEFI/QEMU test

This **isolated test branch** contains only the freestanding Frame Pool prototype and its tests. It does **not** replace the existing kernel memory allocator on `main`.

## Architecture

- Root 2 MiB free-frame stack; the 4 KiB pool obtains whole 2 MiB physical frames by ownership transfer.
- Each transferred 2 MiB frame uses its **first 4 KiB as metadata**, mapped at the next address in a *virtually contiguous* metadata area; the remaining **511 physical 4 KiB frames** are stored as prebuilt PTE values.
- A global TOP cursor handles ONLY 512-frame handoffs to/from PCP; it crosses 511-slot metadata-page boundaries without exposing metadata slot zero to callers. External allocations still accept any count, including 1, 511, 512 and 1025, through PCP.
- **PCP (future per-CPU Page Cache):** a 1024-slot, 4 MiB virtual mapping backed by two real leaf page-table pages. `fm_pcp` stores only ring indices/counters; there is no parallel `entries[1024]` array. Each cached frame is represented by a PRESENT PTE in the PCP page table and is directly accessible at `pcp_virtual_base + slot*4096`. Free installs the original frame PTE into a PCP slot; allocation reads that PTE, clears the PCP slot and installs the same PTE into the destination page table. Thus the fast path transfers PTEs directly between page tables rather than converting through physical-address arrays or rebuilding PTEs. The current single-core UEFI test uses one PCP; the Linux regression build retains its mutex. The global free-frame stack is never used for ordinary one-page operations: PCP refills and drains transfer **exactly 512 allocatable frames** per operation. When PCP fills to 1024, its oldest 512 mapped slots are drained to global, retaining the newest 512. An empty PCP refills by 512. Explicit `fm_pcp_drain_4k()` transfers one complete 512-frame batch only and does nothing for an underfilled batch. This is a policy on **transfer size**, not physical contiguity; the 512 frames can span metadata pages.
- **511 vs 512:** every 2 MiB backing chunk stores one metadata page and only **511 allocatable 4 KiB physical frames**. PCP transfers **512 allocatable frames**, therefore an initially empty global pool needs at least two backed chunks to furnish one PCP refill. Metadata slot 0 never enters PCP. When total free frames are less than 512 outside PCP, they cannot be used until a later full batch is available.

- **Mapped physical metadata boundary:** `meta_4k_backed_end` is the *exclusive virtual-address end* of metadata pages actually backed by physical memory. It starts at the metadata virtual base and advances by exactly 4 KiB only after each successful 2 MiB transfer and metadata mapping. `reserved_4k_meta_pages` is only virtual capacity; `backed_4k_meta_pages` and `backed_4k_meta_end_va` in `fm_stats` report the physically mapped region. The high-water mark is separate historical accounting and not an indication of currently backed pages. Main metadata writes are capacity-checked against mapped pages, and freeing cannot cause a write into reserved-but-unmapped virtual space.
- `fm_alloc_map_4k_at()` moves PTEs directly from PCP leaf PTEs to the target leaf page table and invalidates both the vacated PCP VA and the target VA. `fm_free_unmap_4k()` performs the reverse: it reads the target PTE, clears/invalidate the target VA, then installs the unchanged PTE into a PCP virtual slot. The caller must have created and wired the intermediate paging levels. The lower-level `fm_alloc_map_4k()` omits target-VA invalidation when the caller handles it separately.
- Normal allocation removes entries from the free stack. The manager does **not** maintain per-allocated-page descriptors, track lifetime or coalesce 4 KiB chunks back to 2 MiB.
- Historical maximum pool size is persisted across boots using an OVMF EFI variable. The UEFI harness migrates the previous test variable if found.
- This **single-core, non-reentrant UEFI test has no locks** (the locks are compiled away). The separate POSIX regression build retains mutexes so its multi-threaded tests remain valid. Do not reuse the lock-free mode in SMP, preemptible or interrupt-reentrant code.

## Build and run

Requires `clang`, `lld`, `make`, `qemu-system-x86`, and `ovmf` on Debian.

```bash
sudo apt install clang lld make qemu-system-x86 ovmf
make
QEMU_ACCEL=kvm QEMU_CPU=host ./run_qemu.sh
```

If `/dev/kvm` is unavailable, use `QEMU_ACCEL=tcg QEMU_CPU=max ./run_qemu.sh`.
The QEMU script runs **two UEFI boots** using the same writable OVMF VARS file, then verifies the saved high-water mark. Clean logs are `build/qemu/boot{1,2}.log` and full diagnostics are `build/qemu/boot{1,2}.raw.log`.

To reset the test high-water record, delete `build/qemu/OVMF_VARS-persistent.fd` and rerun.

## Host-only regression

```bash
make host-smoke
make host-smoke-asan
make host-regression
make host-asan
```

These checks use `mmap()` to emulate both metadata and PCP virtual mappings on Linux. Only the UEFI/QEMU tests exercise the real CR3 page tables. CPU cycles from KVM and TCG should not be compared directly.

## Benchmark units

UEFI labels the ordinary path `PCP` (and the historical comparison `legacy global idx`); the old global indexed comparison is not an equivalent cached allocation path. It prints `cycles/round`, `cycles/page`, `~ns/round` and `~ns/page` for TOP and direct-map benchmarks. It also prints the old indexed single-page benchmark in cycles and estimated nanoseconds. One *round* includes both free and allocate, not just allocation. The nanoseconds are **estimates** converted using a guest TSC frequency calibrated against three 100 ms UEFI `Stall` calls (median sample), shown in MHz. The clock and measured time can be affected by KVM/TCG virtualization and firmware timer accuracy; for a real latency comparison use KVM on the same machine. If firmware does not offer a usable Stall timer, the logs keep the cycle counts and omit nanoseconds instead of inventing a frequency.

## Unmapping contract

Before returning a mapped frame, `fm_free_unmap_4k()` clears its leaf PTE and invalidates the target VA before installing the frame into its PCP virtual slot. The PCP slot PTE is invalidated whenever it is installed or removed. The UEFI test uses local `INVLPG` because it is strictly single-core. On SMP, complete remote TLB shootdown before a frame is made available to another CPU. Do not return a page while stale translations remain usable.

The prebuilt PTE flags are `P|RW|NX` for kernel data. User pages, executable mappings, cache controls, and other permissions require caller-specific handling. The prototype assumes a firmware environment that permits a private CR3 while Boot Services are active; it does not call `ExitBootServices()` and is **not** a production kernel allocator.

## Contents

- `frame_pool.c/.h`: global free-frame TOP, PCP virtual PTE cache (1024 slots), 512-frame global handoffs, direct PTE-to-PTE mapping/free APIs.
- `uefi_main.c`, `uefi_min.h`, `uefi_shim.h`: freestanding firmware entry point, real page-table tests, no-lock adapter, RDTSC/RDTSCP benchmarks.
- `run_qemu.sh`: QEMU/OVMF dual-boot harness with clean logs.
- `host_smoke.c`, `sim_host.c/.h`, `persist.c/.h`, `test_frame_pool.c`: native and sanitizer correctness tests.

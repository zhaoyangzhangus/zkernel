# 当前状态

- 目标：最小 x86_64 UEFI → kernel 调试链路。
- 构建：仅 `build/debug/`；debugcon 0xE9 直接输出终端。
- PMM：只使用 MEM_CONVENTIONAL。
- global PTE/PDE pool：均为三层 64 叉 availability bitmap，各 262144 slots。
- per-CPU PTE pool：两层 64 叉，4096 slots；当前仅 bootstrap CPU。
- refill/drain：PTE local/global 每批 64 frames。
- frame：opaque 64-bit；无 allocated 元数据、无 frame→slot 反查、free 可回任意空 slot。
- 下一步：建立页表，并把每 CPU PTE pool 接入 CPU-local 状态。

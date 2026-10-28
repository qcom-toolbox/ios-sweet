# umi Adreno 650 bring-up: hand-off for the next session

## The kernel bring-up (2026-10-26, patches 0160-0170): all 23 steps green

The Xiaomi Mi 10 (umi, SM8250) runs iOS 16.7 on our XNU port. The Adreno 650 bring-up lives in
`bsd/dev/sweet_adreno.c` (kernel patches 0160-0170 in `xnu/patches/`). It runs only with boot-arg
`sweetgpu=N` (steps 1..N). **All steps 1-23 have worked on the phone (0170: step 22's 2D fill and step 23's
indirect-buffer submission with fence both pass).**

| Steps | Patch | What | Result |
|---|---|---|---|
| 1-3 | 0160 | read GCC GPU clocks, cmd-db rails, speed-bin fuse, gpucc | clean, unpowered GPU; speed bin 0 |
| 4-6 | 0161 | CX GDSC on, GMU clocks (200 MHz), first GMU reads | GMU always-on counter counts |
| 7-11 | 0162 | RSCC, SMMU (MMU-500, 5 contexts), GMU memory test, TrustZone queries | TrustZone answers our SMCs |
| 12 | 0163 | zap shader signature (pas_init_image 13) | accepted |
| 13-15 | 0164-0165 | SMMU context 1 for the GMU, a650_gmu.bin, RPMh/PDC init, GMU start | firmware runs (0x100) |
| 16-17 | 0165 | HFI queues, perf + A650 bw tables, start, 305 MHz, oob GPU_SET | 3D core has power |
| 18 | 0166-0167 | SMMU context 0 for the core: ring, a650_sqe.fw, scratch | works with SMR stream 0 / mask 0x401 |
| 19 | 0166 | zap shader loaded through TrustZone (0x8681a000) | authenticated and started |
| 20 | 0166 | A650 hw_init, CP_SQE_CNTL 1, CP_ME_INIT | command processor runs |
| 21 | 0166 | CP_SET_SECURE_MODE 0, CP_MEM_WRITE | 0x600dc0de read back |
| 22 | 0168 | 2D-engine solid fill (red/green/blue) into the panel framebuffer | seen on the panel, pixels exact |
| 23 | 0169-0170 | memory objects above 4 GB, CP_INDIRECT_BUFFER submit, CACHE_FLUSH_TS fence | magenta/yellow/cyan + data words exact, fence signalled |

Logs: `research/logs/umi-016x-gpu*.txt` (`umi-0167-gpu21`, `umi-0168-gpu22`, `umi-0169-gpu23`, `umi-0170-gpu23`).
Full history with every value: NOTES.md (umi section, top).

Lessons:
- The hypervisor grants an SMMU stream match only for the device tree's exact stream/mask pair (GMU 5/0x400,
  GPU 0/0x401). Stream 0 with mask 0x400 was silently refused (SMR reads back 0).
- The SMMU context's SCTLR keeps bit 8 (HUPCF) but drops bit 12.
- The phone's own a650_sqe.fw is version 0x093 (Linux wants >= 0x095 for security fixes); it runs.
- On the last step every announce/put waits `sweetgpupause` ms (2.2 s). Hold anything time-based until
  after the work (step 22: `sweet_fb_gpu_begin` holds 60 s at most, `sweet_fb_gpu_show(ms)` after drawing).
- The 2D engine's pitch must be a multiple of 64 bytes; the panel's rows are 4320, so step 22 treats
  two rows as one (8640 = 135 x 64).
- `CP_EVENT_WRITE` with bit 30 (TIMESTAMP, as Mesa's A6XX event writes) stores a timestamp, not the
  packet's value; for a fence word use the event without bit 30 (Linux a6xx_submit).
- CP protect (0171): Linux's a650 table has 39 entries but 48 CP_PROTECT registers - entries 0..37 go
  into CP_PROTECT(0..37), the infinite-range entry into CP_PROTECT(47), 38..46 stay untouched. Writing the
  entries consecutively leaves register 47 = 0 and LAST_SPAN_INF_RANGE blocks every command register write.

Every step announces its accesses and waits `sweetgpupause` ms so the persistent log survives a reset.
Writes go through `sa_put()` and are recorded; at the end of a run they are undone in reverse order,
except once the 3D core has been powered (then everything is left on and TrustZone keeps the zap
shader; the next restart resets the SoC).

## The solved problem (0169/0170): the CPU wrote over the GPU's pixels

In the 0169 boot (`sweetgpu=23`) the GPU steps ran before iOS's first frame, and the green band read back
0xff000000: an opaque black pixel written by the CPU after `sweet_fb_gpu_begin()` cleaned the framebuffer from
the cache - the kernel text console (drawing until the first swap mutes it) or a `AppleCLCD::present()` frame
copy racing the hold; printf/IOLog order could not tell which. **0170 fixed it: `sweet_fb_gpu_begin()` now sets
the hold and cleans the cache inside the framebuffer workloop's gate (`fWorkLoop->runAction`, no present() at
the same time), mutes the console for the hold (`sweet_console_muted`, put back when the hold ends in
`vsyncFired`, also display-off), and the power-off fade is skipped while held.** Hold-related GPU and FB lines
carry `mach_absolute_time()` stamps. The 0170 boot passed all 23 steps (step 22's bands exact again). One
interplay to remember: backboardd turned the display off ~6 s into a hold, so step 23's `sweet_fb_gpu_begin`
was refused (it always refused display-off) and it drew anyway; the fade-skip kept the panel on the GPU's
picture through the "off", so the colours were still shown.

## Where things stand (2026-10-28, patches 0160-0177)

The full Metal stack works end to end: Metal -> SweetMetal2.bundle -> metal2vulkan -> Turnip ->
SweetAdreno v2 -> the A650. backboardd and SpringBoard both create real devices ("device Turnip
Adreno (TM) 650", ~1.8 s / ~4.2 s), map BOs, compile shaders on the phone and submit; fences
complete at hundreds per boot. Two problems are open (NOTES.md umi 0177 has every value):

1. **The intermittent GPU hang is CP_OPCODE_ERROR** (0177): around the 950th submit of a Metal
   workload (once in ~15 min), the command processor stops in an indirect buffer of **backboardd's**
   command stream: ring fully read, fence word stuck at the previous fence, RBBM GPU_BUSY|CP_BUSY,
   CP_INTERRUPT_STATUS bit 0, no CP_HW_FAULT, no protection violation, no SMMU fault. The stuck-fence
   dump (patch 0177) logs all of that plus the SQE's instruction address (CP_SQE_INSTR_BASE) and the
   IB words it is stopped on, read through context 0's PTEs. **Next: catch the hang with that dump
   running and read the offending opcode.**
2. **No UI frame ever reaches the panel** - both 0177 boots the panel stayed on the garbled console
   frame (user: "same artifacts except we don't even see a part of iOS ui"; the 0175 boot DID show
   UI with artifacts, so 0177's present-path changes may have slowed the frame loop, or
   backboardd never presents: it logs "vertexFunction/fragmentFunction is not a MTLFunction" (118
   per boot) and loops "no-copy buffer refused"). **Next: add present()/directPresent() count+path
   logging with its own budget; check whether backboardd ever creates a full-screen surface; then
   the artifact itself (faint vertical character columns, Image #6 in the umi NOTES section).**
   Flame's later patches (0111 gpu-display-mask, 0113 unmap-after-fence, 0117 pool accounting,
   0119 async present) cover similar ground; refs in `xnu-work/refs/flame-*`.

## How to work

- Kernel tree: `$HOME/ios-sweet-port/xnu-work/bt/xnu` (git, one commit per patch).
  Compile check: `python3 $HOME/ios-sweet-port/xnu-work/xnu-cc-sys.py
  $HOME/ios-sweet-port/xnu-work/run-0152.log $HOME/ios-sweet-port/xnu-work/bt bsd/dev/sweet_adreno.c`
  (works for `iokit/...cpp` files too).
- Build locally: `bash tools/build-kernel-local.sh` (4-12 min incremental; trailing dSYM/python
  steps fail harmlessly - the kernel links). Kernel lands in
  `xnu-work/bt/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet`.
- Patch: `git -C .../bt/xnu format-patch -1 --stdout > xnu/patches/01NN-....patch` in
  `$HOME/ios-sweet-port/ios-sweet`, commit, push `umi-scanout`, `gh workflow run xnu.yml --ref umi-scanout`,
  download with `gh run download RUN -R qcom-toolbox/ios-sweet -n xnu-kernel -D xnu/out-01NN`.
  CI sometimes fails with "job was not acquired by Runner" (no macos-14 runner): start a new run.
- Boot image (no ARM cross compiler on this Mac): `bash tools/umi-boot-image.sh KERNEL OUT.img
  ufslogsecs=2 sweetusb=2 sweetwlan=8 sweetwlandelay=90 sweetwlanssid=<AP> sweetbattery=2
  sweetgpu=23 sweetmtl=2` - it fails at the shim's gcc but has already written
  `shim/out/payload-umi.bin`; then swap the payload into an existing metal image's ramdisk
  (boot image v2, sequential 4096-byte pages: kernel at page 1, ramdisk right after it,
  ramdisk_size at header offset 16; dtb and everything after the ramdisk's page count are kept -
  the ramdisk's page count must not grow). Verify with `tools/unpack_bootv2.py` (compare the
  kernel/dtb with the base image, grep the cmdline out of the ramdisk).
- Phone: `ANDROID_SERIAL=${UMI_SERIAL}` (two phones attached). Only `fastboot boot` (RAM). Never flash or
  wipe; the user flashes `boot` themselves when asked. Logs: from TWRP with `tools/umi-log-pull.sh NAME`;
  live over UDP: `nc -ulk 5140` on the Mac while iOS runs (IOLog only - printf stays in the video
  console / persistent ring; the relay only sees what happens after `tools/umi-usbnet.sh` brings
  the network up, so early-boot lines are missing from it). On-phone files:
  `tools/umi-files.py get/put` (`/var/local/mtl2.log`, `/var/local/turnip-<proc>.log`).
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` (or your own
  attribution line). GPU firmware, Apple files, Wi-Fi network names and device serials stay out of
  git.
- References used: Linux v6.12 `drivers/gpu/drm/msm` (a6xx_gpu.c, a6xx_catalog.c, a6xx.xml,
  adreno_pm4.xml), Mesa 24.2 (`src/freedreno/common/freedreno_devices.py`,
  `src/gallium/drivers/freedreno/a6xx/fd6_blitter.cc`, `fd6_screen.cc`, `fd6_emit.h`).

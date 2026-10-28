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

## Where things stand (2026-10-28, patches 0160-0180)

The Metal stack creates real Turnip A650 devices in backboardd and SpringBoard and compiles
CoreAnimation shaders/pipelines. COPY presents carry varied image data but the panel shows
moving noise. Pipeline `ok=1` records confirm successful pipeline creation, not correct pixels.

1. **COPY-path image corruption is the priority.** `sweetfb_direct=0` reaches the panel.
   Stride sweep on the prepared 0185 images: 4500 = same touch-responsive noise; 4096 = denser
   noise; 5120 = static boot console (photo), probably because the copy bounds check rejects
   the larger span. The 5120 result is not evidence of a GPU hang or a valid stride comparison.
   All actual payload boot lines were checked. `TU_DEBUG=flushall,noubwc` remains configured.
   The photo has broad dark bands between bright low-saturation noise. A stride fix is unproven.
   Source trace: NativeBridgeMetalFrontend passes IOSurfaceGetBytesPerRow to IoSurfacePlane;
   runtime texture.rs's explicit host-image path requests DRM_FORMAT_MOD_LINEAR with that pitch
   and verifies Vulkan's reported pitch. Whether the running workload selects this path is
   still to determine.
2. **Direct scanout does not update the panel.** Registers point at populated pool memory, but
   command-mode GRAM stops picking it up around the GPU hold/display-off/on cycle. Keep COPY
   as the workaround. Compare 0092 before changing DPU programming; re-arming fetch is a hypothesis.
3. **Intermittent CP_OPCODE_ERROR remains open.** Patch 0178 reads CP_SQE_STAT_ADDR=1 then
   CP_SQE_STAT_DATA as Linux a6xx_cp_hw_err_irq does and logs recent submissions/IB words.
   CP_SQE_INSTR_BASE is the microcode base, not a faulting PC. Capture the live dump on a hang.

Latest kernel commits:
- **0179, 761390e:** preserve the prior session's fix excluding imported BOs from submit-time
  cache cleaning. Imports are cleaned at BoMap. Never restore cleaning imports at every submit.
- **0180, 29b1da4:** preserve sweetfb_srcbpr and add opt-in `sweetfb_dump=1` COPY diagnostics.
  After GPU sync and source cache maintenance, three captures starting at 400 s uptime, at least
  30 s apart: two 64x32 pixel regions, row RGB/alpha/black-pixel sums, full geometry, and bounded
  rejection logs. Dumps are observational; they do not fix the corruption.

Both files passed per-file cross-compilation; local kernel build succeeded (log
`/tmp/umi-0180-build.log`). Image `shim/out/umi-0186-copydump.img` uses the 0184 boot line/ADT
with normal source stride and sweetfb_dump=1. Payload fits the existing ramdisk page count;
DTB verified byte-for-byte against check-0176m/dtb. RAM boot succeeded; capture pending.
Live listener is a Python UDP receive loop writing `/tmp/umi-0186-live.log`; unlike the earlier
nc listener it remains unconnected so a new phone source port after reboot is accepted.
The user must run tools/umi-usbnet.sh with sudo after USB re-enumeration.

Session scratchpad: `$HOME/ios-sweet-port/umi-gpu-session.md` (recreated because the old
one was not found). Retrieved 5120 logs under `/tmp/umi-0185-bpr5120-*`; journals accumulate
across boots, so their totals cannot be attributed to this boot alone.

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
  if the ramdisk grows, shift the DTB and tail by the page-count difference). Verify with `tools/unpack_bootv2.py` (compare the
  kernel/dtb with the base image, grep the cmdline out of the ramdisk).
- Phone: select the umi serial from `tools/umi-log-pull.sh` (two phones attached). Only `fastboot boot` (RAM). Never flash or
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

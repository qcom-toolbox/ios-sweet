# umi Adreno 650 bring-up: hand-off for the next session

## Where things stand (2026-10-26)

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

## What to do next: make the Metal device real, then watch it render

The whole Metal-on-Turnip stack is BUILT and INSTALLED (2026-10-28, NOTES 0175b): Turnip for iOS
(Mesa + UniXNU's darwin backend), the metal2vulkan Rust bridge, and SweetMetal2.bundle pushed to
/System/Library/Extensions on the phone. With `sweetmtl=2` Metal loads the plugin in backboardd,
SpringBoard and friends, and they try to create a device. One blocker, already half-solved:

1. **Boot `shim/out/umi-0175-metal.img` and run `tools/umi-usbnet.sh` after the boot starts.**
   `/var/local/mtl2-env` now says `TU_DARWIN_SERVICE=SweetAdreno`, so Turnip opens the real
   NativeAdreno service instead of the empty IOAcceleratorES stand-in. Read `/var/local/mtl2.log`
   (`tools/umi-files.py get /var/local/mtl2.log /tmp/mtl2.log`): expect "MTL2[...]: device ..."
   without "refused", and the panel drawn by CoreAnimation through Turnip. The user should watch
   the screen: wallpaper/buttons should look normal (GPU path) rather than the CPU renderer's.
2. **Patch 0176 (the proper fix):** make `SweetAdreno` subclass `IOAcceleratorES`, set the Metal
   plugin properties on IT, and stop publishing the empty "sgx" accelerator when sweetmtl is on -
   then Turnip's default service match finds the real one and the env file is unneeded (flame's
   SweetAccelerator architecture).
3. If devices come up but rendering is wrong: verify gmem_base (0x100000 vs Mesa's a6xx value),
   check the MTL2/turnip logs for Vulkan errors, and compare against flame's later patches
   (0117-0122: pool accounting, submit timing, async present, ddr votes) for the quality issues
   they hit on the way to 20-26 fps.

## How to work

- Kernel tree: `$HOME/ios-sweet-port/xnu-work/bt/xnu` (git, one commit per patch).
  Compile check: `python3 $HOME/ios-sweet-port/xnu-work/xnu-cc-sys.py
  $HOME/ios-sweet-port/xnu-work/run-0152.log $HOME/ios-sweet-port/xnu-work/bt bsd/dev/sweet_adreno.c`
  (works for `iokit/...cpp` files too).
- Patch: `git -C .../bt/xnu format-patch -1 --stdout > xnu/patches/01NN-....patch` in
  `$HOME/ios-sweet-port/ios-sweet`, commit, push `umi-scanout`, `gh workflow run xnu.yml --ref umi-scanout`,
  download with `gh run download RUN -R qcom-toolbox/ios-sweet -n xnu-kernel -D xnu/out-01NN`.
  CI sometimes fails with "job was not acquired by Runner" (no macos-14 runner): start a new run.
- Boot image (no ARM cross compiler on this Mac): `python3 -I tools/mkadt.py shim/out/umi.adt --highmem
  --board umi`, `tools/mkpayload.py KERNEL shim/out/umi.adt shim/out/payload-umi.bin --cmdline "..."`,
  then swap the payload into `shim/out/umi-0154-early.img`'s ramdisk (boot image v2; ramdisk_size at
  header offset 16; everything after the ramdisk is kept). Boot line = the one in `umi-0154-early.img`
  minus `sweetiolog=wifid`, plus `sweetbattery=2 sweetgpu=22`.
- Phone: `ANDROID_SERIAL=${UMI_SERIAL}` (two phones attached). Only `fastboot boot` (RAM). Never flash or
  wipe; the user flashes `boot` themselves when asked. Logs: from TWRP with `tools/umi-log-pull.sh NAME`.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. GPU firmware,
  Apple files, Wi-Fi network names and device serials stay out of git.
- References used: Linux v6.12 `drivers/gpu/drm/msm` (a6xx_gpu.c, a6xx_catalog.c, a6xx.xml,
  adreno_pm4.xml), Mesa 24.2 (`src/freedreno/common/freedreno_devices.py`,
  `src/gallium/drivers/freedreno/a6xx/fd6_blitter.cc`, `fd6_screen.cc`, `fd6_emit.h`).

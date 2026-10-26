# umi Adreno 650 bring-up: hand-off for the next session

## Where things stand (2026-10-26)

The Xiaomi Mi 10 (umi, SM8250) runs iOS 16.7 on our XNU port. The Adreno 650 bring-up lives in
`bsd/dev/sweet_adreno.c` (kernel patches 0160-0168 in `xnu/patches/`). It runs only with boot-arg
`sweetgpu=N` (steps 1..N). **All 22 steps work on the phone: the GPU draws on the panel.**

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

Logs: `research/logs/umi-016x-gpu*.txt` (0166-0168: `umi-0166-gpu21`, `umi-0167-gpu21`, `umi-0168-gpu22`).
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

Every step announces its accesses and waits `sweetgpupause` ms so the persistent log survives a reset.
Writes go through `sa_put()` and are recorded; at the end of a run they are undone in reverse order,
except once the 3D core has been powered (then everything is left on and TrustZone keeps the zap
shader; the next restart resets the SoC).

## What to do next: a kernel GPU service for Mesa (freedreno)

Goal: iOS user space renders with the GPU (Mesa freedreno, later CoreAnimation's GL renderer). The
kernel side mirrors Linux's msm DRM uAPI so Mesa's `drm/msm` backend ports with a thin libdrm stand-in.

1. **Step 23 (kernel only, testable now):** GPU memory objects (wired pages mapped 4K at a time into
   context 0 at a GPU address range of their own, unmapped + TLBIASID on free), command submission as
   `CP_INDIRECT_BUFFER` (opcode 0x3f: address lo/hi, size in words) from the kernel ring, and a fence:
   `CP_EVENT_WRITE CACHE_FLUSH_TS` (no TIMESTAMP bit) writing a sequence number to a fence page; wait
   by polling (no interrupts yet). Self-test: step 22's fill built in a memory object and submitted as an IB.
2. **IOUserClient** (`SweetAdreno`, entitled clients only): GET_PARAM (chip id 0x06050002, gmem
   1.125 MB, GPU address space), GEM_NEW / GEM_INFO (GPU address, map into the task) / GEM_CLOSE,
   GEM_SUBMIT (IBs + BO list), WAIT_FENCE. Note: CP protect is not set up (skipped in step 20), so a
   client's commands can write GPU registers; set up `a650_protect` (a6xx_catalog.c) before any
   untrusted client.
3. **User space:** a small test binary (iOS SDK + ldid; the SWEET AMFI policy makes signed root
   binaries platform binaries) that draws through the user client; then Mesa freedreno with a
   libdrm-msm stand-in. Getting files onto the phone's HFS root needs a new root image (the user
   flashes it) or a kernel-side copy at boot: decide before step 3.

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

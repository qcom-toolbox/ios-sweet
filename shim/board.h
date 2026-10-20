#pragma once
/*
 * Per-phone constants. Default is sweet (Redmi Note 10 Pro, SM7150);
 * shim/build.sh BOARD=umi builds for the Xiaomi Mi 10 (SM8250).
 * GIC, PS_HOLD, UFS host and the splash framebuffer base are the same on both.
 */
#if defined(BOARD_UMI)
#define BOARD_NAME		"umi"
#define IMEM_RESTART_REASON	0x146bf65cUL	/* qcom,msm-imem@146bf000 restart_reason@65c */
#define BOARD_FB_H		2340		/* 1080x2340 DSC cmd-mode AMOLED */
#else
#define BOARD_NAME		"sweet"
#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem restart_reason@65c */
#define BOARD_FB_H		2400
#endif

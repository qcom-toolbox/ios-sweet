/*
 * sweet-gpu-test: the Adreno 650 through the SweetAdreno user client (iokit/Kernel/arm/
 * SweetAdreno.cpp, kernel patches 0173+), the way Mesa freedreno's libdrm-msm stand-in will:
 *
 *   GET_PARAM (chip id, gmem, VA start), GEM_NEW two objects, GEM_INFO their GPU addresses,
 *   map the command object into this task, build an indirect buffer that CP_MEM_WRITEs four
 *   known words into the data object, GEM_SUBMIT it, WAIT_FENCE, read the data object back and
 *   compare, GEM_CLOSE both. Results go to /private/var/sweet/gpu-test.log, so they can be
 *   fetched from the Mac with tools/umi-files.py (the kernel file server, tcp 2223).
 *
 * PM4 packet encodings as in bsd/dev/sweet_adreno.c (CP_MEM_WRITE 0x3d; the type 7 header with
 * its parity bits). Build with phone/build-gpu-test.sh (iPhoneOS SDK + ldid).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#define SEL_GET_PARAM   1
#define SEL_GEM_NEW     2
#define SEL_GEM_INFO    3
#define SEL_GEM_CLOSE   4
#define SEL_GEM_SUBMIT  5
#define SEL_WAIT_FENCE  6

/* msm DRM uAPI MSM_PARAM_* (include/uapi/drm/msm_drm.h) */
#define MSM_PARAM_GMEM_SIZE     0x02
#define MSM_PARAM_CHIP_ID       0x03
#define MSM_PARAM_VA_START      0x0e

#define CP_MEM_WRITE           0x3d

static FILE *logf;

static uint32_t
pm4_parity(uint32_t v)
{
	return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}

static uint32_t
pkt7(uint32_t opcode, uint32_t words)
{
	return 7u << 28 | words | pm4_parity(words) << 15 | (opcode & 0x7f) << 16 | pm4_parity(opcode) << 23;
}

static kern_return_t
scalar(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t *out, uint32_t *nout)
{
	return IOConnectCallScalarMethod(c, sel, in, nin, out, nout);
}

static uint64_t
get_param(io_connect_t c, uint32_t param, const char *name)
{
	uint64_t out = 0;
	uint32_t nout = 1;
	kern_return_t k = scalar(c, SEL_GET_PARAM, &(uint64_t){ param }, 1, &out, &nout);
	fprintf(logf, "GET_PARAM %#x (%s) -> %#llx%s\n", param, name, out,
	    k == kIOReturnSuccess ? "" : " (failed)");
	return k == kIOReturnSuccess ? out : 0;
}

static int
fail(const char *what, kern_return_t k)
{
	fprintf(logf, "FAIL: %s: %#x\n", what, k);
	fflush(logf);
	return 1;
}

int
main(void)
{
	static const uint32_t words[4] = { 0x5eed0001u, 0x5eed0002u, 0x5eed0003u, 0x5eed0004u };
	io_connect_t connect = MACH_PORT_NULL;
	io_iterator_t iter;
	kern_return_t k;
	uint64_t in[3], out[2];
	uint32_t nout;

	logf = fopen("/private/var/sweet/gpu-test.log", "w");
	if (logf == NULL) {
		logf = stderr;
	}
	fprintf(logf, "sweet-gpu-test: start\n");

	/* Find the service the way a user-space driver would. The GPU bring-up (boot-arg
	 * sweetgpu) only has the core running a minute or so into the boot, and launchd
	 * starts this daemon before that: keep looking, then keep opening, for up to 4 min. */
	for (int tries = 0; tries < 48 && connect == MACH_PORT_NULL; tries++) {
		CFMutableDictionaryRef match = IOServiceMatching("SweetAdreno");
		k = IOServiceGetMatchingServices(MACH_PORT_NULL, match, &iter);
		if (k != KERN_SUCCESS) {
			return fail("IOServiceGetMatchingServices(SweetAdreno)", k);
		}
		io_service_t service = IOIteratorNext(iter);
		IOObjectRelease(iter);
		if (service == MACH_PORT_NULL) {
			fprintf(logf, "no SweetAdreno service yet (try %d)\n", tries);
			sleep(5);
			continue;
		}
		k = IOServiceOpen(service, mach_task_self(), 0, &connect);
		IOObjectRelease(service);
		if (k != KERN_SUCCESS) {
			fprintf(logf, "IOServiceOpen refused (try %d): %#x; the core may still be coming up\n", tries, k);
			connect = MACH_PORT_NULL;
			sleep(5);
		}
	}
	if (connect == MACH_PORT_NULL) {
		return fail("SweetAdreno never opened", KERN_FAILURE);
	}
	fprintf(logf, "SweetAdreno opened\n");

	(void)get_param(connect, MSM_PARAM_CHIP_ID, "chip id");
	(void)get_param(connect, MSM_PARAM_GMEM_SIZE, "gmem bytes");
	(void)get_param(connect, MSM_PARAM_VA_START, "VA start");

	/* Two objects: commands and data. */
	uint64_t cmd_handle = 0, cmd_gpu = 0, data_handle = 0, data_gpu = 0;
	in[0] = 16384;
	nout = 1;
	if (scalar(connect, SEL_GEM_NEW, in, 1, out, &nout) != kIOReturnSuccess || nout != 1) {
		return fail("GEM_NEW commands", k);
	}
	cmd_handle = out[0];
	nout = 1;
	if (scalar(connect, SEL_GEM_NEW, in, 1, out, &nout) != kIOReturnSuccess || nout != 1) {
		return fail("GEM_NEW data", k);
	}
	data_handle = out[0];
	in[0] = cmd_handle;
	nout = 2;
	if (scalar(connect, SEL_GEM_INFO, in, 1, out, &nout) != kIOReturnSuccess) {
		return fail("GEM_INFO commands", k);
	}
	cmd_gpu = out[0];
	in[0] = data_handle;
	nout = 2;
	if (scalar(connect, SEL_GEM_INFO, in, 1, out, &nout) != kIOReturnSuccess) {
		return fail("GEM_INFO data", k);
	}
	data_gpu = out[0];
	fprintf(logf, "commands handle %llu at the core's %#llx, data handle %llu at %#llx\n",
	    cmd_handle, cmd_gpu, data_handle, data_gpu);

	/* The command object in this task: build the indirect buffer in it. */
	mach_vm_address_t map_addr = 0;
	mach_vm_size_t map_size = 0;
	k = IOConnectMapMemory(connect, (uint32_t)cmd_handle, mach_task_self(), &map_addr, &map_size,
	    kIOMapAnywhere);
	if (k != KERN_SUCCESS) {
		return fail("IOConnectMapMemory(commands)", k);
	}
	volatile uint32_t *ib = (volatile uint32_t *)map_addr;
	uint32_t n = 0;
	ib[n++] = pkt7(CP_MEM_WRITE, 2 + 4);
	ib[n++] = (uint32_t)data_gpu;
	ib[n++] = (uint32_t)(data_gpu >> 32);
	for (int i = 0; i < 4; i++) {
		ib[n++] = words[i];
	}
	fprintf(logf, "IB of %u words built at %#llx (mapped %llu bytes)\n", n, (unsigned long long)map_addr,
	    (unsigned long long)map_size);

	/* Submit and wait. */
	in[0] = cmd_handle;
	in[1] = 0;
	in[2] = n;
	nout = 1;
	k = scalar(connect, SEL_GEM_SUBMIT, in, 3, out, &nout);
	if (k != kIOReturnSuccess) {
		return fail("GEM_SUBMIT", k);
	}
	uint64_t fence = out[0];
	fprintf(logf, "GEM_SUBMIT -> fence %llu\n", fence);
	in[0] = fence;
	in[1] = 2000;
	nout = 1;
	k = scalar(connect, SEL_WAIT_FENCE, in, 2, out, &nout);
	if (k != kIOReturnSuccess) {
		return fail("WAIT_FENCE", k);
	}
	fprintf(logf, "WAIT_FENCE(%llu, 2000 ms) -> %s\n", fence, out[0] ? "signalled" : "NOT signalled");
	if (!out[0]) {
		goto out;
	}

	/* The data object in this task: read back what the core wrote. */
	mach_vm_address_t data_addr = 0;
	mach_vm_size_t data_size = 0;
	k = IOConnectMapMemory(connect, (uint32_t)data_handle, mach_task_self(), &data_addr, &data_size,
	    kIOMapAnywhere);
	if (k != KERN_SUCCESS) {
		return fail("IOConnectMapMemory(data)", k);
	}
	volatile uint32_t *got = (volatile uint32_t *)data_addr;
	int right = got[0] == words[0] && got[1] == words[1] && got[2] == words[2] && got[3] == words[3];
	int pass = right;
	fprintf(logf, "the data object reads %#x %#x %#x %#x: %s\n", got[0], got[1], got[2], got[3],
	    right ? "as the core was told to write (USER SPACE DRAWS WITH THE GPU)" :
	    "NOT what the core was told to write");

out:
	in[0] = cmd_handle;
	(void)scalar(connect, SEL_GEM_CLOSE, in, 1, out, &nout);
	in[0] = data_handle;
	(void)scalar(connect, SEL_GEM_CLOSE, in, 1, out, &nout);
	fprintf(logf, "sweet-gpu-test: %s\n", pass ? "PASS" : "FAIL");
	fflush(logf);
	IOServiceClose(connect);
	return pass ? 0 : 1;
}

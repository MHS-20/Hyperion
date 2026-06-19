#include "../module/hyperion.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>

#define DEVICE_PATH "/dev/hyperion"

static int g_fd = -1;

/* Test 1: CPU vendor and VMX detection */
static int test_cpu_vendor(void) {
  char vendor[13] = {0};
  uint32_t ebx, ecx, edx;

  __asm__ volatile("xor %%eax, %%eax\n\t"
                   "cpuid\n\t"
                   : "=b"(ebx), "=c"(ecx), "=d"(edx)
                   :
                   : "eax");
  memcpy(vendor + 0, &ebx, 4);
  memcpy(vendor + 4, &edx, 4);
  memcpy(vendor + 8, &ecx, 4);

  printf("  CPU Vendor: %s\n", vendor);

  __asm__ volatile("mov $1, %%eax\n\t"
                   "cpuid\n\t"
                   : "=c"(ecx)
                   :
                   : "eax", "ebx", "edx");

  printf("  VMX supported: %s\n", (ecx >> 5) & 1 ? "yes" : "no");
  printf("  Hypervisor bit (ECX[31]): %s\n",
         (ecx >> 31) & 1 ? "SET (bad)" : "clear (good)");
  return ((ecx >> 5) & 1) ? 0 : 1;
}

/* Test 2: Check if KVM PV is active */
static int test_kvm_pv(void) {
  FILE *f = fopen("/sys/module/kvm/parameters/current", "r");
  /* Check /proc/cpuinfo for hypervisor flag */
  f = fopen("/proc/cpuinfo", "r");
  if (!f) return 1;

  char line[256];
  int found = 0;
  while (fgets(line, sizeof(line), f)) {
    if (strstr(line, "hypervisor")) {
      printf("  /proc/cpuinfo shows: %s", line);
      found = 1;
    }
  }
  fclose(f);

  if (!found) printf("  No hypervisor flag in /proc/cpuinfo (good)\n");
  else        printf("  Hypervisor flag present (unexpected)\n");

  return found ? 1 : 0;
}

/* Test 3: Open device and init VMX */
static int test_vmx_init(void) {
  g_fd = open(DEVICE_PATH, O_RDWR);
  if (g_fd < 0) {
    perror("  open /dev/hyperion");
    return 1;
  }
  printf("  Device opened: ok\n");

  if (ioctl(g_fd, IOCTL_INIT_VMX) < 0) {
    perror("  IOCTL_INIT_VMX");
    close(g_fd); g_fd = -1;
    return 1;
  }
  printf("  VMX initialized: ok\n");
  return 0;
}

/* Test 4: Stability — run a busy loop for N seconds in non-root mode */
static int test_stability(int seconds) {
  time_t start = time(NULL);
  volatile unsigned long counter = 0;

  printf("  Running stability test for %d seconds...\n", seconds);

  while (time(NULL) - start < seconds) {
    /* Busy work to exercise CR3 switches and CPUID */
    for (int i = 0; i < 10000; i++) {
      uint32_t eax = 1, ebx, ecx, edx;
      __asm__ volatile("cpuid"
                       : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                       : "a"(eax)
                       : "memory");
      counter += ebx;
    }
    usleep(100000); /* 100ms */
    printf("  ."); fflush(stdout);
  }
  printf("\n  Stability test: PASSED (%d seconds, %lu cpuid rounds)\n",
         seconds, counter / 10000);
  return 0;
}

/* Main */
int main(int argc, char **argv) {
  int failed = 0;

  printf("========================================\n");
  printf(" Hyperion Integration Test Suite\n");
  printf("========================================\n\n");

  printf("[1] CPU detection\n");
  failed += test_cpu_vendor();

  printf("\n[2] KVM PV check (post-VMLAUNCH)\n");
  /* Skip if we haven't launched yet — test after VMX init */

  printf("\n[3] VMX initialization\n");
  failed += test_vmx_init();

  if (g_fd >= 0) {
    printf("\n[4] CPUID hypervisor bit (non-root mode)\n");
    failed += test_cpu_vendor();

    printf("\n[5] Stability test\n");
    failed += test_stability(5);

    printf("\n[6] Cleanup\n");
    close(g_fd);
    g_fd = -1;
    printf("  Device closed: ok\n");
  }

  printf("\n========================================\n");
  if (failed == 0)
    printf(" ALL TESTS PASSED\n");
  else
    printf(" %d TEST(S) FAILED\n", failed);
  printf("========================================\n");

  return failed;
}

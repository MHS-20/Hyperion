#include "../module/hyperion.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define DEVICE_PATH "/dev/hyperion"

void get_cpu_vendor(char *vendor) {
  uint32_t ebx, ecx, edx;

  __asm__ volatile("xor %%eax, %%eax\n\t" /* EAX = 0: vendor string query */
                   "cpuid\n\t"
                   : "=b"(ebx), "=c"(ecx), "=d"(edx)
                   :
                   : "eax");

  /* Vendor string is in EBX, EDX, ECX (in that order) */
  memcpy(vendor + 0, &ebx, 4);
  memcpy(vendor + 4, &edx, 4);
  memcpy(vendor + 8, &ecx, 4);
  vendor[12] = '\0';
}

int detect_vmx_support(void) {
  uint32_t ecx;

  __asm__ volatile("mov $1, %%eax\n\t" /* EAX = 1: feature flags query */
                   "cpuid\n\t"
                   : "=c"(ecx)
                   :
                   : "eax", "ebx", "edx");

  /* Bit 5 of ECX indicates VMX support */
  return (ecx >> 5) & 1;
}

int main(void) {
  char vendor[13];

  get_cpu_vendor(vendor);
  printf("[*] CPU Vendor: %s\n", vendor);

  if (strcmp(vendor, "GenuineIntel") == 0) {
    printf("[*] Processor virtualization technology is VT-x.\n");
  } else {
    printf("[*] This program is not designed for a non-VT-x environment!\n");
    return 1;
  }

  if (detect_vmx_support()) {
    printf("[*] VMX Operation is supported by your processor.\n");
  } else {
    printf("[*] VMX Operation is not supported by your processor.\n");
    return 1;
  }

  /* Open /dev/hyperion — this triggers dev_open() in the kernel module,
   * which is where we will initialize VMX operation */
  int fd = open(DEVICE_PATH, O_RDWR);
  if (fd < 0) {
    perror("[!] Failed to open " DEVICE_PATH);
    return 1;
  }

  /* Initialize VMX across all logical processors */
  if (ioctl(fd, IOCTL_INIT_VMX) < 0) {
    perror("[!] IOCTL_INIT_VMX failed");
    close(fd);
    return 1;
  }
  printf("[*] VMX initialized successfully.\n");

  printf("[*] Device opened successfully. Press Enter to exit...\n");
  getchar();

  close(fd);
  return 0;
}

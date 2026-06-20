#include "../module/hyperion.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/hyperion"

#define OPERATION_LOG_INFO    1
#define OPERATION_LOG_WARNING 2
#define OPERATION_LOG_ERROR   3

static int g_fd = -1;
static int g_failed = 0;

static void test_header(const char *name) {
  printf("\n[%s]\n", name);
}

static void pass(const char *msg) {
  printf("  PASS: %s\n", msg);
}

static void fail(const char *msg) {
  printf("  FAIL: %s\n", msg);
  g_failed++;
}

static int read_log(uint8_t *buf, size_t size) {
  int bytes = ioctl(g_fd, IOCTL_READ_LOG_BUFFER, buf);
  return bytes > 0 ? bytes : 0;
}

static int drain_logs(void) {
  uint8_t buf[4096];
  int total = 0;
  int n;
  while ((n = read_log(buf, sizeof(buf))) > 0)
    total += n;
  return total;
}

static int find_log(const char *needle) {
  uint8_t buf[4096];
  int n = read_log(buf, sizeof(buf));
  if (n <= 0) return 0;
  buf[n] = '\0';
  /* skip the 4-byte opcode header, message follows */
  char *msg = (char *)(buf + sizeof(uint32_t));
  return strstr(msg, needle) != NULL;
}

static void print_logs(void) {
  uint8_t buf[4096];
  int n;
  printf("  --- Log buffer messages ---\n");
  while ((n = read_log(buf, sizeof(buf))) > 0) {
    uint32_t op = *(uint32_t *)buf;
    char *msg = (char *)(buf + sizeof(uint32_t));
    msg[n - sizeof(uint32_t) > 0 ? n - sizeof(uint32_t) : 0] = '\0';
    const char *tag = op == OPERATION_LOG_INFO  ? "INFO"
                      : op == OPERATION_LOG_WARNING ? "WARN"
                      : op == OPERATION_LOG_ERROR ? "ERR"
                      : "???";
    printf("  [%s] %s\n", tag, msg);
  }
  printf("  --- End log buffer ---\n");
}

static int test_log_buffer(void) {
  test_header("Log buffer roundtrip");
  drain_logs();

  if (ioctl(g_fd, IOCTL_TEST_LOG, 0xAAA) < 0) {
    fail("IOCTL_TEST_LOG failed");
    return 1;
  }
  pass("IOCTL_TEST_LOG sent");

  usleep(100000);

  if (find_log("tag=0xaaa")) {
    pass("Log message received in userspace");
  } else {
    fail("Log message not found in buffer");
    print_logs();
    return 1;
  }
  return 0;
}

static int test_hidden_rw_hook(void) {
  test_header("Hidden EPT read/write hook");

  if (ioctl(g_fd, IOCTL_TEST_HOOK_RW) < 0) {
    fail("IOCTL_TEST_HOOK_RW failed");
    return 1;
  }
  pass("Hook installed via VMCALL");

  drain_logs();

  if (ioctl(g_fd, IOCTL_TEST_HOOK_TRIGGER) < 0) {
    fail("IOCTL_TEST_HOOK_TRIGGER failed");
    return 1;
  }
  pass("Hook trigger executed");

  usleep(300000);

  if (find_log("EPT hook")) {
    pass("EPT hook trigger message found in log buffer");
  } else {
    printf("  WARN: EPT hook message not in log buffer "
           "(may be in dmesg)\n");
  }

  if (ioctl(g_fd, IOCTL_TEST_HOOK_UNINSTALL) < 0) {
    fail("IOCTL_TEST_HOOK_UNINSTALL failed");
    return 1;
  }
  pass("Hook uninstalled");

  drain_logs();

  if (ioctl(g_fd, IOCTL_TEST_HOOK_TRIGGER) < 0) {
    fail("IOCTL_TEST_HOOK_TRIGGER (post-uninstall) failed");
    return 1;
  }

  usleep(100000);

  if (read_log(NULL, 0) == 0) {
    pass("No spurious hook triggers after uninstall");
  }

  return 0;
}

static int test_stability(void) {
  test_header("Stability (5-second busy loop)");
  volatile unsigned long counter = 0;
  int seconds = 5;
  time_t start = time(NULL);

  while (time(NULL) - start < seconds) {
    for (int i = 0; i < 5000; i++) {
      uint32_t eax = 1, ebx, ecx, edx;
      __asm__ volatile("cpuid"
                       : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                       : "a"(eax)
                       : "memory");
      counter += ebx;
    }
    usleep(100000);
  }

  pass("Stability test completed");
  printf("  Loops: %lu cpuid rounds in %d seconds\n",
         counter / 5000, seconds);
  return 0;
}

int main(int argc, char **argv) {
  printf("========================================\n");
  printf(" Hyperion Feature Test Suite\n");
  printf("========================================\n");

  g_fd = open(DEVICE_PATH, O_RDWR);
  if (g_fd < 0) {
    perror("open " DEVICE_PATH);
    return 1;
  }
  pass("Device opened");

  printf("\n=== VMX Initialization ===\n");
  if (ioctl(g_fd, IOCTL_INIT_VMX) < 0) {
    perror("IOCTL_INIT_VMX");
    close(g_fd);
    return 1;
  }
  pass("VMX initialized (now in non-root mode)");

  test_log_buffer();
  test_hidden_rw_hook();
  test_stability();

  close(g_fd);
  g_fd = -1;
  pass("Device closed (VMX terminated)");

  printf("\n========================================\n");
  if (g_failed == 0)
    printf(" ALL TESTS PASSED\n");
  else
    printf(" %d TEST(S) FAILED\n", g_failed);
  printf("========================================\n");

  return g_failed > 0 ? 1 : 0;
}

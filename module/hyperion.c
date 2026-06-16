#include "hyperion.h"
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <sys/ioctl.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Muhamad Huseyn Salman");
MODULE_DESCRIPTION("Hyperionn Kernel Module");

#define DEVICE_NAME "hyperion"
#define CLASS_NAME "hyperion"

static int major_number;
static struct class *hypervisor_class = NULL;
static struct device *hypervisor_device = NULL;

static inline void enable_vmx_operation(void) {
  unsigned long cr4;

  __asm__ volatile("mov %%cr4, %0\n\t"
                   "or  %1,    %0\n\t" /* Set bit 13 (VMXE) */
                   "mov %0,    %%cr4\n\t"
                   : "=&r"(cr4)
                   : "r"(1UL << 13)
                   : "memory");
}

/* --------- File Operations --------*/
/* Called when userspace opens /dev/my_hypervisor */
static int dev_open(struct inode *inodep, struct file *filep) {
  enable_vmx_operation();
  printk(KERN_INFO "[*] Hyperion: VMX Operation Enabled Successfully!\n");
  return 0;
}

/* Called when userspace closes /dev/my_hypervisor */
static int dev_release(struct inode *inodep, struct file *filep) {
  printk(KERN_INFO "Hyperion: device closed\n");
  return 0;
}

static ssize_t dev_read(struct file *filep, char __user *buf, size_t len,
                        loff_t *offset) {
  printk(KERN_INFO "[*] Hyperion: read() not implemented yet\n");
  return 0;
}

static ssize_t dev_write(struct file *filep, const char __user *buf, size_t len,
                         loff_t *offset) {
  printk(KERN_INFO "[*] Hyperion: write() not implemented yet\n");
  return len;
}

static long dev_ioctl(struct file *filep, unsigned int cmd, unsigned long arg) {
  printk(KERN_INFO "[*] Hyperion: ioctl() called with cmd=0x%x\n", cmd);
  return 0;
}

/* File operations exposed to userspace */
static struct file_operations fops = {.open = dev_open,
                                      .release = dev_release,
                                      .read = dev_read,
                                      .write = dev_write,
                                      .unlocked_ioctl = dev_ioctl};

/* module/hyperion.c — IOCTL handler */

static long dev_ioctl(struct file *filep, unsigned int cmd, unsigned long arg) {
  int ret = 0;

  /* Verify the command belongs to us */
  if (_IOC_TYPE(cmd) != HYPERION_MAGIC) {
    printk(KERN_WARNING "[*] Hyperion: unknown IOCTL type\n");
    return -ENOTTY;
  }

  switch (cmd) {

  case IOCTL_INIT_VMX:
    printk(KERN_INFO "[*] Hyperion: IOCTL_INIT_VMX received\n");
    if (initialize_vmx())
      printk(KERN_INFO "[*] Hyperion: VMX initialized successfully\n");
    else {
      printk(KERN_ERR "[*] Hyperion: VMX initialization failed\n");
      ret = -EPERM;
    }
    break;

  case IOCTL_TERMINATE_VMX:
    printk(KERN_INFO "[*] Hyperion: IOCTL_TERMINATE_VMX received\n");
    terminate_vmx();
    break;

  case IOCTL_SEND_BUFFER: {
    char buf[256];
    if (copy_from_user(buf, (char __user *)arg, sizeof(buf))) {
      ret = -EFAULT;
      break;
    }
    printk(KERN_INFO "[*] Hyperion: received buffer from userspace: %s\n", buf);
    break;
  }

  default:
    printk(KERN_WARNING "[*] Hyperion: unrecognized IOCTL 0x%x\n", cmd);
    ret = -ENOTTY;
    break;
  }

  return ret;
}

/*-------- Init and Exit ---------*/
static int __init hypervisor_init(void) {
  printk(KERN_INFO "Hyperion: module loading\n");

  /* Dynamically allocate a major number for the device */
  major_number = register_chrdev(0, DEVICE_NAME, &fops);
  if (major_number < 0) {
    printk(KERN_ALERT "Hyperion: failed to register a major number\n");
    return major_number;
  }

  /* Register the device class (appears under /sys/class/) */
  hypervisor_class = class_create(CLASS_NAME);
  if (IS_ERR(hypervisor_class)) {
    unregister_chrdev(major_number, DEVICE_NAME);
    printk(KERN_ALERT "Hyperion: failed to register device class\n");
    return PTR_ERR(hypervisor_class);
  }

  /* Create the device node at /dev/my_hypervisor */
  hypervisor_device = device_create(hypervisor_class, NULL,
                                    MKDEV(major_number, 0), NULL, DEVICE_NAME);
  if (IS_ERR(hypervisor_device)) {
    class_destroy(hypervisor_class);
    unregister_chrdev(major_number, DEVICE_NAME);
    printk(KERN_ALERT "Hyperion: failed to create device\n");
    return PTR_ERR(hypervisor_device);
  }

  printk(KERN_INFO "Hyperion: device created at /dev/%s\n", DEVICE_NAME);
  return 0;
}

/* Equivalent to DrvUnload */
static void __exit hypervisor_exit(void) {
  device_destroy(hypervisor_class, MKDEV(major_number, 0));
  class_unregister(hypervisor_class);
  class_destroy(hypervisor_class);
  unregister_chrdev(major_number, DEVICE_NAME);
  printk(KERN_INFO "Hyperion: module unloaded\n");
}

module_init(hypervisor_init);
module_exit(hypervisor_exit);

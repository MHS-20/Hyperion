#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Muhamad Huseyn Salman");
MODULE_DESCRIPTION("Hyperionn Kernel Module");

#define DEVICE_NAME "hyperion"
#define CLASS_NAME "hyperion"

static int major_number;
static struct class *hypervisor_class = NULL;
static struct device *hypervisor_device = NULL;

/* Called when userspace opens /dev/my_hypervisor */
static int dev_open(struct inode *inodep, struct file *filep) {
  printk(KERN_INFO "Hyperion: device opened\n");
  return 0;
}

/* Called when userspace closes /dev/my_hypervisor */
static int dev_release(struct inode *inodep, struct file *filep) {
  printk(KERN_INFO "Hyperion: device closed\n");
  return 0;
}

/* File operations exposed to userspace — expand later for IOCTL */
static struct file_operations fops = {
    .open = dev_open,
    .release = dev_release,
};

/* Equivalent to DriverEntry */
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

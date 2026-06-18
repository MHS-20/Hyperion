obj-m += hyperion.o
hyperion-objs := driver.o vmx.o memory.o ept.o exit_handler.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) clean

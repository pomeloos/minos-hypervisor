ARCH 		:= aarch64
CROSS_COMPILE 	:= aarch64-linux-gnu-
CC 		:= $(CROSS_COMPILE)gcc
LD 		:= $(CROSS_COMPILE)ld
OBJ_COPY	:= $(CROSS_COMPILE)objcopy
OBJ_DUMP 	:= $(CROSS_COMPILE)objdump
NM		:= $(CROSS_COMPILE)nm
STRIP		:= $(CROSS_COMPILE)strip

PWD		:= $(shell pwd)

QUIET ?= @

ifeq ($(QUIET),@)
PROGRESS = @echo Compiling $@ ...
endif

TARGET := mvm

CCFLAG := -Wall -D_XOPEN_SOURCE -D_GNU_SOURCE -march=armv8-a \
	-Wundef -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing \
	-fno-common -fshort-wchar -Werror-implicit-function-declaration \
	-Wno-format-security -I$(PWD)/include -I$(PWD)/../include

src	:= main/mvm.c main/bootimage.c main/os_linux.c main/os_other.c
src	+= libfdt/fdt_addresses.c libfdt/fdt.c libfdt/fdt_empty_tree.c
src	+= libfdt/fdt_ro.c libfdt/fdt_rw.c libfdt/fdt_strerror.c
src	+= libfdt/fdt_sw.c libfdt/fdt_wip.c libfdt/fdt_overlay.c
src	+= main/mevent.c
src	+= main/mvm_queue.c
src	+= devices/vdev.c
src	+= devices/virtio/virtio.c
src	+= devices/virtio/virtio_console.c
src	+= devices/block_if.c
src	+= devices/virtio/virtio_block.c
src	+= devices/virtio/virtio_net.c

INCLUDE_DIR = include/libfdt include ../include

objs	:= $(src:%.c=%.o)

$(TARGET) : $(objs)
	$(PROGRESS)
	$(QUIET) $(CC) $^ -o $@ $(CCFLAG) -lpthread
	$(QUIET) $(STRIP) -s $(TARGET)

%.o : %.c $(INCLUDE_DIR) Makefile
	$(PROGRESS)
	$(QUIET) $(CC) $(CCFLAG) -c $< -o $@

.PHONY: clean

clean:
	$(QUIET) echo "remove all things"
	$(QUIET) rm -rf $(TARGET) $(objs)

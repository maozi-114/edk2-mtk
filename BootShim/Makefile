ARCH              = arm64
TARGET            = aarch64-linux-gnu
CROSS_COMPILE     = $(TARGET)-
CC                = $(CROSS_COMPILE)gcc
OBJCOPY           = $(CROSS_COMPILE)objcopy

ifneq ($(MAKECMDGOALS),clean)
ifndef UEFI_BASE
$(error UEFI_BASE must be set)
endif
ifndef UEFI_SIZE
$(error UEFI_SIZE must be set)
endif
ifeq ($(UEFI_BASE),0)
$(error UEFI_BASE must not be zero)
endif
ifeq ($(UEFI_SIZE),0)
$(error UEFI_SIZE must not be zero)
endif
endif

.PHONY: all clean
all: BootShim.elf BootShim.bin

BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary $< $@

BootShim.elf: BootShim.S
	$(CC) -c $< -o $@ -DUEFI_BASE=$(UEFI_BASE) -DUEFI_SIZE=$(UEFI_SIZE)

BootShim.S:

clean:
	rm -f BootShim.elf BootShim.bin
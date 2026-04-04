# ---------------------------------------------------------------------------
# Cross-compiler detection
#
# CentOS / RHEL ship an ARM cross-compiler with a different tuple than
# Debian/Ubuntu.  Check in order of preference:
#
#   1. arm-linux-gnueabihf-gcc   (Debian/Ubuntu, ARM's own toolchain)
#   2. arm-linux-gnu-gcc         (CentOS/RHEL EPEL package: arm-linux-gnu-gcc)
#   3. armv7hl-redhat-linux-gnueabi-gcc  (older RHEL devtoolset variant)
#
# Override on the command line: make CROSS=arm-linux-gnueabihf-
# ---------------------------------------------------------------------------
ifeq ($(CROSS),)
  ifneq ($(shell which arm-linux-gnueabihf-gcc 2>/dev/null),)
    CROSS = arm-linux-gnueabihf-
  else ifneq ($(shell which arm-linux-gnu-gcc 2>/dev/null),)
    CROSS = arm-linux-gnu-
  else ifneq ($(shell which armv7hl-redhat-linux-gnueabi-gcc 2>/dev/null),)
    CROSS = armv7hl-redhat-linux-gnueabi-
  else
    $(error No ARM cross-compiler found. \
      On CentOS 8/Stream: sudo dnf install arm-linux-gnu-gcc (EPEL). \
      On CentOS 7: install ARM GNU Toolchain from developer.arm.com. \
      Then re-run or set CROSS= explicitly.)
  endif
endif

CC      = $(CROSS)gcc
AS      = $(CROSS)as
LD      = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy
OBJDUMP = $(CROSS)objdump
SIZE    = $(CROSS)size

# ARM Cortex-A7 in AArch32, hard-float ABI
# -D_DEFAULT_SOURCE ensures POSIX/BSD extensions are visible in any libc
# headers pulled in by freestanding includes (edge case on CentOS sysroot).
CFLAGS  = -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
          -marm -mno-thumb-interwork \
          -Os -Wall -Wextra \
          -D_DEFAULT_SOURCE \
          -ffreestanding -fno-builtin -fno-stack-protector \
          -fdata-sections -ffunction-sections \
          -nostdlib -nostdinc \
          -I.

LDFLAGS = -T linker.ld \
          -nostdlib \
          --gc-sections \
          --no-undefined \
          -Map=$(TARGET).map

TARGET  = bl2_sta8600
SRCS_C  = bootloader.c pka_rsa.c
SRCS_S  = start.S
OBJS    = $(SRCS_S:.S=.o) $(SRCS_C:.c=.o)

.PHONY: all clean dis info

all: $(TARGET).bin

$(TARGET).elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary --strip-debug $< $@
	@echo "BL2 binary: $@  ($(shell wc -c < $@) bytes)"

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

dis: $(TARGET).elf
	$(OBJDUMP) -d $< > $(TARGET).dis

# Show which cross-compiler was selected
info:
	@echo "Cross prefix : $(CROSS)"
	@$(CC) --version | head -1

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).bin $(TARGET).map $(TARGET).dis

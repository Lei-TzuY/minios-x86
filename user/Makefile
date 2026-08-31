CC      = gcc
AS      = as
LD      = ld
CFLAGS  = -m32 -nostdlib -fno-stack-protector -fno-pie -no-pie \
          -ffreestanding -O2 -Wall -Wextra
DEPFLAGS = -MMD -MP
LOAD    = 0x310000
PROGRAMS = hello.elf cat.elf fault.elf badptr.elf worker.elf spawner.elf \
	   orphan.elf sleeptest.elf fstest.elf echo.elf malloctest.elf wc.elf grep.elf \
	   head.elf tail.elf sort.elf sigtest.elf sigipc.elf forktest.elf execdemo.elf \
	   demandtest.elf sigchld.elf waitdemo.elf cwddemo.elf statdemo.elf cowstress.elf \
	   alarmdemo.elf pausedemo.elf pipedemo.elf jobctl.elf uptime.elf date.elf \
	   printenv.elf cputime.elf shmtest.elf semtest.elf mmaptest.elf \
	   threadtest.elf threadexit.elf execguard.elf ramgrow.elf pathlim.elf redirref.elf fatref.elf fatgrow.elf forkredir.elf sigretguard.elf sigflags.elf killthread.elf killwait.elf bigseek.elf stress.elf ush.elf
DEPS = $(PROGRAMS:.elf=.d)

.PHONY: all clean

all: $(PROGRAMS)

%.elf: crt0.o %.o
	$(LD) -m elf_i386 --build-id=none -Ttext=$(LOAD) -e _start -o $@ $^

crt0.o: crt0.s
	$(AS) --32 $< -o $@

%.o: %.c
	$(CC) -c $(CFLAGS) $(DEPFLAGS) $< -o $@

-include $(DEPS)

clean:
	rm -f *.o *.d $(PROGRAMS)

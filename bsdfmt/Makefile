# .bsd executable format — host tools and examples.
#
#   make            build bsd_maker, bsd_run and both example images
#   make demo       build, then load and run the examples
#   make inspect    map and protect the examples without calling them
#                   (works on any host; running needs an x86_64 CPU)
#
# The examples are x86_64 machine code, so they are assembled with a
# cross toolchain.  On an x86_64 Linux host, use the native binutils:
#
#   make CROSS=

CROSS   ?= x86_64-elf-
AS      := $(CROSS)as
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11

IMAGES  := factorial.bsd table.bsd

.PHONY: all demo inspect clean
.PRECIOUS: %.o %.elf %.bin

all: bsd_maker bsd_run $(IMAGES)

# ---- host tools ----

bsd_maker: bsd_maker.c bsd_format.h
	$(CC) $(CFLAGS) -o $@ bsd_maker.c

bsd_run: bsd_run.c bsd_format.h
	$(CC) $(CFLAGS) -o $@ bsd_run.c

# ---- raw mode: a flat blob of machine code straight into the container ----

example.o: example.s
	$(AS) -o $@ $<

example.bin: example.o
	$(OBJCOPY) -O binary --only-section=.text $< $@

factorial.bsd: example.bin bsd_maker
	./bsd_maker -o $@ -t example.bin -v

# ---- elf mode: the linker decides the text/data split and the bss size ----

example_data.o: example_data.s
	$(AS) -o $@ $<

example_data.elf: example_data.o bsd.ld
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T bsd.ld $< -o $@

table.bsd: example_data.elf bsd_maker
	./bsd_maker -o $@ -e example_data.elf -v

# ---- running ----

demo: all
	@echo
	@echo "=== factorial.bsd : text only, raw machine code ==="
	./bsd_run -a 10 factorial.bsd
	@echo
	@echo "=== table.bsd : text + data + bss, packed from ELF ==="
	./bsd_run table.bsd

inspect: all
	@echo
	./bsd_run -n factorial.bsd
	@echo
	./bsd_run -n table.bsd

clean:
	rm -f bsd_maker bsd_run *.o *.bin *.elf *.bsd

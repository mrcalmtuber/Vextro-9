# .vx executable format — host tools and examples.
#
#   make            build vx_maker, vx_run and both example images
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

IMAGES  := factorial.vx table.vx

.PHONY: all demo inspect clean
.PRECIOUS: %.o %.elf %.bin

all: vx_maker vx_run $(IMAGES)

# ---- host tools ----

vx_maker: vx_maker.c vx_format.h
	$(CC) $(CFLAGS) -o $@ vx_maker.c

vx_run: vx_run.c vx_format.h
	$(CC) $(CFLAGS) -o $@ vx_run.c

# ---- raw mode: a flat blob of machine code straight into the container ----

example.o: example.s
	$(AS) -o $@ $<

example.bin: example.o
	$(OBJCOPY) -O binary --only-section=.text $< $@

factorial.vx: example.bin vx_maker
	./vx_maker -o $@ -t example.bin -v

# ---- elf mode: the linker decides the text/data split and the bss size ----

example_data.o: example_data.s
	$(AS) -o $@ $<

example_data.elf: example_data.o vx.ld
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T vx.ld $< -o $@

table.vx: example_data.elf vx_maker
	./vx_maker -o $@ -e example_data.elf -v

# ---- running ----

demo: all
	@echo
	@echo "=== factorial.vx : text only, raw machine code ==="
	./vx_run -a 10 factorial.vx
	@echo
	@echo "=== table.vx : text + data + bss, packed from ELF ==="
	./vx_run table.vx

inspect: all
	@echo
	./vx_run -n factorial.vx
	@echo
	./vx_run -n table.vx

clean:
	rm -f vx_maker vx_run *.o *.bin *.elf *.vx

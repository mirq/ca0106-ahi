# CA0106 / Sound Blaster Audigy SE (SB0570) AHI driver for AmigaOS 68k
# Toolchain: m68k-amigaos-gcc 16 (bebbo-style), vasm

PREFIX  ?= /opt/amiga
CC      := $(PREFIX)/bin/m68k-amigaos-gcc
STRIP   := $(PREFIX)/bin/m68k-amigaos-strip
VASM    := $(PREFIX)/bin/vasmm68k_mot

AHIINC  ?= ../ahidev_4.18/AHI/Developer/include/C
INCS    := -Iinclude -Isrc -I$(AHIINC)

CPU     := -m68020-60 -mtune=68060
WARN    := -Wall -Wno-pointer-sign
COPT    := -Os -fomit-frame-pointer -fno-common

# tools: libnix startup + printf
TCFLAGS := $(CPU) $(COPT) $(WARN) $(INCS) -noixemul
# driver: no startup code, no libc
DCFLAGS := $(CPU) $(COPT) $(WARN) $(INCS) -noixemul -nostdlib -ffreestanding \
           -fno-builtin -DCA_DRIVER

B := build

TOOLS   := $(B)/caprobe $(B)/catone $(B)/cactl $(B)/capci $(B)/ahitest $(B)/ahistream
DRIVER  := $(B)/ca0106.audio
MODES   := $(B)/CA0106

all: $(TOOLS) $(DRIVER) $(MODES)

$(B):
	mkdir -p $(B)

HWSRC := src/ca0106_hw.c
HWHDR := src/ca0106_hw.h src/ca0106_regs.h include/inline/openpci.h include/inline/prometheus_dma.h

$(B)/%.tool.o: tools/%.c $(HWHDR) | $(B)
	$(CC) $(TCFLAGS) -c $< -o $@

$(B)/hw.tool.o: $(HWSRC) $(HWHDR) | $(B)
	$(CC) $(TCFLAGS) -c $< -o $@

$(B)/caprobe: $(B)/caprobe.tool.o $(B)/hw.tool.o
	$(CC) $(TCFLAGS) $^ -o $@

$(B)/catone: $(B)/catone.tool.o $(B)/hw.tool.o
	$(CC) $(TCFLAGS) $^ -o $@ -lm

$(B)/cactl: $(B)/cactl.tool.o $(B)/hw.tool.o
	$(CC) $(TCFLAGS) $^ -o $@ -lm

# ---- driver ----
DOBJS := $(B)/drv_lib.o $(B)/drv_audio.o $(B)/hw.drv.o

$(B)/drv_lib.o: driver/lib.c driver/driver.h $(HWHDR) | $(B)
	$(CC) $(DCFLAGS) -fno-toplevel-reorder -c $< -o $@

$(B)/drv_audio.o: driver/audio.c driver/driver.h $(HWHDR) | $(B)
	$(CC) $(DCFLAGS) -c $< -o $@

$(B)/hw.drv.o: $(HWSRC) $(HWHDR) | $(B)
	$(CC) $(DCFLAGS) -c $< -o $@

$(DRIVER): $(DOBJS)
	$(CC) $(DCFLAGS) $(DOBJS) -o $@.dbg -lgcc
	$(STRIP) --strip-debug --strip-unneeded -o $@ $@.dbg

$(MODES): driver/modefile.s | $(B)
	$(VASM) -quiet -Fbin -o $@ $<

dist: lha

# Amiga release archive: CA0106/ drawer with driver, mode file, tools, docs
VERSION ?= 4.2
lha: all
	rm -rf $(B)/lha && mkdir -p $(B)/lha/CA0106/Devs/AHI $(B)/lha/CA0106/Devs/AudioModes $(B)/lha/CA0106/C $(B)/lha/CA0106/Tools
	cp $(DRIVER) $(B)/lha/CA0106/Devs/AHI/
	cp $(MODES) $(B)/lha/CA0106/Devs/AudioModes/
	cp $(B)/cactl $(B)/lha/CA0106/C/
	cp $(B)/caprobe $(B)/catone $(B)/capci $(B)/ahitest $(B)/ahistream $(B)/lha/CA0106/Tools/
	cp dist/Install-CA0106 dist/CA0106-Debug dist/ca0106.readme README.md LICENSE $(B)/lha/CA0106/
	rm -f $(B)/ca0106-$(VERSION).lha
	cd $(B)/lha && lha -ao5q ../ca0106-$(VERSION).lha CA0106

clean:
	rm -rf $(B)

.PHONY: all clean dist lha

$(B)/capci: $(B)/capci.tool.o $(B)/hw.tool.o
	$(CC) $(TCFLAGS) $^ -o $@

$(B)/ahitest: $(B)/ahitest.tool.o $(B)/hw.tool.o
	$(CC) $(TCFLAGS) $^ -o $@ -lm

$(B)/ahistream: $(B)/ahistream.tool.o $(B)/hw.tool.o
	$(CC) $(TCFLAGS) $^ -o $@ -lm

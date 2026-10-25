# RMM V2 - build pipeline
#
# modules.map -> generator -> core pass-1 -> module links -> trampolines +
# archive + ABI tables -> core pass-2 -> rmm.elf
#
# Targets:
# make            -> build/rmm.elf (multiboot2, boots like before)
# make report     -> module sizes + validator output

ARCH      := x86_64
BUILD     := build
GEN       := $(BUILD)/generated
OBJ       := $(BUILD)/obj
NASM      ?= nasm
# build tooling lives in the rmm-tools submodule (tools/), pure C
RMMGEN    := tools/bin/rmm-gen

# arch gate: only x86_64 builds, arm64 is a documented skeleton
ifneq ($(ARCH),x86_64)
$(error ARCH=$(ARCH): only x86_64 is buildable; arm64 is a skeleton - see arch/README.md)
endif

$(RMMGEN): $(wildcard tools/src/*.c) $(wildcard tools/src/*.h) tools/Makefile
	@echo "  CC    tools (rmm-gen)"
	@$(MAKE) -C tools

ifeq ($(origin CROSS_PREFIX),environment)
CROSS := $(CROSS_PREFIX)
endif
ifeq ($(CROSS),)
CROSS := $(shell command -v $(ARCH)-elf-gcc 2>/dev/null | xargs dirname 2>/dev/null)/
endif
ifeq ($(CROSS),/)
CROSS :=
endif
ifneq ($(CROSS),)
CC := $(CROSS)$(ARCH)-elf-gcc
LD := $(CROSS)$(ARCH)-elf-ld
else
CC := gcc
LD := ld
endif

# freestanding kernel flags.  Core: small code model (identity-mapped).
# Modules: large model (slot VAs) + IBT endbr64 - additional layer.
CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -fno-pie -fno-common \
	  -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
	  -mno-avx -fno-asynchronous-unwind-tables -O2 -g \
	  -Wall -Wno-unused-function -Wno-unused-variable
CFLAGS_CORE := $(CFLAGS) -mcmodel=small
CFLAGS_MOD  := $(CFLAGS) -mcmodel=large -mstringop-strategy=loop \
	       -fcf-protection=branch

# every unit compiles against every include root (path-compatible with
# the old tree so ported sources compile unmodified)
INC := -Iinclude -Ishim -Icore -Imodules/fs -Imodules/drv_bus -Imodules/drv_gfx \
       -Imodules/drv_input -Iarch/x86_64/include -I$(GEN) -Iarch/x86_64/context

# sources
# percpu.c is compiled, cpu.c/lapic.c/madt.c/smp.c are WIP leftovers of
# the old tree and are excluded (msi.c provides lapic_init).
ARCH_CONTEXT_C := $(filter-out %/cpu.c %/lapic.c %/madt.c %/smp.c, \
	    $(wildcard arch/x86_64/context/*.c))
CORE_C   := $(wildcard core/console/*.c) $(wildcard core/drivers/timer/*.c) \
	    $(wildcard core/drivers/serial/*.c) $(wildcard core/drivers/vga/*.c) \
	    $(wildcard core/exec/*.c) $(wildcard core/kernel/*.c) \
	    $(wildcard core/modules/*.c) $(wildcard core/exceptions/*.c) \
	    $(wildcard core/loader/*.c) \
	    $(wildcard arch/x86_64/mm/*.c) $(wildcard arch/x86_64/traps/*.c) \
	    $(ARCH_CONTEXT_C) $(wildcard arch/x86_64/*.c)
CORE_ASM := arch/x86_64/boot/header.asm arch/x86_64/boot/main.asm arch/x86_64/boot/main64.asm \
	    core/exec/syscall_entry.S
CORE_OBJS := $(patsubst %.c,$(OBJ)/core/%.c.o,$(filter %.c,$(CORE_C))) \
	     $(patsubst %.asm,$(OBJ)/core/%.asm.o,$(filter %.asm,$(CORE_ASM))) \
	     $(OBJ)/core/exec/syscall_entry.S.o

MODS     := fs drv_bus drv_gfx drv_input
MOD_C_fs       := $(wildcard modules/fs/*.c)
MOD_C_drv_bus  := $(wildcard modules/drv_bus/pci/*.c) \
	          $(wildcard modules/drv_bus/ata/*.c) \
	          $(wildcard modules/drv_bus/core/*.c)
MOD_C_drv_gfx  := $(wildcard modules/drv_gfx/gpu/*.c) \
	          $(wildcard modules/drv_gfx/graphics/*.c)
MOD_C_drv_input:= $(wildcard modules/drv_input/input/*.c) \
	          $(wildcard modules/drv_input/ps2/*.c)
# module_main.c per module
MOD_C_fs       += modules/fs/module_main.c
MOD_C_drv_bus  += modules/drv_bus/module_main.c
MOD_C_drv_gfx  += modules/drv_gfx/module_main.c
MOD_C_drv_input+= modules/drv_input/module_main.c

# find_source: map object name -> real source (units share basenames rarely,
# exceptions: per-unit dirs are mirrored in build/obj/<unit>/)
define SRCDIR_FOR
$(patsubst %/,%,$(dir $(1)))
endef

.DEFAULT_GOAL := all
.PHONY: all clean report
all: $(BUILD)/rmm.elf

# phase 0: offsets (single source for asm/C boundary)
# offsets + bootstrap manifest headers: no object scan needed, breaks the
# build bootstrap cycle (all kernel objects include rmm/modules.generated.h)
$(GEN)/offsets.txt $(GEN)/rmm/modules.generated.h $(GEN)/modules.generated.c: \
	$(RMMGEN) include/rmm/module.h linker/modules.map
	@mkdir -p $(GEN)
	@$(RMMGEN) offsets
	@$(RMMGEN) manifest

# module_main objects (flat, one per module)
$(OBJ)/fs/module_main.o: modules/fs/module_main.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    fs/module_main.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@
$(OBJ)/drv_bus/module_main.o: modules/drv_bus/module_main.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    drv_bus/module_main.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@
$(OBJ)/drv_gfx/module_main.o: modules/drv_gfx/module_main.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    drv_gfx/module_main.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@
$(OBJ)/drv_input/module_main.o: modules/drv_input/module_main.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    drv_input/module_main.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@

# path-preserving object names: build/obj/core/<original/rel/path>.o
$(OBJ)/core/%.c.o: %.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS_CORE) $(INC) -c $< -o $@

$(OBJ)/core/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  ASM   $<"
	@$(NASM) -f elf64 $< -o $@

$(OBJ)/core/exec/syscall_entry.S.o: core/exec/syscall_entry.S
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS_CORE) -c $< -o $@

# core objects that live under modules/drv_bus/core (driver.c is core-owned
# infrastructure placed in drv_bus's slot? No - it belongs to CORE, the
# driver registry is called before modules load.  Actually driver.c lives
# in the drv_bus module, exclude it from core below.)

# module object compilation
$(OBJ)/fs/%.o: modules/fs/%.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    fs/$*.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@

$(OBJ)/drv_bus/%.o: modules/drv_bus/%.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    drv_bus/$*.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@
$(OBJ)/drv_bus/pci/%.o: modules/drv_bus/pci/%.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    drv_bus/pci/$*.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@
$(OBJ)/drv_bus/ata/%.o: modules/drv_bus/ata/%.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    drv_bus/ata/$*.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@
$(OBJ)/drv_bus/core/%.o: modules/drv_bus/core/%.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    drv_bus/core/$*.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@

$(OBJ)/drv_gfx/%.o: modules/drv_gfx/%.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    drv_gfx/$*.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@
$(OBJ)/drv_gfx/gpu/%.o: modules/drv_gfx/gpu/%.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    drv_gfx/gpu/$*.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@
$(OBJ)/drv_gfx/graphics/%.o: modules/drv_gfx/graphics/%.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    drv_gfx/graphics/$*.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@

$(OBJ)/drv_input/%.o: modules/drv_input/%.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    drv_input/$*.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@
$(OBJ)/drv_input/input/%.o: modules/drv_input/input/%.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    drv_input/input/$*.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@
$(OBJ)/drv_input/ps2/%.o: modules/drv_input/ps2/%.c $(GEN)/offsets.txt
	@mkdir -p $(dir $@)
	@echo "  CC    drv_input/ps2/$*.c"
	@$(CC) $(CFLAGS_MOD) $(INC) -c $< -o $@

# resolve module objects from the source lists
MOD_OBJS_fs        := $(patsubst %.c,$(OBJ)/fs/%.o,$(notdir $(filter-out %/module_main.c,$(MOD_C_fs)))) $(OBJ)/fs/module_main.o
MOD_OBJS_drv_bus   := $(patsubst %.c,$(OBJ)/drv_bus/%.o,$(filter-out modules/drv_bus/pci/% modules/drv_bus/ata/% modules/drv_bus/core/% %/module_main.c,$(MOD_C_drv_bus))) \
	              $(OBJ)/drv_bus/module_main.o \
	              $(patsubst %.c,$(OBJ)/drv_bus/pci/%.o,$(notdir $(filter modules/drv_bus/pci/%,$(MOD_C_drv_bus)))) \
	              $(patsubst %.c,$(OBJ)/drv_bus/ata/%.o,$(notdir $(filter modules/drv_bus/ata/%,$(MOD_C_drv_bus)))) \
	              $(patsubst %.c,$(OBJ)/drv_bus/core/%.o,$(notdir $(filter modules/drv_bus/core/%,$(MOD_C_drv_bus))))
MOD_OBJS_drv_gfx   := $(OBJ)/drv_gfx/module_main.o \
	              $(patsubst %.c,$(OBJ)/drv_gfx/gpu/%.o,$(notdir $(filter modules/drv_gfx/gpu/%,$(MOD_C_drv_gfx)))) \
	              $(patsubst %.c,$(OBJ)/drv_gfx/graphics/%.o,$(notdir $(filter modules/drv_gfx/graphics/%,$(MOD_C_drv_gfx))))
MOD_OBJS_drv_input := $(OBJ)/drv_input/module_main.o \
	              $(patsubst %.c,$(OBJ)/drv_input/input/%.o,$(notdir $(filter modules/drv_input/input/%,$(MOD_C_drv_input)))) \
	              $(patsubst %.c,$(OBJ)/drv_input/ps2/%.o,$(notdir $(filter modules/drv_input/ps2/%,$(MOD_C_drv_input))))

# phase 1: prelink generation
$(OBJ)/core/objlist.txt: $(CORE_OBJS) $(MOD_OBJS_fs) $(MOD_OBJS_drv_bus) \
	                 $(MOD_OBJS_drv_gfx) $(MOD_OBJS_drv_input)
	@mkdir -p $(dir $@)
	@echo "  GEN   objlist"
	@ls $(CORE_OBJS) $(MOD_OBJS_fs) $(MOD_OBJS_drv_bus) \
	    $(MOD_OBJS_drv_gfx) $(MOD_OBJS_drv_input) > $@


# after objects exist: stub table, aliases, core script, entry table
$(GEN)/rmm_stub.ld $(GEN)/core.ld $(GEN)/entry_table.S: \
	$(OBJ)/core/objlist.txt $(RMMGEN) linker/modules.map
	@echo "  GEN   prelink (stubs, aliases, core script)"
	@$(RMMGEN) prelink $(OBJ)/core/objlist.txt

# phase 2: core pass 1 + symbol table
$(BUILD)/core1.elf: $(CORE_OBJS) $(GEN)/core.ld $(OBJ)/core/modules.generated.c.o \
	            $(OBJ)/gen/entry_table.o
	@echo "  LD    core pass 1"
	@$(LD) -z max-page-size=0x1000 -T $(GEN)/core.ld -o $@ $(CORE_OBJS) $(OBJ)/core/modules.generated.c.o \
	    $(OBJ)/gen/entry_table.o

$(OBJ)/core/modules.generated.c.o: $(GEN)/modules.generated.c
	@$(CC) $(CFLAGS_CORE) $(INC) -c $< -o $@

# phase 3: module links (topological: drv_bus -> drv_gfx -> drv_input -> fs)
$(GEN)/core_syms.ld: $(BUILD)/core1.elf
	@$(RMMGEN) core-syms $<

$(BUILD)/modules/drv_bus.elf: $(MOD_OBJS_drv_bus) $(GEN)/core_syms.ld
	@echo "  GEN   drv_bus script+veneers"
	@$(RMMGEN) module drv_bus
	@$(CC) $(CFLAGS_MOD) $(INC) -c $(GEN)/drv_bus_veneers.S -o $(OBJ)/drv_bus/veneers.o
	@echo "  LD    module drv_bus"
	@mkdir -p $(BUILD)/modules
	@$(LD) -T $(GEN)/drv_bus.ld -o $@ $(MOD_OBJS_drv_bus) $(OBJ)/drv_bus/veneers.o

$(BUILD)/modules/drv_gfx.elf: $(MOD_OBJS_drv_gfx) $(BUILD)/modules/drv_bus.nm
	@echo "  GEN   drv_gfx script+veneers"
	@$(RMMGEN) module drv_gfx build/modules/drv_bus.nm
	@$(CC) $(CFLAGS_MOD) $(INC) -c $(GEN)/drv_gfx_veneers.S -o $(OBJ)/drv_gfx/veneers.o
	@echo "  LD    module drv_gfx"
	@$(LD) -T $(GEN)/drv_gfx.ld -o $@ $(MOD_OBJS_drv_gfx) $(OBJ)/drv_gfx/veneers.o

$(BUILD)/modules/drv_input.elf: $(MOD_OBJS_drv_input) $(BUILD)/modules/drv_bus.nm
	@echo "  GEN   drv_input script+veneers"
	@$(RMMGEN) module drv_input build/modules/drv_bus.nm
	@$(CC) $(CFLAGS_MOD) $(INC) -c $(GEN)/drv_input_veneers.S -o $(OBJ)/drv_input/veneers.o
	@echo "  LD    module drv_input"
	@$(LD) -T $(GEN)/drv_input.ld -o $@ $(MOD_OBJS_drv_input) $(OBJ)/drv_input/veneers.o

$(BUILD)/modules/fs.elf: $(MOD_OBJS_fs) $(BUILD)/modules/drv_bus.nm \
	                 $(BUILD)/modules/drv_gfx.nm $(BUILD)/modules/drv_input.nm
	@echo "  GEN   fs script+veneers"
	@$(RMMGEN) module fs build/modules/drv_bus.nm build/modules/drv_gfx.nm \
	    build/modules/drv_input.nm
	@$(CC) $(CFLAGS_MOD) $(INC) -c $(GEN)/fs_veneers.S -o $(OBJ)/fs/veneers.o
	@echo "  LD    module fs"
	@$(LD) -T $(GEN)/fs.ld -o $@ $(MOD_OBJS_fs) $(OBJ)/fs/veneers.o

build/modules/%.nm: build/modules/%.elf
	@nm -g --defined-only $< > $@

# phase 4: final codegen (trampolines, abi, archive)
$(GEN)/trampolines.S $(GEN)/abi_table.S $(GEN)/archive.S $(BUILD)/modules/archive.bin: \
	$(BUILD)/modules/drv_bus.nm $(BUILD)/modules/drv_gfx.nm \
	$(BUILD)/modules/drv_input.nm $(BUILD)/modules/fs.nm $(RMMGEN)
	@echo "  GEN   final (trampolines, ABI, archive, validator)"
	@$(RMMGEN) final drv_bus=build/modules/drv_bus.elf \
	    drv_gfx=build/modules/drv_gfx.elf drv_input=build/modules/drv_input.elf \
	    fs=build/modules/fs.elf

$(OBJ)/gen/trampolines.o: $(GEN)/trampolines.S $(GEN)/offsets.txt
	@mkdir -p $(OBJ)/gen
	@echo "  CC    trampolines.S"
	@$(CC) $(CFLAGS_CORE) -c $< -o $@

$(OBJ)/gen/abi_table.o: $(GEN)/abi_table.S $(BUILD)/modules/archive.bin
	@mkdir -p $(OBJ)/gen
	@$(CC) $(CFLAGS_CORE) -c $< -o $@

$(OBJ)/gen/entry_table.o: $(GEN)/entry_table.S $(GEN)/rmm_stub.ld
	@mkdir -p $(OBJ)/gen
	@$(CC) $(CFLAGS_CORE) -c $< -o $@

$(OBJ)/gen/archive.o: $(GEN)/archive.S $(BUILD)/modules/archive.bin
	@echo "  CC    archive.S"
	@$(CC) $(CFLAGS_CORE) -c $< -o $@

# phase 5: core pass 2 (final kernel)
$(BUILD)/rmm.elf: $(CORE_OBJS) $(OBJ)/core/modules.generated.c.o \
	$(OBJ)/gen/trampolines.o $(OBJ)/gen/abi_table.o $(OBJ)/gen/archive.o \
	$(OBJ)/gen/entry_table.o $(GEN)/core.ld
	@echo "  LD    core pass 2 (final)"
	@$(LD) -z max-page-size=0x1000 -T $(GEN)/core.ld -o $@ $(CORE_OBJS) $(OBJ)/core/modules.generated.c.o \
	    $(OBJ)/gen/trampolines.o $(OBJ)/gen/abi_table.o $(OBJ)/gen/archive.o \
	    $(OBJ)/gen/entry_table.o
	@echo "  DONE  $(@) - modules demand-load from the embedded archive"

report:
	@cat $(GEN)/module_report.json 2>/dev/null || echo "no report yet"

clean:
	rm -rf $(BUILD)

# Mods for Snap64 Recomp, one folder each under mods/, built the way the mod
# template builds its example (Snap64RecompModTemplate): MIPS code with clang
# and lld into an ELF, which RecompModTool turns into the .nrm the port loads.
#
#     make MOD=unlimited_film
#     RecompModTool mods/unlimited_film/mod.toml build/unlimited_film
#
# The game's headers come from the decompilation checked out as pokemonsnap,
# the symbols from Snap64RecompSyms; both sit beside this file.

MOD ?= unlimited_film
BUILD_DIR := build/$(MOD)
SRC_DIR := mods/$(MOD)/src

ifeq ($(OS),Windows_NT)
    CC      := clang
    LD      := ld.lld
else ifneq ($(shell uname),Darwin)
    CC      := clang
    LD      := ld.lld
else
    CC      ?= clang
    LD      ?= ld.lld
endif

TARGET  := $(BUILD_DIR)/mod.elf

LDSCRIPT := mod.ld
ARCHFLAGS := -target mips -mips2 -mabi=32 -O2 -G0 -mno-abicalls -mno-odd-spreg -mno-check-zero-division \
             -fomit-frame-pointer -ffast-math -fno-unsafe-math-optimizations -fno-builtin-memset -fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-bcopy -funsigned-char -fno-builtin-sinf -fno-builtin-cosf
WARNFLAGS := -Wall -Wextra -Wno-incompatible-library-redeclaration -Wno-unused-parameter -Wno-unknown-pragmas -Wno-unused-variable \
             -Wno-missing-braces -Wno-unsupported-floating-point-opt -Werror=section -Wno-visibility
CFLAGS   := $(ARCHFLAGS) $(WARNFLAGS) -D_LANGUAGE_C -nostdinc -ffunction-sections
CPPFLAGS := -DMIPS -DF3DEX_GBI_2 -DNDEBUG -D_FINALROM -I include -I include/dummy_headers \
            -I pokemonsnap/include -I pokemonsnap/src -I pokemonsnap/ultralib/include -I pokemonsnap/ultralib/include/PR \
            -idirafter include/dummy_headers/stdlib
LDFLAGS  := -nostdlib -T $(LDSCRIPT) -Map $(BUILD_DIR)/mod.map --unresolved-symbols=ignore-all --emit-relocs -e 0 --no-nmagic -gc-sections

C_SRCS := $(wildcard $(SRC_DIR)/*.c)
C_OBJS := $(addprefix $(BUILD_DIR)/, $(notdir $(C_SRCS:.c=.o)))
C_DEPS := $(C_OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(C_OBJS) $(LDSCRIPT) | $(BUILD_DIR)
	$(LD) $(C_OBJS) $(LDFLAGS) -o $@

$(BUILD_DIR):
ifeq ($(OS),Windows_NT)
	if not exist "$(subst /,\,$@)" mkdir "$(subst /,\,$@)"
else
	mkdir -p $@
endif

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -MMD -MF $(@:.o=.d) -c -o $@

clean:
ifeq ($(OS),Windows_NT)
	if exist build rmdir /S /Q build
else
	rm -rf build
endif

-include $(C_DEPS)

.PHONY: clean all

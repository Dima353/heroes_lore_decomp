# Nintendo DS (devkitARM + libnds/calico + libfat + nds-zlib's minizip).
# Run through tools/build_nds.sh, which copies the sources to a path without spaces.
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif
# Before ds_rules: its %.elf rule prepends calico to LIBPATHS at parse time.
LIBPATHS := -L$(DEVKITPRO)/libnds/lib -L$(DEVKITPRO)/portlibs/nds/lib -L$(DEVKITPRO)/portlibs/armv5te/lib
include $(DEVKITARM)/ds_rules

OUTPUT         := heroes_lore
GAME_TITLE     := Heroes Lore
GAME_SUBTITLE1 := Wind of Soltia
GAME_SUBTITLE2 := Nintendo DS port
GAME_ICON      := assets/icon.bmp
BUILD          := build
CORE_DIR       := ../src

# Core sources shared with the SDL port; the DS front end lives in source/.
CORE   := map sprite character_sprite hero_movement equipment new_game language_pack event_runtime event_trigger event_visual npc_movement npc_sprite language eif bitmap_font combat combat_boss combat_guardian item_catalog save_game service shop_transaction audio
FRONT  := main_nds nds_sound
# soundbank.bin: instruments from GeneralUser GS (tools/build_nds_soundbank.py, run by build_nds.sh).
BANK   := data/soundbank.bin
OFILES := $(BUILD)/soundbank.bin.o $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE) $(FRONT)))

ARCH     := -march=armv5te -mtune=arm946e-s -mthumb
CXXFLAGS := -std=gnu++17 -Os -Wall -ffunction-sections -fdata-sections $(ARCH) -DARM9 -D__NDS__ \
            -DHEROES_LORE_NDS_BUILD=1 \
            -I$(CORE_DIR) -I$(BUILD) -I$(LIBNDS)/include -I$(CALICO)/include $(foreach dir,$(PORTLIBS),-I$(dir)/include)
LDFLAGS  := -specs=ds_arm9.specs $(ARCH) -Wl,--gc-sections -Wl,-Map,$(OUTPUT).map
LIBS     := -lminizip -lz -lfat -lnds9
export LD := $(CXX)

.PHONY: all clean jar
all: $(OUTPUT).nds

# make jar GAME_JAR=/path/to/Heroes_Lore.jar: the same program with the game JAR linked in (symbol
# game_jar, read in place by map.cpp). Such a file contains the game: for your own console only.
jar: $(OUTPUT)_jar.nds
$(OUTPUT)_jar.elf: OFILES += $(BUILD)/game_jar.o
$(OUTPUT)_jar.elf: LDFLAGS += -Wl,--undefined=game_jar -Wl,--undefined=game_jar_size # weak refs alone do not keep it
$(OUTPUT)_jar.elf: $(OFILES) $(BUILD)/game_jar.o
$(OUTPUT)_jar.nds: $(OUTPUT)_jar.elf
$(BUILD)/game_jar.o: $(GAME_JAR)
	@test -n "$(GAME_JAR)" || { echo "GAME_JAR is not set" >&2; exit 1; }
	@mkdir -p $(BUILD)
	bin2s -a 4 $< > $(BUILD)/game_jar.s
	$(CC) -x assembler-with-cpp $(ARCH) -c $(BUILD)/game_jar.s -o $@

$(OUTPUT).nds: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(BUILD)/soundbank.bin.o $(BUILD)/soundbank_bin.h: $(BANK)
	@mkdir -p $(BUILD)
	bin2s -a 4 -H $(BUILD)/soundbank_bin.h $< > $(BUILD)/soundbank.s
	$(CC) -x assembler-with-cpp $(ARCH) -c $(BUILD)/soundbank.s -o $(BUILD)/soundbank.bin.o
$(BUILD)/nds_sound.o: $(BUILD)/soundbank_bin.h

$(BUILD)/%.o: $(CORE_DIR)/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: source/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD) $(OUTPUT).elf $(OUTPUT).nds $(OUTPUT)_jar.elf $(OUTPUT)_jar.nds $(OUTPUT).map $(OUTPUT).lst

-include $(OFILES:.o=.d)

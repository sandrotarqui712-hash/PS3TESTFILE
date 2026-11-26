#---------------------------------------------------------------------------------
# Gestor de archivos PS3 - Makefile para PSL1GHT
# Uso:   make        (compila el .elf)
#        make pkg    (genera el .pkg instalable)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "Define PSL1GHT en tu entorno: export PSL1GHT=<ruta>")
endif

include $(PSL1GHT)/ppu_rules

TARGET		:=	$(notdir $(CURDIR))
BUILD		:=	build
SOURCE		:=	source
INCLUDE		:=	include
DATA		:=	data
LIBS		:=	-lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm

TITLE		:=	Gestor de archivos
APPID		:=	FILEMGR01
CONTENTID	:=	UP0001-$(APPID)_00-0000000000000000
SFOXML		:=	$(CURDIR)/sfo.xml
ICON0		:=	$(CURDIR)/ICON0.PNG

CFLAGS		=	-O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS	=	$(CFLAGS)
LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCE),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))
export BUILDDIR	:=	$(CURDIR)/$(BUILD)
export DEPSDIR	:=	$(BUILDDIR)

CFILES		:=	$(foreach dir,$(SOURCE),$(notdir $(wildcard $(dir)/*.c)))
CXXFILES	:=	$(foreach dir,$(SOURCE),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCE),$(notdir $(wildcard $(dir)/*.S)))

export OFILES	:=	$(CFILES:.c=.o) $(CXXFILES:.cpp=.o) $(SFILES:.S=.o)
export INCLUDES	:=	$(foreach dir,$(INCLUDE),-I$(CURDIR)/$(dir)) \
			-I$(CURDIR)/$(SOURCE) -I$(CURDIR)/$(BUILD)

.PHONY: $(BUILD) clean pkg

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo limpiando ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).pkg

pkg: $(BUILD)
	@echo Creando PKG ...
	@mkdir -p $(BUILDDIR)/pkg/USRDIR
	@cp $(ICON0) $(BUILDDIR)/pkg/ICON0.PNG
	@$(FSELF) $(BUILDDIR)/$(TARGET).elf $(BUILDDIR)/pkg/USRDIR/EBOOT.BIN
	@$(SFO) --title "$(TITLE)" --appid "$(APPID)" -f $(SFOXML) $(BUILDDIR)/pkg/PARAM.SFO
	@$(PKG) --contentid $(CONTENTID) $(BUILDDIR)/pkg/ $(OUTPUT).pkg

else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif

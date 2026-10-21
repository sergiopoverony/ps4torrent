# Сборка PKG-приложения "ps4torrent client" (экран и геймпад). Оно показывает раздачи службы ps4torrentd
# и само ставит/запускает службу (отправкой ELF в BinLoader GoldHEN и записью в автозапуск).
# Нужен OpenOrbis PS4 Toolchain и переменная OO_PS4_TOOLCHAIN. Для сборки службы (в корне репозитория) нужен
# ps4-payload-sdk и переменная PS4_PAYLOAD_SDK.
#
# Собирать одной командой, из этой папки:   make full
# Она сначала пересобирает службу, кладёт её в ../release/ps4torrentd.elf (именно этот ELF зашивается в PKG)
# и затем чисто собирает приложение. Простая "make" берёт ../release/ps4torrentd.elf как есть: если после правки
# службы забыть её пересобрать, получится PKG с новым интерфейсом, но со СТАРОЙ службой внутри (версии разойдутся).

# PkgTool.Core работает на .NET: без этих настроек он падает, если в системе нет библиотеки ICU,
# или ругается на версию .NET.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT := 1
export DOTNET_ROLL_FORWARD := LatestMajor

TITLE       := ps4torrent client
TITLE_ID    := BREW00088
CONTENT_ID  := IV0000-BREW00088_00-PS4TC00000000000
LIBS        := -lc -lkernel -lc++ -lSceNet -lSceVideoOut -lSceSysmodule -lSceFreeType -lScePad -lSceUserService -lSceSystemService
EXTRAFLAGS  := -O2

# Версия приложения = версия службы (из ../version.h): приложение сравнивает их при запуске.
APPVER      := $(shell sed -n 's/.*APP_VERSION "\(.*\)".*/\1/p' ../version.h)

# Всё, что кладётся в PKG (/app0/assets/...). ELF службы копируется сюда из ../release (см. ниже).
ASSETS      := assets/fonts/DejaVuSansMono.ttf assets/fonts/LICENSE-DejaVu.txt assets/images/title.png \
               assets/misc/lang_en.json assets/misc/lang_ru.json assets/misc/ps4torrentd.elf

# Системные модули для приложения (без libc.prx приложение сразу вылетает). Берутся из набора OpenOrbis.
LIBMODULES  := sce_module/libc.prx sce_module/libSceFios2.prx

TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
PROJDIR     := src
INTDIR      := build/obj

CFILES      := $(wildcard $(PROJDIR)/*.c)
CPPFILES    := $(wildcard $(PROJDIR)/*.cpp)
OBJS        := $(patsubst $(PROJDIR)/%.c, $(INTDIR)/%.o, $(CFILES)) $(patsubst $(PROJDIR)/%.cpp, $(INTDIR)/%.o, $(CPPFILES))

CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c $(EXTRAFLAGS) -DAPP_VERSION=\"$(APPVER)\" -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include
CXXFLAGS    := $(CFLAGS) -isystem $(TOOLCHAIN)/include/c++/v1
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

_unused     := $(shell mkdir -p $(INTDIR) sce_sys/about sce_module assets/misc)

UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
		CC      := clang
		CCX     := clang++
		LD      := ld.lld
		CDIR    := linux
endif
ifeq ($(UNAME_S),Darwin)
		CC      := /usr/local/opt/llvm/bin/clang
		CCX     := /usr/local/opt/llvm/bin/clang++
		LD      := /usr/local/opt/llvm/bin/ld.lld
		CDIR    := macos
endif
ifneq (,$(findstring MSYS_NT,$(UNAME_S)))
		CC      := clang
		CCX     := clang++
		LD      := ld.lld
		CDIR    := windows
endif

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core pkg_build $< .

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_sys/pic1.png $(LIBMODULES) $(ASSETS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

# Системные модули: ищем файл с нужным именем в наборе OpenOrbis (обычно он лежит в samples/*/sce_module).
sce_module/%.prx:
	@src="$$(find '$(TOOLCHAIN)' -name '$*.prx' 2>/dev/null | head -n 1)"; \
	if [ -z "$$src" ]; then echo "Не найден $*.prx в $(TOOLCHAIN). Скопируйте его вручную в pkg/sce_module/"; exit 1; fi; \
	echo "cp $$src $@"; cp "$$src" $@

# Служба берётся из ../release (её обновляет "make full").
assets/misc/ps4torrentd.elf: ../release/ps4torrentd.elf
	cp $< $@

# Иконка и фон запуска лежат в sce_src/ (PS4 ждёт имена icon0.png и pic1.png в sce_sys/).
sce_sys/icon0.png: sce_src/icon0.png
	cp $< $@

sce_sys/pic1.png: sce_src/pic1.png
	cp $< $@

sce_sys/about/right.sprx:
	cp $(TOOLCHAIN)/samples/piglet/sce_sys/about/right.sprx $@

sce_sys/param.sfo: Makefile
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_new $@
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '01.00'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '01.00'

eboot.bin: $(OBJS)
	$(LD) $(INTDIR)/*.o -o $(INTDIR)/ps4tc_client.elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-fself -in=$(INTDIR)/ps4tc_client.elf -out=$(INTDIR)/ps4tc_client.oelf --eboot "eboot.bin" --paid 0x3800000000000010

$(INTDIR)/%.o: $(PROJDIR)/%.c $(wildcard $(PROJDIR)/*.h) ../version.h
	$(CC) $(CFLAGS) -o $@ $<

$(INTDIR)/%.o: $(PROJDIR)/%.cpp $(wildcard $(PROJDIR)/*.h) ../version.h
	$(CCX) $(CXXFLAGS) -o $@ $<

clean:
	rm -rf $(CONTENT_ID).pkg pkg.gp4 eboot.bin build sce_sys sce_module assets/misc/ps4torrentd.elf

# Собирает службу в корне репозитория (нужен PS4_PAYLOAD_SDK), копирует её в ../release/ps4torrentd.elf
# и затем чисто пересобирает сам PKG. Используйте это вместо простого "make", когда меняли код или версию.
.PHONY: full
full:
	$(MAKE) -C ..
	mkdir -p ../release
	cp ../ps4torrentd.elf ../release/ps4torrentd.elf
	$(MAKE) clean
	$(MAKE) all

.PHONY: all clean

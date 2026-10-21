# ps4torrentd: сборка под ps4-payload-sdk
PS4_HOST ?= 192.168.0.24
PS4_PORT ?= 9090

# Путь к SDK по умолчанию; export нужен обязательно: скрипт orbis-ld читает PS4_PAYLOAD_SDK из окружения,
# и без экспорта линковка падает ("cannot find linker script /ldscripts/elf_x86_64.x").
PS4_PAYLOAD_SDK ?= /opt/ps4-payload-sdk
export PS4_PAYLOAD_SDK

ifdef PS4_PAYLOAD_SDK
    include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk
else
    $(error PS4_PAYLOAD_SDK is undefined)
endif

ELF ?= ps4torrentd.elf
SRCS := main.cpp assets.S writer.cpp incoming.cpp log.cpp net.cpp bencode.cpp sha1.cpp torrent.cpp tracker.cpp storage.cpp download.cpp resume.cpp swarm.cpp lang_gen.cpp lang_gen.S
CXXFLAGS := -std=c++17 -O2 -Wall

# Сборка с вшитым файлом (для проверки размера и, позже, для PKG):
#   make BLOB_FILE=blob.bin ELF=ps4torrentd-blob.elf
ifdef BLOB_FILE
SRCS += blob.S
CXXFLAGS += -DHAVE_BLOB -DBLOB_PATH=\"$(BLOB_FILE)\"
$(ELF): blob_info.h $(BLOB_FILE)
blob_info.h: $(BLOB_FILE)
	printf '#define BLOB_SHA1_HEX "%s"\n' "$$(sha1sum $(BLOB_FILE) | cut -d' ' -f1)" > $@
endif

all: $(ELF)

# Языковые файлы assets/lang/*.json вшиваются в программу (lang_gen.* создаёт mklang.sh)
lang_gen.S lang_gen.cpp: $(wildcard assets/lang/*.json) mklang.sh
	sh ./mklang.sh

$(ELF): $(SRCS) $(wildcard *.h) $(wildcard assets/*) $(wildcard assets/lang/*)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(ELF) lang_gen.S lang_gen.cpp

# отправка на консоль: make send PS4_HOST=IP
send: $(ELF)
	$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) $^

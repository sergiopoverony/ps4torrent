#!/bin/bash
# Сборка тестовых бинарников для запуска НА КОМПЬЮТЕРЕ (Linux, g++): те же исходники, что у консольной программы,
# но с подставными путями (-DUSB_BASE и др.), адресами /tmp/ft/... и санитайзерами памяти.
# Использование: cd <папка с исходниками, где main.cpp>; bash tests/build_tests.sh [имя ...]   (без имён: собрать все)
SRC="main.cpp assets.S writer.cpp incoming.cpp log.cpp net.cpp bencode.cpp sha1.cpp torrent.cpp tracker.cpp storage.cpp download.cpp resume.cpp swarm.cpp"
BASE="-std=c++17 -O1 -g -Wall -fsanitize=address,undefined -DHOST_TEST -DINTERNAL_ROOT=\\\"/tmp/ft/internal/torrents\\\" -DLISTEN_PORT=26881 -DUSB_BASE=\\\"/tmp/ft/usb\\\" -DEXT_BASE=\\\"/tmp/ft/ext\\\" -DHTTP_PORT=18787 -DLOG_DIR=\\\"/tmp/ft/log\\\" -DCONFIG_FILE=\\\"/tmp/ft/log/config.txt\\\""
declare -A V
V[daemon_host]="-DAUTOSTART_DEFAULT=1"                                   # api, delete, wr, part_test, все загрузки (автозапуск включён)
V[daemon_auto]=""                                                        # auto_run, auto2_run (автозапуск выключен, как на консоли)
V[daemon_sess]="-DRESUME_FIRST_DELAY_S=3 -DRESUME_STAGGER_S=5"           # session_run (сжатые задержки очереди возобновления)
V[daemon_store]="-DAUTOSTART_DEFAULT=1"                                  # storage_run
V[daemon_mnt]=""                                                         # mounts_run
V[daemon_done]="-DAUTOSTART_DEFAULT=1"                                 # done_run (завершённые в списке после перезапуска)
V[daemon_mem]="-DAUTOSTART_DEFAULT=1"                                  # mem_run (ASAN)
V[daemon_sel]=""                                                         # sel_run (выбор файлов)
V[daemon_diag]="-DMNT_DIR=\\\"/tmp/ft/mnt_mock\\\" -DDEV_DIR=\\\"/tmp/ft/dev_mock\\\""   # diag_run
V[daemon_fix]="-DAUTOSTART_DEFAULT=1 -DLOOP_GAP_S=4 -DWRITE_RETRY_S=60 -DNET_REFRESH_S=100000"      # fix_run
V[daemon_net2]="-DAUTOSTART_DEFAULT=1 -DLOOP_GAP_S=4 -DWRITE_RETRY_S=4 -DNET_REFRESH_S=8 -DNET_POLL_DOWN_S=2 -DNET_POLL_UP_S=3 -DNET_FALLBACK_S=12 -DTAKEOVER_WAIT_S=8"   # fd_run, heal_run, takeover_run
names="$@"; [ -z "$names" ] && names="${!V[@]}"
for n in $names; do
  echo "== $n"; eval g++ $BASE ${V[$n]} -o /tmp/ft/$n $SRC -lpthread || echo "BUILD FAILED: $n"
done

# Для mem_run нужны ещё сборки БЕЗ санитайзера (иначе mallinfo2 не показывает кучу): daemon_mem_plain (текущий код) и daemon_old_plain
# (предыдущая версия с такой же тестовой точкой heap_kb в /status). В основном коде эти тестовые точки (heap_kb, /tmp/ft/force_oom) есть
# только под -DHOST_TEST.

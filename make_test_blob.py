#!/usr/bin/env python3
# Создаёт blob.bin: 7 МБ случайных (но воспроизводимых) данных. Заглушка вместо будущего PKG.
# Использование: python3 make_test_blob.py [размер_в_МБ]   (по умолчанию 7)
import random, sys
mb = int(sys.argv[1]) if len(sys.argv) > 1 else 7
random.seed(20261004)
size = mb * 1024 * 1024
head = ("PS4TORRENT-EMBEDDED-BLOB-TEST %d MiB (random data, stand-in for the future PKG)\n" % mb).encode()
data = head + bytes(random.getrandbits(8) for _ in range(size - len(head)))
open("blob.bin", "wb").write(data)
print("blob.bin:", len(data), "bytes")

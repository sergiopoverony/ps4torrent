/* Вшитый в ELF файл (по умолчанию blob.bin). Сейчас нужен для проверки, сколько данных принимает загрузчик,
 * позже сюда попадёт PKG управляющего интерфейса. Символы: ps4_blob_start .. ps4_blob_end. */
    .section .rodata
    .balign 16
    .globl ps4_blob_start
ps4_blob_start:
    .incbin BLOB_PATH
    .globl ps4_blob_end
ps4_blob_end:
    .byte 0
#if defined(__linux__)
    .section .note.GNU-stack,"",@progbits
#endif

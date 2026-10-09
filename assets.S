/* Картинки, вшитые в ELF (папка assets/): значок сайта, логотип страницы и иконка уведомлений.
 * Для замены достаточно положить новый файл с тем же именем в assets/ и пересобрать (make). */
    .section .rodata
    .balign 16
    .globl ps4_favicon_start
    .globl ps4_favicon_end
ps4_favicon_start:
    .incbin "assets/favicon.ico"
ps4_favicon_end:
    .byte 0
    .balign 16
    .globl ps4_logo_start
    .globl ps4_logo_end
ps4_logo_start:
    .incbin "assets/title.png"
ps4_logo_end:
    .byte 0
    .balign 16
    .globl ps4_notify_start
    .globl ps4_notify_end
ps4_notify_start:
    .incbin "assets/notify64.png"
ps4_notify_end:
    .byte 0
#if defined(__linux__)
    .section .note.GNU-stack,"",@progbits
#endif

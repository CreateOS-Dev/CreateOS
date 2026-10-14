.section .rodata
.balign 16
.global createos_noto_sans_ttf_start
.global createos_noto_sans_ttf_end
.type createos_noto_sans_ttf_start, @object
createos_noto_sans_ttf_start:
.incbin "rootfs_maker/root/assets/fonts/NotoSans.ttf"
createos_noto_sans_ttf_end:
.size createos_noto_sans_ttf_start, createos_noto_sans_ttf_end-createos_noto_sans_ttf_start
.section .note.GNU-stack,"",@progbits

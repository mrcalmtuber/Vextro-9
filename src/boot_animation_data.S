.section .rodata, "a", @progbits
.global boot_anim_data
.global boot_anim_data_end
.align 16
boot_anim_data:
.incbin "build/boot_anim.raw"
boot_anim_data_end:

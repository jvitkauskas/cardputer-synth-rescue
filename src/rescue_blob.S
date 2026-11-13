/* The rescue image pieces (rescue/BOARD/, see tools/make_rescue_image.sh),
 * embedded as they are. Paths are relative to the project directory, where
 * PlatformIO runs the assembler. */
    .section .rodata.rescue, "a"
    .macro piece name, file
    .balign 4
    .global \name
\name:
    .incbin "\file"
    .global \name\()_end
\name\()_end:
    .endm

    piece rescue_fm1_boot, "rescue/fm1/boot.bin"
    piece rescue_smk37_boot, "rescue/smk37/boot.bin"

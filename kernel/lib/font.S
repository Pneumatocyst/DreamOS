# Embed the console font (Terminus 8x16, PSF2 format) straight into the kernel.
.section .rodata
.global font_psf
.global font_psf_end
font_psf:
    .incbin "assets/font.psf"
font_psf_end:

.section .note.GNU-stack,"",@progbits

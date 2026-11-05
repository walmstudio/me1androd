/* linux/assets.S - files embedded into me1-launcher and me1-linux (paths relative to the repository root, where make runs):
 * fonts (Exo 2: SIL OFL 1.1, linux/assets/OFL-Exo2.txt; DejaVu: linux/assets/LICENSE-DejaVu.txt), the loading picture,
 * the window icon and the interface translations (android/assets/i18n: shared with the Android launcher, linux/i18n:
 * strings of the Linux launcher). */
#define ASSET(name, file) \
    .global name; .global name##_size; .balign 16; name: .incbin file; .byte 0; .balign 4; name##_size: .int name##_size - name - 1
    .section .rodata
ASSET(me1a_exo_reg, "linux/assets/Exo2-Regular.ttf")
ASSET(me1a_exo_bold, "linux/assets/Exo2-SemiBold.ttf")
ASSET(me1a_mono, "linux/assets/DejaVuSansMono.ttf")
ASSET(me1a_sym, "linux/assets/DejaVuSans-sym.ttf")
ASSET(me1a_loading, "android/assets/loading.jpg")
ASSET(me1a_icon, "linux/assets/icon128.png")
ASSET(me1a_tr_en, "android/assets/i18n/en.json")
ASSET(me1a_tr_de, "android/assets/i18n/de.json")
ASSET(me1a_tr_fr, "android/assets/i18n/fr.json")
ASSET(me1a_tr_it, "android/assets/i18n/it.json")
ASSET(me1a_tr_es, "android/assets/i18n/es.json")
ASSET(me1a_tr_pl, "android/assets/i18n/pl.json")
ASSET(me1a_tr_cs, "android/assets/i18n/cs.json")
ASSET(me1a_tr_hu, "android/assets/i18n/hu.json")
ASSET(me1a_lx_en, "linux/i18n/en.json")
ASSET(me1a_lx_de, "linux/i18n/de.json")
ASSET(me1a_lx_fr, "linux/i18n/fr.json")
ASSET(me1a_lx_it, "linux/i18n/it.json")
ASSET(me1a_lx_es, "linux/i18n/es.json")
ASSET(me1a_lx_pl, "linux/i18n/pl.json")
ASSET(me1a_lx_cs, "linux/i18n/cs.json")
ASSET(me1a_lx_hu, "linux/i18n/hu.json")
    .section .note.GNU-stack,"",%progbits

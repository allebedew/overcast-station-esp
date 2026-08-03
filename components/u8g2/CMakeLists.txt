# Vendored subset of u8g2 (https://github.com/olikraus/u8g2, new-bsd) -- only the
# font decoder. The display, buffer and setup halves are not here: gfx_text.c
# drives the decoder through its own u8g2_cb_t and renders into gfx_canvas_t.
# Fonts are not here either; tools/extract_fonts.py slices the wanted ones into
# main/ui/gfx/gfx_fonts.c, because upstream's u8g2_fonts.c is 40 MB.
idf_component_register(SRCS "csrc/u8g2_font.c"
                            "csrc/u8g2_hvline.c"
                            "csrc/u8g2_intersection.c"
                            "csrc/u8g2_kerning.c"
                            "csrc/u8x8_8x8.c"
                            "csrc/u8x8_string.c"
                            "csrc/u8x8_u8toa.c"
                            "csrc/u8x8_u16toa.c"
                       INCLUDE_DIRS "csrc")

# Host build of the drawing framework -- same sources as the firmware, so what
# the PNGs show is what the panel will show.
#
#   make run      render every scene into out/
#   make clean

GFX  := ../gfx
UI   := ..
MAIN := ../..
U8G2 := ../../../components/u8g2/csrc

# ui_model.c is firmware-only -- it reads the sensor modules, which need the
# IDF. The host fills a ui_model_t by hand instead; ui_model.h itself is plain C
# and includes cleanly here.
SRCS := sim_main.c sim_png.c \
        $(UI)/ui.c $(UI)/screens/screen_now.c \
        $(GFX)/gfx_canvas.c $(GFX)/gfx_text.c $(GFX)/gfx_fonts.c \
        $(U8G2)/u8g2_font.c $(U8G2)/u8g2_hvline.c $(U8G2)/u8g2_intersection.c \
        $(U8G2)/u8g2_kerning.c $(U8G2)/u8x8_8x8.c $(U8G2)/u8x8_string.c \
        $(U8G2)/u8x8_u8toa.c $(U8G2)/u8x8_u16toa.c

# u8x8_8x8.c is here only for the UTF-8 decoder; the rest of it refers to the
# display half of u8x8 that we deliberately do not vendor, so that dead code has
# to be stripped. The IDF build does the same via its default gc-sections.
CFLAGS := -std=c11 -O1 -g -Wall -Wextra -ffunction-sections -fdata-sections \
          -I$(GFX) -I$(UI) -I$(MAIN) -I$(MAIN)/sensors -I$(U8G2)
LDLIBS := -lz -lm
LDFLAGS := -Wl,-dead_strip

sim: $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LDLIBS)

# The scale defaults to 4; ./sim 1 renders at the panel's real size.
run: sim
	@mkdir -p out
	./sim

clean:
	rm -rf sim sim.dSYM out

.PHONY: run clean

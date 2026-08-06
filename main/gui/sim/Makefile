# Host build of the drawing framework -- same sources as the firmware, so what
# the PNGs show is what the panel will show.
#
#   make run      render every scene into out/
#   make clean

GFX  := ../gfx
UI   := ../views
MAIN := ../..
U8G2 := ../../../components/u8g2/csrc

# ui_model.c is firmware-only -- it reads the sensor modules, which need the
# IDF. The host fills a ui_model_t by hand instead; ui_model.h itself is plain C
# and includes cleanly here.
# Basenames only, found through VPATH -- that keeps the object tree flat inside
# out/obj instead of mirroring the '..' in the source paths.
VPATH := $(UI) $(GFX) $(U8G2)

SRCS := sim_main.c sim_png.c \
        ui.c ui_state.c chart.c screen_now.c \
        gfx_canvas.c gfx_text.c gfx_fonts.c \
        u8g2_font.c u8g2_hvline.c u8g2_intersection.c \
        u8g2_kerning.c u8x8_8x8.c u8x8_string.c \
        u8x8_u8toa.c u8x8_u16toa.c

# u8x8_8x8.c is here only for the UTF-8 decoder; the rest of it refers to the
# display half of u8x8 that we deliberately do not vendor, so that dead code has
# to be stripped. The IDF build does the same via its default gc-sections.
# -MMD -MP emits the header dependencies next to each object, so editing ui.h
# (UI_GAP and friends) rebuilds what uses it instead of silently re-running a
# stale binary.
CFLAGS := -std=c11 -O1 -g -Wall -Wextra -ffunction-sections -fdata-sections \
          -MMD -MP -I$(GFX) -I$(UI) -I$(MAIN) -I$(MAIN)/sensors -I$(U8G2)
LDLIBS := -lz -lm
LDFLAGS := -Wl,-dead_strip

OBJDIR := out/obj
OBJS   := $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))

sim: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): | $(OBJDIR)

$(OBJDIR):
	@mkdir -p $@

$(OBJDIR)/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(OBJS:.o=.d)

# The scale defaults to 4; ./sim 1 renders at the panel's real size.
run: sim
	@mkdir -p out
	./sim

clean:
	rm -rf sim sim.dSYM out

.PHONY: run clean

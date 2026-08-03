// Host-only: canvas -> PNG, scaled up and tinted like the green panel.

#include "sim_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[4];
    be32(hdr, len);
    fwrite(hdr, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) {
        fwrite(data, 1, len, f);
    }

    uLong c = crc32(0, (const Bytef *)type, 4);
    if (len) {
        c = crc32(c, data, len);
    }
    uint8_t crc[4];
    be32(crc, (uint32_t)c);
    fwrite(crc, 1, 4, f);
}

// Roughly what the green phosphor does: a gamma curve, not a linear ramp.
static void tint(uint8_t level, uint8_t rgb[3])
{
    double v = level == 0 ? 0.0 : pow((double)level / 15.0, 0.75);
    rgb[0] = (uint8_t)(7 + v * 96);
    rgb[1] = (uint8_t)(10 + v * 235);
    rgb[2] = (uint8_t)(8 + v * 130);
}

bool sim_write_png(const char *path, const gfx_canvas_t *c, int scale)
{
    if (scale < 1) {
        scale = 1;
    }
    const int w = GFX_W * scale, h = GFX_H * scale;

    // One filter byte plus RGB per row.
    const size_t stride = 1 + (size_t)w * 3;
    uint8_t *raw = malloc(stride * (size_t)h);
    if (!raw) {
        return false;
    }

    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + stride * (size_t)y;
        *row++ = 0;
        for (int x = 0; x < w; x++) {
            int px = x / scale, py = y / scale;
            uint8_t byte = c->buf[px][py >> 1];
            uint8_t level = (py & 1) ? (byte & 0x0F) : (byte >> 4);
            tint(level, row);
            row += 3;
        }
    }

    uLongf zlen = compressBound((uLong)(stride * (size_t)h));
    uint8_t *z = malloc(zlen);
    if (!z || compress2(z, &zlen, raw, (uLong)(stride * (size_t)h), 6) != Z_OK) {
        free(raw); free(z);
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(raw); free(z);
        return false;
    }

    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;      // bit depth
    ihdr[9] = 2;      // truecolour
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, sizeof(ihdr));
    chunk(f, "IDAT", z, (uint32_t)zlen);
    chunk(f, "IEND", NULL, 0);

    fclose(f);
    free(raw);
    free(z);
    return true;
}

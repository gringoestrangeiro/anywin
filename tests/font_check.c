/* Native font harness (no X server needed).  Included white-box so the built-in bitmap path can
 * be exercised directly.  Usage: font_check <out.pgm>.  Prints METRICS/PATH/COVERAGE/BUILTIN lines
 * and RESULT PASS|FAIL; the shell test cross-checks the metrics against the font files. */
#include "../runtime/font.c"

int aw_debug;
void aw_log(const char *fmt, ...) { (void)fmt; }

static int failures;
#define CHECK(c, msg)                                                                              \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("CHECK FAILED: %s\n", msg);                                                     \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static int contains(const char *s, const char *sub) { return s && strstr(s, sub) != NULL; }

static void print_metrics(const char *tag, aw_font *f) {
    const aw_font_metrics *m = font_metrics(f);
    printf("METRICS %s asc=%d desc=%d height=%d internal=%d ave=%d max=%d fixed=%d weight=%d italic=%d\n", tag,
           m->ascent, m->descent, m->height, m->internal_leading, m->ave_char_width, m->max_char_width,
           m->fixed_pitch, m->weight, m->italic);
}

static void print_path(const char *tag, aw_font *f) {
    printf("PATH %s %s synth_bold=%d synth_italic=%d\n", tag, f->f ? f->f->path : "(builtin)", f->synth_bold,
           f->synth_italic);
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "font.pgm";

    aw_font *dv12 = font_open("DejaVu Sans", -12, 0, 400, 0, 0, 0);
    print_metrics("DejaVuSans-12", dv12);
    print_path("DejaVuSans-12", dv12);
    const aw_font_metrics *m = font_metrics(dv12);
    CHECK(m->ascent == 12 && m->descent == 3, "DejaVu Sans at -12 must give ascent 12, descent 3");
    CHECK(m->height == m->ascent + m->descent, "height = ascent + descent");
    CHECK(contains(dv12->f ? dv12->f->path : "", "DejaVuSans.ttf"), "DejaVu Sans resolves to DejaVuSans.ttf");

    aw_font *noto12 = font_open("Noto Sans", -12, 0, 400, 0, 0, 0);
    print_metrics("NotoSans-12", noto12);
    print_path("NotoSans-12", noto12);

    aw_font *bold = font_open("DejaVu Sans", -12, 0, 700, 0, 0, 0);
    print_path("DejaVuSans-bold", bold);
    CHECK(contains(bold->f ? bold->f->path : "", "DejaVuSans-Bold.ttf") && !bold->synth_bold,
          "weight 700 selects DejaVuSans-Bold.ttf without synthesis");

    aw_font *ital = font_open("DejaVu Sans", -12, 0, 400, 1, 0, 0);
    print_path("DejaVuSans-italic", ital);
    CHECK(contains(ital->f ? ital->f->path : "", "DejaVuSans-Oblique.ttf"), "italic selects the Oblique file");

    aw_font *mono = font_open("Courier New", -12, 0, 400, 0, 0, 0);
    print_metrics("Courier-12", mono);
    print_path("Courier-12", mono);
    CHECK(mono->m.fixed_pitch == 1 && contains(mono->f ? mono->f->path : "", "Mono"),
          "Courier New maps to a fixed-pitch mono face");

    aw_font *serif = font_open("Times New Roman", -12, 0, 400, 0, 0, 0);
    print_path("TimesNewRoman-12", serif);
    CHECK(contains(serif->f ? serif->f->path : "", "LiberationSerif-Regular.ttf"), "Times New Roman maps to Liberation Serif");

    aw_font *unknown = font_open("Wingdings", -12, 0, 400, 0, 0, 0);
    print_path("Wingdings-12", unknown);
    CHECK(unknown->f != NULL, "unknown face falls back to a sans face");

    aw_font *def = font_open("", 0, 0, 0, 0, 0, 0);
    print_metrics("default-cell16", def);
    CHECK(font_open("", 0, 0, 0, 0, 0, 0) == def, "identical parameters return the cached font");

    /* render one line at -16 and check coverage */
    aw_font *dv16 = font_open("DejaVu Sans", -16, 0, 400, 0, 0, 0);
    const char *txt = "Hello, World! 0123 Oo";
    int width = 8, n = (int)strlen(txt);
    for (int i = 0; i < n; i++) width += font_advance(dv16, (unsigned char)txt[i]);
    int height = font_metrics(dv16)->height + 8;
    int base = font_metrics(dv16)->ascent + 4;
    uint8_t *canvas = calloc((size_t)width * (size_t)height, 1);
    int pen = 4, nonzero = 0, strong = 0;
    long sum = 0;
    for (int i = 0; i < n; i++) {
        const aw_glyph *g = font_glyph(dv16, (unsigned char)txt[i]);
        for (int j = 0; j < g->h; j++)
            for (int k = 0; k < g->w; k++) {
                int x = pen + g->left + k, y = base - g->top + j;
                if (x < 0 || y < 0 || x >= width || y >= height) continue;
                uint8_t a = g->alpha[j * g->w + k];
                if (a > canvas[y * width + x]) canvas[y * width + x] = a;
            }
        pen += g->advance;
    }
    for (int i = 0; i < width * height; i++) {
        sum += canvas[i];
        if (canvas[i]) nonzero++;
        if (canvas[i] >= 128) strong++;
    }
    printf("COVERAGE nonzero=%d strong=%d sum=%ld width=%d height=%d\n", nonzero, strong, sum, width, height);
    CHECK(nonzero > 400 && strong > 150, "rendered text must have real AA coverage");

    /* the 'O' glyph has a counter: centre empty, ring covered */
    const aw_glyph *O = font_glyph(dv16, 'O');
    int cx = O->w / 2, cy = O->h / 2, ring = 0;
    for (int j = 0; j < O->h; j++)
        for (int k = 0; k < O->w; k++)
            if ((k == 0 || k == O->w - 1) && O->alpha[j * O->w + k] > ring) ring = O->alpha[j * O->w + k];
    printf("GLYPH O w=%d h=%d left=%d top=%d adv=%d centre=%d ring=%d\n", O->w, O->h, O->left, O->top, O->advance,
           O->alpha[cy * O->w + cx], ring);
    CHECK(O->alpha[cy * O->w + cx] < 64 && ring > 128, "'O' must have a counter");

    /* composite glyph (e + acute in DejaVu): components must be combined */
    const aw_glyph *eacute = font_glyph(dv16, 0xe9);
    int ecov = 0;
    for (int i = 0; i < eacute->w * eacute->h; i++) ecov += eacute->alpha[i] > 0;
    printf("COMPOSITE eacute w=%d h=%d cov=%d\n", eacute->w, eacute->h, ecov);
    CHECK(eacute->w > 0 && ecov > 30, "composite glyph e-acute must render");

    /* space has an advance and no ink */
    CHECK(font_advance(dv16, ' ') > 0 && font_glyph(dv16, ' ')->w == 0, "space: advance only");

    /* built-in 5x7 fallback (used when no font file exists) */
    aw_font b;
    memset(&b, 0, sizeof b);
    b.bi_k = 2;
    b.ppem = 16;
    b.m.ascent = 14;
    b.m.descent = 2;
    b.m.height = 16;
    aw_glyph bg;
    builtin_glyph(&b, 'H', &bg);
    int bcov = 0;
    for (int i = 0; i < bg.w * bg.h; i++) bcov += bg.alpha[i] > 0;
    printf("BUILTIN H w=%d h=%d top=%d adv=%d cov=%d\n", bg.w, bg.h, bg.top, bg.advance, bcov);
    CHECK(bg.w == 10 && bg.h == 14 && bcov > 20, "built-in glyph has the scaled 5x7 shape");
    builtin_glyph(&b, 0x20ac, &bg); /* not ASCII: box */
    CHECK(bg.w > 0 && bg.h > 0, "built-in: non-ASCII codepoints draw a box");

    /* PGM dump of the line */
    FILE *fp = fopen(out, "wb");
    if (fp) {
        fprintf(fp, "P5\n%d %d\n255\n", width, height);
        fwrite(canvas, 1, (size_t)width * (size_t)height, fp);
        fclose(fp);
        printf("PGM %s\n", out);
    }
    free(canvas);

    printf("RESULT %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

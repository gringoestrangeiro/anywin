/*
 * anywin - static Windows (PE32+ x86-64) to Linux (ELF x86-64) relinker.
 *
 * The PE image is laid out at its preferred image base inside the output
 * ELF, every import address table slot is turned into a link-time
 * relocation against a symbol of the form  __aw_<module>_<function>
 * which the anywin runtime (libanywin.o) provides, and the result is
 * linked statically with the system linker.  Imports the runtime does not
 * implement are bound to generated weak stubs that report the missing
 * function when (and only if) they are called.
 *
 * Nothing of the PE loader survives at runtime: no DLL loading, no import
 * resolution, no relocation processing.  The output is an ordinary static
 * ELF executable.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../runtime/modmap.h"

static int verbose;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "anywin: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

typedef struct {
    char name[9];
    uint32_t vaddr, vsize, rawoff, rawsize, chars;
} Section;

typedef struct {
    uint32_t slot_rva; /* RVA of the IAT slot */
    char *module;      /* normalized module namespace */
    char *dll;         /* original DLL name */
    char *func;        /* function name or "#ordinal" */
    char *sym;         /* link symbol */
} Import;

static uint8_t *file;
static size_t file_size;
static uint8_t *image;
static uint32_t image_size;
static uint64_t image_base;
static uint32_t entry_rva;
static uint16_t subsystem;
static Section *sects;
static int nsects;
static uint32_t size_of_headers;
static Import *imps;
static int nimps, cimps;

static uint8_t *rva_ptr(uint32_t rva, uint32_t len)
{
    if ((uint64_t)rva + len > image_size)
        die("RVA 0x%x out of range", rva);
    return image + rva;
}

static const char *rva_str(uint32_t rva)
{
    uint8_t *p = rva_ptr(rva, 1);
    size_t max = image_size - rva;
    if (!memchr(p, 0, max))
        die("unterminated string at RVA 0x%x", rva);
    return (const char *)p;
}

static void add_import(uint32_t slot, const char *dll, const char *func)
{
    if (nimps == cimps) {
        cimps = cimps ? cimps * 2 : 64;
        imps = realloc(imps, cimps * sizeof *imps);
    }
    Import *im = &imps[nimps++];
    im->slot_rva = slot;
    im->dll = strdup(dll);
    im->module = strdup(aw_normalize_module(dll));
    im->func = strdup(func);
    if (asprintf(&im->sym, "__aw_%s_%s", im->module, func) < 0)
        die("oom");
}

static void parse_thunks(const char *dll, uint32_t lookup_rva, uint32_t iat_rva)
{
    for (uint32_t i = 0;; i++) {
        uint64_t v = rd64(rva_ptr(lookup_rva + i * 8, 8));
        if (!v)
            break;
        char fn[512];
        if (v >> 63) {
            int ord = (int)(v & 0xffff);
            const char *n = aw_ordinal_name(aw_normalize_module(dll), ord);
            if (n)
                snprintf(fn, sizeof fn, "%s", n);
            else
                snprintf(fn, sizeof fn, "#%d", ord);
        } else {
            snprintf(fn, sizeof fn, "%s", rva_str((uint32_t)(v & 0x7fffffff) + 2));
        }
        add_import(iat_rva + i * 8, dll, fn);
    }
}

static void load_pe(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open %s: %s", path, strerror(errno));
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc(file_size + 16);
    if (fread(file, 1, file_size, f) != file_size)
        die("read error");
    fclose(f);

    if (file_size < 0x40 || rd16(file) != 0x5a4d)
        die("%s: not an MZ executable", path);
    uint32_t pe = rd32(file + 0x3c);
    if (pe + 24 > file_size || rd32(file + pe) != 0x4550)
        die("%s: not a PE executable", path);
    const uint8_t *coff = file + pe + 4;
    uint16_t machine = rd16(coff);
    if (machine != 0x8664)
        die("%s: unsupported machine 0x%x (only x86-64 is supported)", path, machine);
    nsects = rd16(coff + 2);
    uint16_t optsize = rd16(coff + 16);
    uint16_t chars = rd16(coff + 18);
    if (chars & 0x2000)
        die("%s: is a DLL, not an executable", path);
    const uint8_t *opt = coff + 20;
    if (rd16(opt) != 0x20b)
        die("%s: not PE32+", path);
    entry_rva = rd32(opt + 16);
    image_base = rd64(opt + 24);
    uint32_t salign = rd32(opt + 32);
    image_size = rd32(opt + 56);
    size_of_headers = rd32(opt + 60);
    subsystem = rd16(opt + 68);
    uint32_t ndirs = rd32(opt + 108);
    const uint8_t *dirs = opt + 112;
    if (salign < 0x1000)
        die("%s: section alignment 0x%x below page size is not supported", path, salign);

    image = calloc(1, image_size);
    memcpy(image, file, size_of_headers < file_size ? size_of_headers : file_size);
    sects = calloc(nsects, sizeof *sects);
    const uint8_t *sh = opt + optsize;
    for (int i = 0; i < nsects; i++, sh += 40) {
        Section *s = &sects[i];
        memcpy(s->name, sh, 8);
        s->vsize = rd32(sh + 8);
        s->vaddr = rd32(sh + 12);
        s->rawsize = rd32(sh + 16);
        s->rawoff = rd32(sh + 20);
        s->chars = rd32(sh + 36);
        if (!s->vsize)
            s->vsize = s->rawsize;
        uint32_t n = s->rawsize < s->vsize ? s->rawsize : s->vsize;
        if (n && (uint64_t)s->rawoff + n > file_size)
            n = s->rawoff < file_size ? file_size - s->rawoff : 0;
        if ((uint64_t)s->vaddr + s->vsize > image_size)
            die("section %s exceeds image", s->name);
        if (n)
            memcpy(image + s->vaddr, file + s->rawoff, n);
    }

    /* imports */
    if (ndirs > 1 && rd32(dirs + 8)) {
        uint32_t rva = rd32(dirs + 8);
        for (;; rva += 20) {
            const uint8_t *d = rva_ptr(rva, 20);
            uint32_t oft = rd32(d), name = rd32(d + 12), ft = rd32(d + 16);
            if (!name && !ft)
                break;
            parse_thunks(rva_str(name), oft ? oft : ft, ft);
        }
    }
    /* delay-load imports: bind them statically too */
    if (ndirs > 13 && rd32(dirs + 13 * 8)) {
        uint32_t rva = rd32(dirs + 13 * 8);
        for (;; rva += 32) {
            const uint8_t *d = rva_ptr(rva, 32);
            uint32_t attr = rd32(d), name = rd32(d + 4), iat = rd32(d + 12), int_ = rd32(d + 16);
            if (!name)
                break;
            if (!(attr & 1))
                die("old-style (VA based) delay import descriptors are not supported");
            parse_thunks(rva_str(name), int_, iat);
        }
    }
}

static int import_at(uint32_t rva)
{
    /* imports are sorted by slot */
    int lo = 0, hi = nimps - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (imps[mid].slot_rva == rva)
            return mid;
        if (imps[mid].slot_rva < rva)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static int cmp_imp(const void *a, const void *b)
{
    const Import *x = a, *y = b;
    return x->slot_rva < y->slot_rva ? -1 : x->slot_rva > y->slot_rva;
}

static void emit_quoted_sym(FILE *o, const char *s)
{
    fputc('"', o);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fputc('\\', o);
        fputc(*s, o);
    }
    fputc('"', o);
}

/* Emit image bytes [start,end) with IAT slots replaced by symbol refs */
static void emit_range(FILE *o, const char *bin, uint32_t start, uint32_t end)
{
    uint32_t p = start;
    while (p < end) {
        /* next import slot inside the range */
        uint32_t next = end;
        for (int i = 0; i < nimps; i++)
            if (imps[i].slot_rva >= p && imps[i].slot_rva < next) {
                next = imps[i].slot_rva;
                break;
            }
        if (next > p) {
            /* skip trailing zeros as .zero to keep output compact */
            uint32_t q = next;
            while (q > p && image[q - 1] == 0)
                q--;
            if (q > p)
                fprintf(o, "\t.incbin \"%s\", %u, %u\n", bin, p, q - p);
            if (next > q)
                fprintf(o, "\t.zero %u\n", next - q);
            p = next;
        }
        if (p < end) {
            int i = import_at(p);
            fprintf(o, "\t.quad ");
            emit_quoted_sym(o, imps[i].sym);
            fprintf(o, "\n");
            p += 8;
        }
    }
}

static int run(char *const argv[])
{
    if (verbose) {
        for (int i = 0; argv[i]; i++)
            fprintf(stderr, "%s ", argv[i]);
        fprintf(stderr, "\n");
    }
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static char *self_dir(void)
{
    static char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n < 0)
        die("readlink /proc/self/exe failed");
    buf[n] = 0;
    char *s = strrchr(buf, '/');
    if (s)
        *s = 0;
    return buf;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: anywin [options] input.exe [-o output]\n"
            "  -o FILE       output ELF path (default: input name without .exe)\n"
            "  -r FILE       runtime object (default: libanywin.o next to anywin)\n"
            "  --keep        keep intermediate files\n"
            "  --list        list imports and whether the runtime implements them\n"
            "  -v            verbose\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL, *rt = NULL, *cc = getenv("CC");
    int keep = 0, list = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc)
            rt = argv[++i];
        else if (!strcmp(argv[i], "--keep"))
            keep = 1;
        else if (!strcmp(argv[i], "--list"))
            list = 1;
        else if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (argv[i][0] == '-')
            usage();
        else if (!in)
            in = argv[i];
        else
            usage();
    }
    if (!in)
        usage();
    if (!cc)
        cc = "cc";
    char outbuf[PATH_MAX];
    if (!out) {
        snprintf(outbuf, sizeof outbuf, "%s", in);
        size_t l = strlen(outbuf);
        if (l > 4 && !strcasecmp(outbuf + l - 4, ".exe"))
            outbuf[l - 4] = 0;
        else
            strcat(outbuf, ".elf");
        out = outbuf;
    }
    char rtbuf[PATH_MAX];
    if (!rt) {
        snprintf(rtbuf, sizeof rtbuf, "%s/libanywin.o", self_dir());
        rt = rtbuf;
    }
    if (access(rt, R_OK))
        die("runtime object %s not found", rt);

    load_pe(in);
    qsort(imps, nimps, sizeof *imps, cmp_imp);

    /* Which symbols does the runtime provide?  (only for reporting) */
    char *provided = NULL;
    size_t provided_len = 0;
    {
        char cmd[PATH_MAX + 64];
        snprintf(cmd, sizeof cmd, "nm -g --defined-only '%s' 2>/dev/null", rt);
        FILE *p = popen(cmd, "r");
        if (p) {
            FILE *m = open_memstream(&provided, &provided_len);
            char line[1024];
            while (fgets(line, sizeof line, p)) {
                char *s = strrchr(line, ' ');
                if (s && !strncmp(s + 1, "__aw_", 5))
                    fputs(s + 1, m);
            }
            fputc(0, m);
            fclose(m);
            pclose(p);
        }
    }
    int missing = 0;
    for (int i = 0; i < nimps; i++) {
        char key[1100];
        snprintf(key, sizeof key, "%s\n", imps[i].sym);
        int have = provided && (strstr(provided, key) == provided ||
                                (strstr(provided, key) && strstr(provided, key)[-1] == '\n'));
        /* exact line match */
        if (provided) {
            have = 0;
            for (char *s = provided; (s = strstr(s, key)); s++)
                if (s == provided || s[-1] == '\n') {
                    have = 1;
                    break;
                }
        }
        if (list)
            printf("%-4s %-28s %s\n", have ? "ok" : "MISS", imps[i].dll, imps[i].func);
        if (!have) {
            missing++;
            if (!list)
                fprintf(stderr, "anywin: warning: %s!%s not implemented (stubbed)\n", imps[i].dll, imps[i].func);
        }
    }
    if (list)
        return 0;

    /* work directory */
    char tmpl[] = "/tmp/anywin.XXXXXX";
    char *wd = mkdtemp(tmpl);
    if (!wd)
        die("mkdtemp failed");
    char bin[PATH_MAX], asmf[PATH_MAX], obj[PATH_MAX];
    snprintf(bin, sizeof bin, "%s/image.bin", wd);
    snprintf(asmf, sizeof asmf, "%s/image.S", wd);
    snprintf(obj, sizeof obj, "%s/image.o", wd);
    FILE *f = fopen(bin, "wb");
    fwrite(image, 1, image_size, f);
    fclose(f);

    FILE *o = fopen(asmf, "w");
    fprintf(o, "/* generated by anywin from %s */\n", in);
    fprintf(o, "\t.globl __aw_image_base\n\t.set __aw_image_base, 0x%llx\n", (unsigned long long)image_base);
    fprintf(o, "\t.globl __aw_image_size\n\t.set __aw_image_size, 0x%x\n", image_size);

    /* headers */
    uint32_t hdr_end = (size_of_headers + 0xfff) & ~0xfffu;
    uint32_t first_sect = image_size;
    for (int i = 0; i < nsects; i++)
        if (sects[i].vaddr < first_sect)
            first_sect = sects[i].vaddr;
    if (hdr_end > first_sect)
        hdr_end = first_sect;
    fprintf(o, "\t.section .aw_hdr,\"a\",@progbits\n");
    emit_range(o, bin, 0, hdr_end);
    if (first_sect > hdr_end)
        fprintf(o, "\t.zero %u\n", first_sect - hdr_end);

    /* section start options for the linker */
    char **sectopts = calloc(nsects + 2, sizeof(char *));
    int nsectopts = 0;
    asprintf(&sectopts[nsectopts++], "-Wl,--section-start=.aw_hdr=0x%llx", (unsigned long long)image_base);

    for (int i = 0; i < nsects; i++) {
        Section *s = &sects[i];
        uint32_t start = s->vaddr;
        uint32_t end = s->vaddr + ((s->vsize + 0xfff) & ~0xfffu);
        if (i + 1 < nsects && end > sects[i + 1].vaddr)
            end = sects[i + 1].vaddr;
        if (end > image_size)
            end = image_size;
        int x = (s->chars & 0x20000000) != 0, w = (s->chars & 0x80000000) != 0;
        int has_data = 0;
        for (uint32_t k = start; k < end && !has_data; k++)
            has_data = image[k] != 0;
        for (int k = 0; k < nimps && !has_data; k++)
            has_data = imps[k].slot_rva >= start && imps[k].slot_rva < end;
        const char *flags = x ? (w ? "awx" : "ax") : (w ? "aw" : "a");
        fprintf(o, "\t.section .aw_s%d,\"%s\",@%s\n", i, flags, has_data ? "progbits" : "nobits");
        if (has_data)
            emit_range(o, bin, start, end);
        else
            fprintf(o, "\t.zero %u\n", end - start);
        asprintf(&sectopts[nsectopts++], "-Wl,--section-start=.aw_s%d=0x%llx", i,
                 (unsigned long long)(image_base + start));
    }

    /* image description for the runtime */
    fprintf(o, "\t.section .rodata\n\t.globl __aw_image_entry\n__aw_image_entry:\n\t.quad 0x%llx\n",
            (unsigned long long)(image_base + entry_rva));
    fprintf(o, "\t.globl __aw_image_subsystem\n__aw_image_subsystem:\n\t.long %d\n", subsystem);

    /* weak fallbacks for imports */
    fprintf(o, "\t.text\n");
    char *done = calloc(nimps, 1);
    for (int i = 0; i < nimps; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (!strcmp(imps[j].sym, imps[i].sym)) {
                dup = 1;
                break;
            }
        if (dup)
            continue;
        fprintf(o, "\t.weak ");
        emit_quoted_sym(o, imps[i].sym);
        fprintf(o, "\n");
        emit_quoted_sym(o, imps[i].sym);
        fprintf(o, ":\n\tlea .Lname%d(%%rip), %%rax\n\tjmp __aw_unimplemented\n", i);
        fprintf(o, "\t.section .rodata.str1.1,\"aMS\",@progbits,1\n.Lname%d:\n\t.asciz \"%s!%s\"\n\t.text\n", i,
                imps[i].dll, imps[i].func);
    }
    free(done);
    fprintf(o, "\t.section .note.GNU-stack,\"\",@progbits\n");
    fclose(o);

    char *as_argv[] = {(char *)cc, "-c", asmf, "-o", obj, NULL};
    if (run(as_argv))
        die("assembling failed");

    /* place the runtime ELF image away from the PE image */
    char ttext[64];
    uint64_t lo = image_base, hi = image_base + image_size;
    uint64_t rtbase = 0x400000;
    if (!(hi <= rtbase || lo >= rtbase + 0x10000000))
        rtbase = 0x7e0000000ull;
    snprintf(ttext, sizeof ttext, "-Wl,-Ttext-segment=0x%llx", (unsigned long long)rtbase);

    char **ld = calloc(40 + nsectopts, sizeof(char *));
    int n = 0;
    ld[n++] = (char *)cc;
    ld[n++] = "-static";
    ld[n++] = "-no-pie";
    ld[n++] = "-s";
    ld[n++] = "-o";
    ld[n++] = (char *)out;
    ld[n++] = obj;
    ld[n++] = (char *)rt;
    ld[n++] = ttext;
    for (int i = 0; i < nsectopts; i++)
        ld[n++] = sectopts[i];
    ld[n++] = "-Wl,--no-warn-rwx-segments";
    ld[n++] = "-Wl,-z,noexecstack";
    ld[n++] = "-lm";
    ld[n++] = "-lpthread";
    /* extra flags, e.g. ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all during development */
    char *extra = getenv("ANYWIN_LDFLAGS") ? strdup(getenv("ANYWIN_LDFLAGS")) : NULL;
    for (char *t = extra ? strtok(extra, " ") : NULL; t && n < 30 + nsectopts; t = strtok(NULL, " "))
        ld[n++] = t;
    ld[n] = NULL;
    if (run(ld))
        die("linking failed (work dir %s)", wd);

    if (!keep) {
        unlink(bin);
        unlink(asmf);
        unlink(obj);
        rmdir(wd);
    } else
        fprintf(stderr, "anywin: intermediate files kept in %s\n", wd);
    fprintf(stderr, "anywin: %s -> %s (%d imports, %d unimplemented)\n", in, out, nimps, missing);
    return 0;
}

/*
 * images.c - which lifted code is in guest memory right now.
 *
 * Epic Pinball is several programs sharing one address space: PINBALL.EXE
 * (menu, music host), one table EXE it EXECs at a time, and the MASI driver
 * blobs PINBALL reads into memory and far-calls. tools/lift.py lifts each one
 * at the segment it is loaded at, so an indirect jump or call is resolved by
 * asking which *live* image covers the linear address. The thirteen tables all
 * load at about the same place, which is why liveness matters: only the one
 * PINBALL last EXEC'd answers.
 *
 * Placement is recorded as it happens (work/layout.txt), and tools/lift.py
 * reads it back: the first run of a table nobody has lifted yet places it, the
 * next lift lifts it. An image found at a segment other than the one it was
 * lifted at is not run -- every relocated segment in its C would be wrong.
 * See docs/architecture.md, "Images".
 */
#include "machine.h"
#include <ctype.h>
#ifdef _WIN32
#include <direct.h>
#endif

extern const LiftedImage *const g_lifted[];      /* work/gen/images_list.c, from tools/lift.py */
#define lifted g_lifted

typedef struct {
    const LiftedImage *li;      /* NULL: placed but not lifted */
    char file[13];
    uint32_t base, len;         /* linear */
    int live;
} Placed;
static Placed placed[32];
static int nplaced;

static const LiftedImage *by_file(const char *file)
{
    for (int i = 0; lifted[i]; i++)
        if (!_stricmp(lifted[i]->file, file)) return lifted[i];
    return NULL;
}

/* The image name tools/lift.py uses for a file; 0 for a driver it does not lift. */
static int image_name(const char *file, char *name)
{
    static const char *const fixed[][2] = {
        {"PINBALL.EXE", "pin"}, {"MDRV000R.MUS", "mlpr"}, {"MDRV001R.MUS", "nosnd"}, {"MDRV004R.MUS", "sb"}};
    for (int i = 0; i < 4; i++) if (!_stricmp(file, fixed[i][0])) { strcpy(name, fixed[i][1]); return 1; }
    if (_strnicmp(file, "MDRV", 4) == 0) return 0;
    int i = 0;
    for (; file[i] && file[i] != '.' && i < 15; i++) name[i] = (char)tolower((unsigned char)file[i]);
    name[i] = 0;
    return 1;
}

/* work/layout.txt: "<image> <segment>", one per image, for tools/lift.py */
static void record_layout(const char *file, uint16_t seg)
{
    char name[16];
    if (!image_name(file, name)) return;
    char line[64], buf[4096] = {0};
    snprintf(line, sizeof line, "%s %04X\n", name, seg);
    FILE *f = fopen("work/layout.txt", "r");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    /* drop any older line for this image */
    char out[4096] = {0}, *p = buf;
    while (*p) {
        char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p + 1) : strlen(p);
        if (!(strncmp(p, name, strlen(name)) == 0 && p[strlen(name)] == ' ')) strncat(out, p, l);
        p += l;
    }
    strcat(out, line);
    _mkdir("work");
    f = fopen("work/layout.txt", "w");
    if (f) { fputs(out, f); fclose(f); }
}

static uint32_t fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = 0x811C9DC5u;
    while (n--) h = (h ^ *p++) * 0x01000193u;
    return h;
}

/* A program or driver now occupies [base, base+len): `raw` is the whole file
 * it came from (identity), `seg` the segment its first byte is at. Anything
 * live that it overlaps has been overwritten. */
void images_placed(const char *file, const uint8_t *raw, uint32_t rawlen, uint16_t seg, uint32_t len)
{
    uint32_t base = (uint32_t)seg * 16;
    for (int i = 0; i < nplaced; i++)
        if (placed[i].live && base < placed[i].base + placed[i].len && placed[i].base < base + len) {
            placed[i].live = 0;
            trace("[image] %s overwritten by %s\n", placed[i].file, file);
        }
    const LiftedImage *li = by_file(file);
    if (li && (li->size != rawlen || li->fnv != fnv(raw, rawlen)))
        fatal("%s is not the file that was lifted (%u bytes, expected %u) -- run tools/lift.py again",
              file, rawlen, li->size);
    if (li && li->load_seg != seg) {
        record_layout(file, seg);
        fatal("%s was lifted at segment %04X but is loaded at %04X -- run tools/lift.py again "
              "(work/layout.txt has the new place)", file, li->load_seg, seg);
    }
    if (!li) {
        record_layout(file, seg);
        fprintf(stderr, "[image] %s at %04X is not lifted yet; work/layout.txt now places it "
                        "-- run tools/lift.py and rebuild\n", file, seg);
    }
    Placed *p = NULL;
    for (int i = 0; i < nplaced; i++) if (!_stricmp(placed[i].file, file)) p = &placed[i];
    if (!p && nplaced < 32) p = &placed[nplaced++];
    if (!p) return;
    p->li = li; snprintf(p->file, sizeof p->file, "%s", file);
    p->base = base; p->len = len; p->live = 1;
    trace("[image] %s live at %04X (%u bytes)%s\n", file, seg, len, li ? "" : ", not lifted");
}

/* A program has exited: its memory is free, its code is gone. */
void images_gone(uint16_t seg)
{
    for (int i = 0; i < nplaced; i++)
        if (placed[i].live && placed[i].base == (uint32_t)seg * 16) {
            placed[i].live = 0;
            trace("[image] %s gone\n", placed[i].file);
        }
}

static Placed *owner_of(uint32_t lin)
{
    for (int i = 0; i < nplaced; i++)
        if (placed[i].live && lin - placed[i].base < placed[i].len) return &placed[i];
    return NULL;
}

void (*images_lookup(uint16_t seg, uint16_t off))(CPU *)
{
    uint32_t lin = (uint32_t)seg * 16 + off;
    Placed *p = owner_of(lin);
    if (!p || !p->li) return NULL;
    const LiftedImage *li = p->li;
    uint32_t rel = lin - (uint32_t)li->load_seg * 16;
    unsigned lo = 0, hi = li->nfuncs;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (li->funcs[mid].addr < rel) lo = mid + 1; else hi = mid;
    }
    return (lo < li->nfuncs && li->funcs[lo].addr == rel) ? li->funcs[lo].fn : NULL;
}

/* Debug aid: has the registry been stomped? Called every tick in trace builds. */
void images_check(void)
{
    static int had;
    if (nplaced) had = nplaced;
    else if (had) {
        had = 0;
        fprintf(stderr, "[image] registry wiped (host memory corruption)\n");
        { void host_backtrace(void); host_backtrace(); }
#ifdef RECOMP_TRACE
        { void recomp_trace_dump(const char *); recomp_trace_dump("wipe"); }
#endif
    }
}

/* An address the guest reached with no lifted function: into
 * work/misses/<image>.txt, which the next tools/lift.py makes an entry. */
void images_miss(const char *how, uint16_t seg, uint16_t off)
{
    static uint32_t seen[1024]; static int nseen;
    uint32_t key = (uint32_t)seg << 16 | off;
    for (int i = 0; i < nseen; i++) if (seen[i] == key) return;
    if (nseen < 1024) seen[nseen++] = key;
    Placed *p = owner_of((uint32_t)seg * 16 + off);
    fprintf(stderr, "[miss] %s %04X:%04X (%s)\n", how, seg, off, p ? p->file : "no image there");
    if (!p)
        for (int i = 0; i < nplaced; i++)
            trace("  [image] %s base %05X len %X live %d\n", placed[i].file, placed[i].base, placed[i].len, placed[i].live);
    { void host_backtrace(void); host_backtrace(); }
#ifdef RECOMP_TRACE
    { void recomp_trace_dump(const char *); recomp_trace_dump("miss"); }
#endif
    if (!p) return;
    char n[16], path[64];
    if (!image_name(p->file, n)) return;
    _mkdir("work"); _mkdir("work/misses");
    snprintf(path, sizeof path, "work/misses/%s.txt", n);
    char line[16], have[16];
    snprintf(line, sizeof line, "%04X:%04X\n", seg, off);
    FILE *f = fopen(path, "r");
    while (f && fgets(have, sizeof have, f))
        if (!strcmp(have, line)) { fclose(f); return; }      /* recorded by an earlier run */
    if (f) fclose(f);
    f = fopen(path, "a");
    if (f) { fputs(line, f); fclose(f); }
}

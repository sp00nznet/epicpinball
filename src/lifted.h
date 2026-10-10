/*
 * lifted.h - what the generated C (work/gen/<image>/) compiles against.
 *
 * The CPU model is pcrecomp's runtime/recomp16/cpu.h, used as-is. Everything
 * the lifted code calls out to -- interrupts, ports, indirect dispatch -- is
 * declared here and implemented by the DOS machine in src/. Each image's own
 * prototypes and SEG_ constants are in work/gen/<image>/<image>.h.
 * See docs/architecture.md.
 */
#pragma once

#define RECOMP_MEM_HOOK          /* planar VGA: A000 is not plain memory */
#define RECOMP_IRQ               /* loops poll the timer/keyboard/SB */
#include "recomp16/cpu.h"

/* ---- services ---- */
void dos_int21(CPU *cpu);
void bios_int10(CPU *cpu);
void bios_int16(CPU *cpu);
void mouse_int33(CPU *cpu);
void int_handler(CPU *cpu, unsigned num);

uint8_t  port_in8(CPU *cpu, uint16_t port);
uint16_t port_in16(CPU *cpu, uint16_t port);
void     port_out8(CPU *cpu, uint16_t port, uint8_t v);
void     port_out16(CPU *cpu, uint16_t port, uint16_t v);

/* ---- indirect control flow ---- */
void recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off);   /* jmp: no frame */
void dispatch_near(CPU *cpu, uint16_t cs, uint16_t off);      /* call: 2-byte frame pushed */
void dispatch_far(CPU *cpu, uint16_t seg, uint16_t off);      /* call: 4-byte frame pushed */
void recomp_div0(const char *what);
int  cpu_lar(CPU *cpu, uint16_t sel, uint16_t *out);
int  cpu_lsl(CPU *cpu, uint16_t sel, uint16_t *out);

typedef struct { uint32_t addr; void (*fn)(CPU *); } RecompFunc;

/* One lifted program or driver: emitted by tools/lift.py as
 * work/gen/<image>/<image>_table.c; src/images.c keeps them. */
typedef struct {
    const char *name, *file;      /* "ep1", "EP1.EXE" */
    uint16_t load_seg;            /* the segment its first byte was lifted at */
    uint32_t size, fnv;           /* of the file the lift was made from */
    unsigned nfuncs;
    const RecompFunc *funcs;      /* sorted by image-relative linear address */
} LiftedImage;

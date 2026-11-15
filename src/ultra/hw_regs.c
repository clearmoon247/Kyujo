/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * hw_regs.c - the N64's hardware registers, emulated.
 *
 * libultra talks to the console's chips (PI cartridge interface, SI controller interface, AI audio, VI video, MI interrupt
 * controller, SP/DP graphics processor) by reading and writing memory-mapped registers through the IO_READ / IO_WRITE macros.
 * Port patch 0001 turns those macros into calls to HostHwRead32 / HostHwWrite32 below, so libultra's own C code runs unchanged on
 * top of this model.
 *
 * Rules of this file:
 *   - Register addresses and bit names come from lib/ultralib/include/PR/rcp.h (the decomp's copy of Nintendo's header) - nothing
 *     is typed from memory. Each case names its register so a reader can compare it with that header.
 *   - Only behaviour the game actually exercises is modelled. Any other register access stops the program and names the
 *     register (host_missing_register), so a gap is never silent.
 *   - A device finishes its work immediately (a DMA copies at once). Real hardware takes time, but software can only rely on the
 *     completion interrupt, and that is still delivered asynchronously through the scheduler's event mechanism.
 *
 * GAME-side file: libultra headers only (the C library is reached through platform.h).
 */
#include "PR/os_internal.h"
#include "PR/rcp.h"
#include "PR/os_flash.h"
#include "platform.h"
#include "hw_internal.h"

/* ---- address handling ----------------------------------------------------------------------------------------------------------- */

/* libultra passes either a physical address (PI_STATUS_REG) or a KSEG1 one ((u32)osRomBase | devAddr, 0xB0xxxxxx). Both name the
 * same physical address once the segment bits are dropped. */
#define PHYS(a) ((a) & 0x1FFFFFFFu)

/* Cartridge ROM seen by the CPU/PI: domain 1, address 2 (rcp.h: PI_DOM1_ADDR2). */
#define CART_ROM_BASE PI_DOM1_ADDR2
#define CART_ROM_END  0x1FBFFFFFu

/* ---- device state ---------------------------------------------------------------------------------------------------------------- */

/* MI: which devices are asking for an interrupt (MI_INTR_REG) and which are allowed to (MI_INTR_MASK_REG). */
static u32 sMiIntr, sMiMask;

/* PI */
static u32 sPiDramAddr, sPiCartAddr;
/* Bus timing for the two PI domains (PI_BSD_DOM1/2_*). They only describe how fast the cartridge bus is clocked; the emulated
 * DMA is instant, so the values are stored and read back and have no other effect. */
static u32 sPiDomain[8];

/* SI: the DMA address register and the interrupt flag live here; PIF RAM and the controllers are in hw_pif.c. */
static u32 sSiDramAddr;
static u32 sSiIntr;           /* SI_STATUS_INTERRUPT */

/* VI: the registers libultra programs (viswapcontext.c writes all of them each retrace). They are stored for the renderer, which reads
 * the frame buffer origin, width, scale and control bits when it presents a frame. The registers that set the timing also drive the
 * host's field clock (vi_update_clock). */
typedef struct {
    u32 control, origin, width, intr, burst, vSync, hSync, leap, hStart, vStart, vBurst, xScale, yScale;
} ViRegs;
static ViRegs sVi = {
    /* Power-on state of a console whose firmware has left video running in NTSC. Only the two timing registers matter before
     * libultra sets its own; their values are those of libultra's NTSC mode table (src/vimodes/vimodentsclan1.c: VSYNC(525),
     * HSYNC(3093, 0)). VI_INTR starts "off": 0xFFFF is beyond the last line, and no interrupt is raised until libultra writes it. */
    0, 0, 0, 0xFFFF, 0, 525, 3093, 0, 0, 0, 0, 0, 0
};
extern s32 osViClock;          /* set by osInitialize from osTvType: 48681812 Hz for NTSC (rcp.h VI_NTSC_CLOCK) */

/* ---- interrupt bookkeeping ----------------------------------------------------------------------------------------------------- */

/* Raises a device interrupt: sets its MI_INTR bit and posts the event libultra's scheduler turns into a message. */
static void raise_irq(u32 mi_bit, int event)
{
    sMiIntr |= mi_bit;
    host_event_post(event);
}

void HostHwRaiseSpIrq(void) { raise_irq(MI_INTR_SP, OS_EVENT_SP); }
void HostHwAckSpIrq(void) { sMiIntr &= ~MI_INTR_SP; }
void HostHwRaiseDpIrq(void) { raise_irq(MI_INTR_DP, OS_EVENT_DP); }
void HostHwRaiseAiIrq(void) { raise_irq(MI_INTR_AI, OS_EVENT_AI); }
void HostHwAckAiIrq(void) { sMiIntr &= ~MI_INTR_AI; }

/* Called by the scheduler layer (os_sched.c) when it takes an event. On the console the exception handler acknowledges the device
 * (e.g. writes PI_STATUS_REG with PI_CLR_INTR) at that point; here the matching state is cleared. */
void HostHwAckEvent(int event)
{
    switch (event) {
    case OS_EVENT_SP:                                    /* exceptasm.s "rcp:": writes SP_CLR_INTR | SP_CLR_SIG3 to SP_STATUS_REG */
        HostSpWrite32(SP_STATUS_REG, SP_CLR_INTR | SP_CLR_SIG3);
        break;
    case OS_EVENT_PI: sMiIntr &= ~MI_INTR_PI; break;
    case OS_EVENT_SI: sMiIntr &= ~MI_INTR_SI; sSiIntr = 0; break;
    case OS_EVENT_AI: sMiIntr &= ~MI_INTR_AI; break;
    case OS_EVENT_VI: sMiIntr &= ~MI_INTR_VI; break;
    case OS_EVENT_DP: sMiIntr &= ~MI_INTR_DP; break;
    default: break;
    }
}

/* ---- PI: cartridge DMA --------------------------------------------------------------------------------------------------------- */

/* Copies `len` bytes of cartridge space starting at physical address `cart` into host memory at `dram` (a native pointer: in this
 * port a pointer IS the "physical" address handed to the RCP, see osVirtualToPhysical in os_memmap.c). */
/* Diagnostic helper: logs "<label> 0x<8 hex digits>" (the game side has no printf). */
void HostHwLogHex(const char *label, u32 v)
{
    char buf[96];
    int n = 0, i;
    static const char digits[] = "0123456789abcdef";
    while (label[n] && n < 80) {
        buf[n] = label[n];
        n++;
    }
    buf[n++] = ' '; buf[n++] = '0'; buf[n++] = 'x';
    for (i = 28; i >= 0; i -= 4) {
        buf[n++] = digits[(v >> i) & 0xF];
    }
    buf[n] = 0;
    host_log(buf);
}

static void pi_dma_cart_to_dram(u32 cart, u32 dram, u32 len)
{
    u32 offset;
    if (flash_owns(cart)) { flash_dma_to_dram(cart, (void *)dram, len); return; }
    if (cart < CART_ROM_BASE || cart > CART_ROM_END) host_missing_register("DMA from a non-cartridge-ROM address by", cart);
    offset = cart - CART_ROM_BASE;
    if (offset + len > host_rom_size()) {
        HostHwLogHex("kyujo: PI DMA: cartridge address", cart);
        HostHwLogHex("kyujo: PI DMA: memory address   ", dram);
        HostHwLogHex("kyujo: PI DMA: length           ", len);
        host_fatal("PI DMA reads past the end of the ROM");
    }
    host_copy_from_rom(offset, (void *)dram, len);
}

static void pi_write(u32 reg, u32 v)
{
    switch (reg) {
    case PI_DRAM_ADDR_REG: sPiDramAddr = v; break;
    case PI_CART_ADDR_REG: sPiCartAddr = v; break;
    case PI_WR_LEN_REG:                       /* "write to RDRAM": cartridge -> memory. The register holds length-1. */
        pi_dma_cart_to_dram(sPiCartAddr, sPiDramAddr, v + 1);
        raise_irq(MI_INTR_PI, OS_EVENT_PI);
        break;
    case PI_RD_LEN_REG:                       /* memory -> cartridge: only the flash chip accepts data */
        if (!flash_owns(sPiCartAddr)) host_missing_register("DMA to the cartridge address", sPiCartAddr);
        flash_dma_from_dram(sPiCartAddr, (const void *)sPiDramAddr, v + 1);
        raise_irq(MI_INTR_PI, OS_EVENT_PI);
        break;
    case PI_STATUS_REG:
        if (v & PI_STATUS_CLR_INTR) sMiIntr &= ~MI_INTR_PI;
        break;                                 /* PI_STATUS_RESET: nothing to reset */
    case PI_BSD_DOM1_LAT_REG: sPiDomain[0] = v; break;
    case PI_BSD_DOM1_PWD_REG: sPiDomain[1] = v; break;
    case PI_BSD_DOM1_PGS_REG: sPiDomain[2] = v; break;
    case PI_BSD_DOM1_RLS_REG: sPiDomain[3] = v; break;
    case PI_BSD_DOM2_LAT_REG: sPiDomain[4] = v; break;
    case PI_BSD_DOM2_PWD_REG: sPiDomain[5] = v; break;
    case PI_BSD_DOM2_PGS_REG: sPiDomain[6] = v; break;
    case PI_BSD_DOM2_RLS_REG: sPiDomain[7] = v; break;
    default: host_missing_register("PI register write", reg);
    }
}

static u32 pi_read(u32 reg)
{
    switch (reg) {
    case PI_STATUS_REG:       return 0;                  /* never busy: DMAs complete instantly */
    case PI_DRAM_ADDR_REG:    return sPiDramAddr;
    case PI_CART_ADDR_REG:    return sPiCartAddr;
    case PI_BSD_DOM1_LAT_REG: return sPiDomain[0];
    case PI_BSD_DOM1_PWD_REG: return sPiDomain[1];
    case PI_BSD_DOM1_PGS_REG: return sPiDomain[2];
    case PI_BSD_DOM1_RLS_REG: return sPiDomain[3];
    case PI_BSD_DOM2_LAT_REG: return sPiDomain[4];
    case PI_BSD_DOM2_PWD_REG: return sPiDomain[5];
    case PI_BSD_DOM2_PGS_REG: return sPiDomain[6];
    case PI_BSD_DOM2_RLS_REG: return sPiDomain[7];
    default: host_missing_register("PI register read", reg);
    }
}

/* ---- VI: video interface ------------------------------------------------------------------------------------------------------- */

/* Derives the field length from the timing registers and (re)starts the host's field clock.
 *   A video line lasts (H_SYNC[11:0] + 1) VI clock periods; V_SYNC is the number of HALF-lines in a field (rcp.h comments).
 *   So a field lasts V_SYNC/2 lines. For the NTSC table: 525/2 * 3094 / 48,681,812 Hz = 16.683 ms = 1/59.94 s, the NTSC field rate
 *   (an arithmetic cross-check of the formula, not something the headers state).
 * The interrupt is raised when the beam reaches half-line V_INTR (VI_INTR_REG: "interrupt when current half-line = V_INTR"). */
static void vi_update_clock(void)
{
    unsigned long long line_clocks = (sVi.hSync & 0xFFF) + 1ULL;
    unsigned long long half_lines = sVi.vSync;
    unsigned long long clock_hz = (unsigned long long)(u32)osViClock;
    unsigned long long period_ns, offset_ns;

    host_trace("vi timing: v_sync, intr", sVi.vSync, sVi.intr);
    if (half_lines == 0 || clock_hz == 0) return;             /* not programmed yet */
    period_ns = half_lines * line_clocks * 500000000ULL / clock_hz;     /* half a line = line_clocks/2 clocks */
    offset_ns = (sVi.intr < half_lines) ? period_ns * sVi.intr / half_lines : 0;
    host_vi_set_timing(period_ns, offset_ns, (sVi.intr < half_lines) ? OS_EVENT_VI : -1);
}

static u32 vi_read(u32 reg)
{
    switch (reg) {
    case VI_CONTROL_REG: return sVi.control;
    case VI_ORIGIN_REG:  return sVi.origin;
    case VI_WIDTH_REG:   return sVi.width;
    case VI_INTR_REG:    return sVi.intr;
    case VI_CURRENT_REG: {
        /* Current half-line, from the host's field clock. In interlaced modes (serrate on) the lowest bit is the field number;
         * otherwise it is constant (rcp.h: VI_CURRENT_REG comment). */
        u32 pos, half_line;
        vi_update_clock();                                    /* starts the clock on first use */
        pos = host_vi_position(sVi.vSync);                    /* (half_line << 1) | field & 1 */
        half_line = (pos >> 1) & ~1u;
        if (sVi.control & VI_CTRL_SERRATE_ON) half_line |= (pos & 1);
        return half_line;
    }
    case VI_BURST_REG:   return sVi.burst;
    case VI_V_SYNC_REG:  return sVi.vSync;
    case VI_H_SYNC_REG:  return sVi.hSync;
    case VI_LEAP_REG:    return sVi.leap;
    case VI_H_START_REG: return sVi.hStart;
    case VI_V_START_REG: return sVi.vStart;
    case VI_V_BURST_REG: return sVi.vBurst;
    case VI_X_SCALE_REG: return sVi.xScale;
    case VI_Y_SCALE_REG: return sVi.yScale;
    default: host_missing_register("VI register read", reg);
    }
}

static void vi_write(u32 reg, u32 v)
{
    switch (reg) {
    case VI_CONTROL_REG:
        sVi.control = v;
        /* libultra's __osViSwapContext writes VI_CONTROL last, so all registers of the new frame are now in place: tell the video output. */
        HostHwGfxCompleteFor(sVi.origin);     /* the field shows what the RSP has drawn: finish the graphics task first if it draws into this very buffer */
        host_vi_commit(sVi.control, sVi.origin, sVi.width, sVi.hStart, sVi.vStart, sVi.xScale, sVi.yScale);
        break;
    case VI_ORIGIN_REG:  sVi.origin = v; break;
    case VI_WIDTH_REG:   sVi.width = v; break;
    case VI_INTR_REG:    sVi.intr = v; vi_update_clock(); break;
    case VI_CURRENT_REG: sMiIntr &= ~MI_INTR_VI; break;       /* "Any write to this register will clear interrupt line" (rcp.h) */
    case VI_BURST_REG:   sVi.burst = v; break;
    case VI_V_SYNC_REG:  sVi.vSync = v; vi_update_clock(); break;
    case VI_H_SYNC_REG:  sVi.hSync = v; vi_update_clock(); break;
    case VI_LEAP_REG:    sVi.leap = v; break;
    case VI_H_START_REG: sVi.hStart = v; break;
    case VI_V_START_REG: sVi.vStart = v; break;
    case VI_V_BURST_REG: sVi.vBurst = v; break;
    case VI_X_SCALE_REG: sVi.xScale = v; break;
    case VI_Y_SCALE_REG: sVi.yScale = v; break;
    default: host_missing_register("VI register write", reg);
    }
}

/* ---- the public entry points ----------------------------------------------------------------------------------------------------- */

/* Types spelled `unsigned int` on purpose: they must match the declarations in port patch 0001 (rcp.h), and libultra's u32 is
 * `unsigned long` on this compiler. */
unsigned int HostHwRead32(unsigned int addr)
{
    u32 a = PHYS(addr);

    if (a >= CART_ROM_BASE && a <= CART_ROM_END) {            /* direct CPU/PI read of cartridge ROM, e.g. osPiRawReadIo */
        u32 off = a - CART_ROM_BASE;
        const unsigned char *rom = host_rom_data();
        if (!rom || off + 4 > host_rom_size()) host_fatal("read of cartridge space outside the loaded ROM");
        return ((u32)rom[off] << 24) | ((u32)rom[off + 1] << 16) | ((u32)rom[off + 2] << 8) | rom[off + 3];
    }
    if (flash_owns(a)) return flash_read(a);
    if (a >= PIF_RAM_START && a <= PIF_RAM_END) return HostPifRead32(a - PIF_RAM_START);
    if (a >= PI_BASE_REG && a <= PI_BASE_REG + 0x33) return pi_read(a);
    if (a >= VI_BASE_REG && a <= VI_Y_SCALE_REG) return vi_read(a);
    if (a >= AI_BASE_REG && a <= AI_BITRATE_REG) return HostAiRead32(a);
    if (HostSpOwns(a)) return HostSpRead32(a);

    switch (a) {
    case SI_STATUS_REG:      return sSiIntr ? SI_STATUS_INTERRUPT : 0;       /* never busy */
    case SI_DRAM_ADDR_REG:   return sSiDramAddr;
    case MI_INTR_REG:        return sMiIntr;
    case MI_INTR_MASK_REG:   return sMiMask;
    default:                 host_missing_register("read", a);
    }
}

void HostHwWrite32(unsigned int addr, unsigned int data_in)
{
    u32 data = data_in;
    u32 a = PHYS(addr);

    if (flash_owns(a)) { flash_write(a, data); return; }
    if (a >= PIF_RAM_START && a <= PIF_RAM_END) { HostPifWrite32(a - PIF_RAM_START, data); return; }
    if (a >= PI_BASE_REG && a <= PI_BASE_REG + 0x33) { pi_write(a, data); return; }
    if (a >= VI_BASE_REG && a <= VI_Y_SCALE_REG) { vi_write(a, data); return; }
    if (a >= AI_BASE_REG && a <= AI_BITRATE_REG) { HostAiWrite32(a, data); return; }
    if (HostSpOwns(a)) { HostSpWrite32(a, data); return; }

    switch (a) {
    case SI_STATUS_REG:      sSiIntr = 0; sMiIntr &= ~MI_INTR_SI; break;     /* any write clears the SI interrupt */
    case SI_DRAM_ADDR_REG:   sSiDramAddr = data; break;
    case SI_PIF_ADDR_RD64B_REG:                                              /* PIF RAM -> memory; the written value is the PIF address */
        if (data != PIF_RAM_START) host_missing_register("SI DMA from a PIF address other than PIF RAM:", data);
        {
            unsigned long long wire_ns = HostPifDmaToDram((void *)sSiDramAddr);
            sSiIntr = 1;
            if (wire_ns == 0) {
                raise_irq(MI_INTR_SI, OS_EVENT_SI);                          /* nothing was exchanged on the wire: done at once */
            } else {
                /* The Joybus transfer takes time on the console, and the game relies on it (the GB Tower emulator starts the Game Boy boot
                 * program while the cartridge is still being read; see docs/known-decomp-issues.md). The interrupt is raised later, by the
                 * timer thread (it may only post the event, not touch game state). The MI/SI status bits are set at once, slightly earlier than
                 * the hardware would; nothing reads them before the event (libultra waits for the interrupt). */
                sMiIntr |= MI_INTR_SI;
                host_event_post_after(wire_ns, OS_EVENT_SI);
            }
        }
        break;
    case SI_PIF_ADDR_WR64B_REG:                                              /* memory -> PIF RAM */
        if (data != PIF_RAM_START) host_missing_register("SI DMA to a PIF address other than PIF RAM:", data);
        HostPifDmaFromDram((const void *)sSiDramAddr);
        sSiIntr = 1;
        raise_irq(MI_INTR_SI, OS_EVENT_SI);
        break;
    case MI_INTR_MASK_REG:                                                   /* write: pairs of CLR/SET bits (rcp.h) */
        if (data & MI_INTR_MASK_CLR_SP) sMiMask &= ~MI_INTR_MASK_SP;
        if (data & MI_INTR_MASK_SET_SP) sMiMask |=  MI_INTR_MASK_SP;
        if (data & MI_INTR_MASK_CLR_SI) sMiMask &= ~MI_INTR_MASK_SI;
        if (data & MI_INTR_MASK_SET_SI) sMiMask |=  MI_INTR_MASK_SI;
        if (data & MI_INTR_MASK_CLR_AI) sMiMask &= ~MI_INTR_MASK_AI;
        if (data & MI_INTR_MASK_SET_AI) sMiMask |=  MI_INTR_MASK_AI;
        if (data & MI_INTR_MASK_CLR_VI) sMiMask &= ~MI_INTR_MASK_VI;
        if (data & MI_INTR_MASK_SET_VI) sMiMask |=  MI_INTR_MASK_VI;
        if (data & MI_INTR_MASK_CLR_PI) sMiMask &= ~MI_INTR_MASK_PI;
        if (data & MI_INTR_MASK_SET_PI) sMiMask |=  MI_INTR_MASK_PI;
        if (data & MI_INTR_MASK_CLR_DP) sMiMask &= ~MI_INTR_MASK_DP;
        if (data & MI_INTR_MASK_SET_DP) sMiMask |=  MI_INTR_MASK_DP;
        break;
    default:                 host_missing_register("write", a);
    }
}

/* Target of the HW_REG() macro after port patch 0007: a direct access to a register that nothing models. It never returns. */
void *HostHwDirectAccess(unsigned int reg)
{
    host_missing_register("direct (HW_REG) access to", PHYS(reg));
}

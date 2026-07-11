/*
 * impact_cursor.c - hardware cursor support for the SGI Impact/ImpactSR.
 * Copyright (C) 2019 - 2026 René Rebe <rene@exactco.de>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "impact.h"
#include "cursorstr.h"

#include "servermd.h"

#define CURS_MAX 32

static void ImpactShowCursor(ScrnInfoPtr pScrn);
static void ImpactHideCursor(ScrnInfoPtr pScrn);
static void ImpactSetCursorPosition(ScrnInfoPtr pScrn, int x, int y);
static void ImpactSetCursorColors(ScrnInfoPtr pScrn, int bg, int fg);
/*static void ImpactLoadCursorImage(ScrnInfoPtr pScrn, unsigned char *bits);*/
static unsigned char* ImpactRealizeCursor(xf86CursorInfoPtr infoPtr, CursorPtr pCurs);

/* PROM's MgrasInitCMAP writes xmap pp1_select = 1 for broadcast cmap access */
#define MGRAS_XMAP_WRITEALLPP1	1

#define mgras_xmapSetPP1Select(xmap,data)	(xmap)->pp1select = data
#define mgras_xmapSetAddr(xmap,addr)	(xmap)->index = addr
#define mgras_xmapSetDIBdata(xmap,data)	(xmap)->dib = data
#define mgras_xmapSetConfig(xmap,data)	(xmap)->config = data

Bool
ImpactHWCursorInit(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	ImpactPtr pImpact = IMPACTPTR(pScrn);
	ImpactRegsPtr pImpactRegs = IMPACTREGSPTR(pScrn);
	xf86CursorInfoPtr infoPtr;
	CARD16 tmp;

	infoPtr = xf86CreateCursorInfoRec();
	if(!infoPtr)
		return FALSE;

	pImpact->CursorInfoRec = infoPtr;
	infoPtr->MaxWidth = CURS_MAX;
	infoPtr->MaxHeight = CURS_MAX;
	infoPtr->Flags = HARDWARE_CURSOR_TRUECOLOR_AT_8BPP;

	infoPtr->SetCursorColors = ImpactSetCursorColors;
	infoPtr->SetCursorPosition = ImpactSetCursorPosition;
	infoPtr->LoadCursorImage = ImpactLoadCursorImage;
	infoPtr->HideCursor = ImpactHideCursor;
	infoPtr->ShowCursor = ImpactShowCursor;
	infoPtr->RealizeCursor = ImpactRealizeCursor;
	infoPtr->UseHWCursor = NULL;

	return xf86InitCursor(pScreen, infoPtr);
}


static void ImpactShowCursor(ScrnInfoPtr pScrn)
{
	ImpactPtr pImpact = IMPACTPTR(pScrn);
	ImpactRegsPtr pImpactRegs = pImpact->pImpactRegs;
	unsigned short val = (*pImpact->Vc3Get)( pImpactRegs, VC3_IREG_CURSOR);
	(*pImpact->Vc3Set)(pImpactRegs, 0x1d, val | (0x2 | VC3_CTRL_ECURS));
}

static void ImpactHideCursor(ScrnInfoPtr pScrn)
{
	ImpactPtr pImpact = IMPACTPTR(pScrn);
	ImpactRegsPtr pImpactRegs = pImpact->pImpactRegs;
	unsigned short val = (*pImpact->Vc3Get)( pImpactRegs, VC3_IREG_CURSOR);
	(*pImpact->Vc3Set)(pImpactRegs, 0x1d, val & ~VC3_CTRL_ECURS);
}

static void ImpactSetCursorPosition(ScrnInfoPtr pScrn, int x, int y)
{
	ImpactPtr pImpact = IMPACTPTR(pScrn);
	ImpactRegsPtr pImpactRegs = pImpact->pImpactRegs;
	(*pImpact->Vc3Set)( pImpactRegs, VC3_IREG_CURSX, (CARD16) x + 31);
        (*pImpact->Vc3Set)( pImpactRegs, VC3_IREG_CURSY, (CARD16) y + 31);
}

#define HQ3_BFIFO_MAX		16

/* Upper bound on any hardware spin-wait.  The PROM's equivalents are unbounded,
 * but a stuck poll here would lock the whole machine; bail out instead. */
#define MGRAS_SPIN_MAX		10000000

/* Wait for the BFIFO (DCB FIFO) to drain.  Matches MgrasInitCMAP in the IP30
 * PROM: it spins while ((depth+1)>>1) >= 7, where depth = giostatus & 0x1f.
 * 'gio' is the board's giostatus register (I2 and SR keep it at different
 * offsets, so the caller passes the right one). */
static __inline__ void
mgras_BFIFOWAIT(mgireg32_t *gio)
{
	int n = MGRAS_SPIN_MAX;
	while (--n && (((*gio & 0x1f) + 1) >> 1) >= 7);
}

/* Wait for the BFIFO to drain, then for the CMAP to be ready.  The PROM polls
 * cmap0 status until bit 0x08 (ready) is SET - not the 0x04 busy bit, and not
 * the opposite polarity, which would spin forever.  Must be called with CBlank
 * at its normal value so the CMAP FIFO can actually drain. */
static __inline__ void
mgras_cmapFIFOWAIT(mgireg32_t *gio, Impact_cmapregs_t *cmap)
{
	int n = MGRAS_SPIN_MAX;
	mgras_BFIFOWAIT(gio);
	while (--n && !(cmap->status & 0x08));
}

/* The CMAP 16-bit address register is loaded byte-swapped. */
#define mgras_cmapAddr(v)	((((v) << 8) & 0xff00) | (((v) >> 8) & 0xff))

/* Force CBlank high while touching the CMAP (works around the documented
 * CMAP-FIFO update bug) by setting the XMAP DIB top-scan register. */
static void
mgras_cmapToggleCblank(Impact_xmapregs_t *xmap, int disable)
{
	mgras_xmapSetAddr(xmap, 4);			/* DIB top-scan register */
	if (disable)
		mgras_xmapSetDIBdata(xmap, (0x3ff | (1 << 14)));
	else
		mgras_xmapSetDIBdata(xmap, 0x3ff);
}

/*
 * The cursor's two visible colors live in the CMAP, addressed through the
 * broadcast "cmapall" DCB port so both color-map banks are updated at once.
 * The palette base is fixed by the PROM: MgrasInitCursor (IP30 PROM, verified
 * by disassembly) loads its cursor CmapData with mgras_LoadCmap(base, 0x1cfc,
 * ..., 8), i.e. cursor-MSB 0x73f, base 0x1cfc.  The 2-bit cursor pixel value
 * is added to that base.
 *
 * ImpactRealizeCursor builds a 2-plane glyph: plane 0 marks foreground pixels
 * (source&mask) and plane 1 marks background pixels (~source&mask), giving pixel
 * value 1 = foreground, value 2 = background, value 0 = transparent.  So the
 * foreground color goes to base+1 and the background color to base+2 - which
 * matches the PROM's CmapData (entry+1 = red, entry+2 = white).
 */
#define MGRAS_CURSOR_CMAP_BASE	0x1cfc		/* cursor-MSB 0x73f << 2 */

/* The CMAP 'pal' register takes the colour as (r<<24)|(g<<16)|(b<<8).  The X
 * server hands us fg/bg as packed 8-8-8 RGB (0xRRGGBB), so a single <<8 puts
 * R,G,B into the top three bytes. */
#define IMPACT_CMAP_PAL(rgb)	((unsigned)(rgb) << 8)

static void ImpactSetCursorColors(ScrnInfoPtr pScrn, int bg, int fg)
{
	ImpactPtr pImpact = IMPACTPTR(pScrn);
	ImpactRegsPtr pImpactRegs = pImpact->pImpactRegs;
	int isSR = (&ImpactSRXmapGetModeRegister == pImpact->XmapGetModeRegister);

	Impact_xmapregs_t*  xmap    = isSR ? &pImpactRegs->sr.xmap
					   : &pImpactRegs->i2.xmap;
	Impact_cmapregs_t*  cmapall = isSR ? &pImpactRegs->sr.cmapall
					   : &pImpactRegs->i2.cmapall;
	Impact_cmapregs_t*  cmap0   = isSR ? &pImpactRegs->sr.cmap0
					   : &pImpactRegs->i2.cmap0;
	mgireg32_t*         gio     = isSR ? &pImpactRegs->sr.giostatus
					   : &pImpactRegs->i2.giostatus;
	unsigned base = MGRAS_CURSOR_CMAP_BASE;

	/* Follow MgrasInitCMAP's ordering: the CMAP FIFO only drains while CBlank
	 * is at its normal value (0x3ff), so we poll "ready" (status bit 0x08)
	 * with CBlank normal, THEN force CBlank high (0x43ff) to write - polling
	 * with CBlank forced high would wait forever because the FIFO can't drain.
	 * Two colours never overflow the FIFO, so no mid-write status poll. */
	mgras_xmapSetPP1Select(xmap, MGRAS_XMAP_WRITEALLPP1);

	mgras_cmapToggleCblank(xmap, 0);		/* CBlank normal: FIFO drains */
	mgras_cmapFIFOWAIT(gio, cmap0);			/* wait until CMAP ready */

	mgras_cmapToggleCblank(xmap, 1);		/* force CBlank high to write */

	/* Address each slot explicitly (auto-increment is off), writing the
	 * byte-swapped address twice: an SGI workaround for the address being
	 * dropped when the CMAP bus turns around. */
	cmapall->addr = mgras_cmapAddr(base + 1);
	cmapall->addr = mgras_cmapAddr(base + 1);
	cmapall->pal  = IMPACT_CMAP_PAL(fg);	/* pixel value 1: foreground */

	cmapall->addr = mgras_cmapAddr(base + 2);
	cmapall->addr = mgras_cmapAddr(base + 2);
	cmapall->pal  = IMPACT_CMAP_PAL(bg);	/* pixel value 2: background */

	mgras_cmapToggleCblank(xmap, 0);	/* CBlank normal again: drain writes */
	mgras_BFIFOWAIT(gio);
}

static unsigned char* ImpactRealizeCursor(xf86CursorInfoPtr infoPtr, CursorPtr pCurs)
{
	int size = (infoPtr->MaxWidth * infoPtr->MaxHeight) >> 2;
	CARD32 *mem, *SrcS, *SrcM, *DstS;
	unsigned int i;

	if (!(mem = calloc(1, size)))
        	return NULL;
	SrcS = (CARD32*)pCurs->bits->source;
    	SrcM = (CARD32*)pCurs->bits->mask;
    	DstS = mem;
	/* first color: maximum is 32*4 Bytes */
	for(i=0; i < pCurs->bits->height; i++) {
		*DstS = *SrcS & *SrcM;
		DstS++, SrcS++, SrcM++;
	}
	/* second color is the lower of mem: again 32*4 Bytes at most */
	DstS = mem + CURS_MAX;
	SrcS = (CARD32*)pCurs->bits->source;
    	SrcM = (CARD32*)pCurs->bits->mask;
	for(i=0; i < pCurs->bits->height; i++) {
		*DstS = (~*SrcS) & *SrcM;
		DstS++, SrcS++, SrcM++;
	}
	return (unsigned char*) mem;
}

void ImpactLoadCursorImage(ScrnInfoPtr pScrn, unsigned char *bits)
{
        ImpactPtr pImpact = IMPACTPTR(pScrn);
        ImpactRegsPtr pImpactRegs = pImpact->pImpactRegs;
	int i;

        const int curSramAddr = 0x500; /* TODO: maybe just read, like newport? */
        (*pImpact->Vc3Set)( pImpactRegs, VC3_IRES_RAM_ADDR, curSramAddr);

	/* address of cursor data in vc3 ram */
        for (i = 0; i < ((CURS_MAX * CURS_MAX) >> 3); i++) {
		/* write cursor data */
		pImpactRegs->sr.vc3.ram = *(unsigned short*)bits;
		bits += sizeof(unsigned short);
        }
	(*pImpact->Vc3Set)( pImpactRegs, VC3_IREG_CURSEP, curSramAddr);
}

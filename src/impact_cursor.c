impact_cursor.c
 */
/* $XFree86$ */

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

#define MGRAS_XMAP_WRITEALLPP1	0

#define mgras_xmapSetPP1Select(base,data)	base->sr.xmap.pp1select = data
#define mgras_xmapSetAddr(base,addr)	base->sr.xmap.index = addr
#define mgras_xmapSetDIBdata(base,data)	base->sr.xmap.dib = data
#define mgras_xmapSetConfig(base,data)	base->sr.xmap.config = data 

Bool
ImpactHWCursorInit(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
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

#if 0
	/* enable cursor funtion in shadow register */
	pImpact->vc2ctrl |= VC2_CTRL_ECURS;
	/* enable glyph cursor, maximum size is 32x32x2 */
	pImpact->vc2ctrl &= ~( VC2_CTRL_ECG64 | VC2_CTRL_ECCURS);
	/* setup hw cursors cmap base address  */
	NewportBfwait(pNewportRegs);
	pNewportRegs->set.dcbmode = (DCB_XMAP0 | R_DCB_XMAP9_PROTOCOL |
			XM9_CRS_CURS_CMAP_MSB | NPORT_DMODE_W1 );
	tmp = pNewportRegs->set.dcbdata0.bytes.b3;
	pNewportRegs->set.dcbmode = (DCB_XMAP0 | W_DCB_XMAP9_PROTOCOL |
			XM9_CRS_CURS_CMAP_MSB | NPORT_DMODE_W1 );
	pNewportRegs->set.dcbdata0.bytes.b3 = tmp;
	pNewport->curs_cmap_base = (tmp << 5) & 0xffe0;
#endif

	//mgras_xmapSetPP1Select(pImpactRegs, MGRAS_XMAP_WRITEALLPP1);
	//mgras_xmapSetAddr(pImpactRegs, 0x1);        /* Do NOT REMOVE THIS */
	//mgras_xmapSetConfig(pImpactRegs, 0xff000000); /* Hack for Auto Inc */

	return xf86InitCursor(pScreen, infoPtr);
}


static void ImpactShowCursor(ScrnInfoPtr pScrn)
{
	ImpactPtr pImpact = IMPACTPTR(pScrn);
	ImpactRegsPtr pImpactRegs = pImpact->pImpactRegs;
	unsigned short val = (*pImpact->Vc3Get)( pImpactRegs, VC3_IREG_CURSOR);
	//fprintf(stderr, "show cursor: %x\n", val);
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
	/* temp. hack due prom set up 64px cursor? */
	(*pImpact->Vc3Set)( pImpactRegs, VC3_IREG_CURSX, (CARD16) x + 31);
        (*pImpact->Vc3Set)( pImpactRegs, VC3_IREG_CURSY, (CARD16) y + 31);
}

#define HQ3_BFIFO_MAX		16	
#define HQ3_BFIFODEPTH(base)	(base->sr.giostatus & 0x1f)

#define mgras_BFIFOWAIT(base,n)			\
	while (HQ3_BFIFODEPTH(base) > (64-n))

/* For normal operation, wait for BFIFO to emtpy, then check CMAP0 status */
#define mgras_cmapFIFOWAIT(base)					\
	{								\
		mgras_BFIFOWAIT(base,64);				\
		while (!(base->i2.cmap0.status & 0x08));			\
	}
#define new_mgras_cmapFIFOWAIT(base)                    \
    {                               \
        mgras_BFIFOWAIT(base,HQ3_BFIFO_MAX);            \
        while ((base->sr.cmap0.status & 0x04));            \
        while ((base->sr.cmap1.status & 0x04));            \
    }


void
mgras_cmapToggleCblank(ImpactRegsPtr base, int OnOff)
{
       	mgras_xmapSetAddr(base, 4);
	if (OnOff) {				/* CBlank is On */
        	mgras_xmapSetDIBdata(base, (0x3ff | (1 << 14)));
	}
	else
        	mgras_xmapSetDIBdata(base, 0x3ff);
}

#define mgras_cmapSetDiag(base, CmapID, val)				\
{									\
	if (CmapID){ 							\
		base->sr.cmap1.reserved = val;				\
	} else { 							\
		 base->sr.cmap0.reserved = val;				\
	}								\
}

#define mgras_cmapSetAddr(base, reg) base->sr.cmapall.addr = reg
#define mgras_cmapSetRGB(base,r,g,b) base->sr.cmapall.pal = (r << 24) | (g << 16) | (b << 8)

#define MGRAS_CMAP_NCOLMAPENT	8192

static void ImpactSetCursorColors(ScrnInfoPtr pScrn, int bg, int fg)
{
        ImpactPtr pImpact = IMPACTPTR(pScrn);
        ImpactRegsPtr pImpactRegs = pImpact->pImpactRegs;
	int i;

	Impact_xmapregs_t* xmap =
                (&ImpactSRXmapGetModeRegister != pImpact->XmapGetModeRegister)
                        ? &pImpactRegs->i2.xmap
                        : &pImpactRegs->sr.xmap;

	/* for now we index to the adress we know */
	Impact_cmapregs_t* cmap0 = (Impact_cmapregs_t*)&pImpactRegs->sr.cmap0;
        Impact_cmapregs_t* cmap1 = (Impact_cmapregs_t*)&pImpactRegs->sr.cmap1;
        Impact_cmapregs_t* cmapall = (Impact_cmapregs_t*)&pImpactRegs->sr.cmapall;

	/* test if we are at the right spot */
	fprintf(stderr, "xmap/vc3: %p/%p cmaps: %p %p %p - size: %x\n",
		&pImpactRegs->sr.xmap, &pImpactRegs->sr.vc3, cmap0, cmap1, cmapall, sizeof(Impact_cmapregs_t));

	fprintf(stderr, "cmap rev: %x %x %x\n", cmap0->rev & 0x1f, cmap1->rev & 0x1f, cmapall->rev & 0x1f);
	
	uint8_t* regs = (uint8_t*)pImpactRegs;
        uint16_t* addr = (uint16_t*)&pImpactRegs->sr.cmapall.addr; //(regs + 0x70c30 + 0x800);
	uint32_t* pal = (uint32_t*)&pImpactRegs->sr.cmapall.pal; //(regs + 0x70d18 + 0x800);
	fprintf(stderr, "%p %p %p - col: %x %x\n", regs, addr, pal, bg, fg);

	mgras_BFIFOWAIT(pImpactRegs, HQ3_BFIFO_MAX);
	new_mgras_cmapFIFOWAIT(pImpactRegs);

        mgras_xmapSetPP1Select(pImpactRegs, MGRAS_XMAP_WRITEALLPP1);
        mgras_cmapToggleCblank(pImpactRegs, 1); /* disable xmap cblank for writing */

        /* the addr appears to be 16 bit and byte swapped? */
#define ADDR(reg) ((reg << 8) & 0xff00) | ((reg >> 8) & 0xff)

	cmapall->addr = ADDR(0);
        cmapall->addr = ADDR(0);
	cmapall->pal = bg << 8; /* RGBA? */
	cmapall->pal = fg << 8; /* auto increment */

	// load test ramp
	for (i = 0; i < MGRAS_CMAP_NCOLMAPENT; ++i) {

                /* Every 16 colors, check cmap FIFO.
                 * Need to give 2 dummy writes after the read.
                 */
                if ((i == 0) || ((i & 0x1F) == 0x1F)) {
			mgras_cmapToggleCblank(pImpactRegs, 0); /* Enable cblank */
                        /* cmapFIFOWAIT calls BFIFOWAIT */
                        new_mgras_cmapFIFOWAIT(pImpactRegs);
			mgras_cmapToggleCblank(pImpactRegs, 1); /* disable cblank*/

                        mgras_cmapSetAddr(pImpactRegs, addr);
                        mgras_cmapSetAddr(pImpactRegs, addr);

		}
		mgras_cmapSetRGB(pImpactRegs, i & 0xff, i & 0xff, i & 0xff);
	}

        new_mgras_cmapFIFOWAIT(pImpactRegs);
        mgras_cmapToggleCblank(pImpactRegs, 0); /* enable xmap cblank for reading */

	//mgras_cmapSetDiag(pImpactRegs, 1, 1);
        cmap1->addr = ADDR(8191);
	//cmap1->addr = ADDR(0);
        for (i = 0; i < 512; ++i) { 
		uint16_t addr = cmap1->addr; //, addr2 = cmap1->addr;
                //fprintf(stderr, "%02d / %02d: %x / %x\n", ADDR(addr), ADDR(addr2), cmap0->pal, cmap1->pal);
		fprintf(stderr, "%02d: %x\n", ADDR(addr), cmap1->pal);
        }
        //mgras_cmapSetDiag(pImpactRegs, 1, 0);
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
	//(*pImpact->WaitCfifoEmpty)(pImpactRegs); // HQ3/4?

	/* address of cursor data in vc2's ram */
        //tmp = NewportVc2Get( pNewportRegs, VC2_IREG_CENTRY);
 	/* this is where we want to write to: */
        //NewportVc2Set( pNewportRegs, VC2_IREG_RADDR, tmp);
        //pNewportRegs->set.dcbmode = (NPORT_DMODE_AVC2 | VC2_REGADDR_RAM | NPORT_DMODE_W2 | VC2_PROTOCOL);
	/* write cursor data */
        for (i = 0; i < ((CURS_MAX * CURS_MAX) >> 3); i++) {
		//pNewportRegs->set.dcbdata0.hwords.s1 = bits[i];
		pImpactRegs->sr.vc3.ram = *(unsigned short*)bits;
		bits += sizeof(unsigned short);
        }
	(*pImpact->Vc3Set)( pImpactRegs, VC3_IREG_CURSEP, curSramAddr);
}

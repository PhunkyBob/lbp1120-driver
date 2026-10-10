/*
 *  lbp1120rnd.h - Unidrv (v3) render plug-in for the Canon LASER SHOT LBP-1120 (CAPT).
 *
 *  Structure follows the Windows-driver-samples "bitmap" OEM Unidrv render plug-in
 *  (print/OEM Printer Customization Plug-in Samples/C++/bitmap, tag win11-22h2):
 *  IPrintOemUni2 + ImageProcessing collecting the bands + a DrvEndDoc DDI hook.
 *
 *  GPLv2 or later: links the CAPT encoder derived from capt-0.1 (see capt_encode.h).
 */
#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <objbase.h>
#include <stdarg.h>
#include <stdio.h>
#include <windef.h>
#include <winerror.h>
#include <winbase.h>
#include <wingdi.h>
#pragma warning(push)
#pragma warning(disable : 4005) /* ddraw.h (via winddi.h) redefines CO_E_NOTINITIALIZED */
#include <winddi.h>
#pragma warning(pop)
#include <winspool.h>
#include <compstui.h>
#include <winddiui.h>
#include <printoem.h>
#include <prcomoem.h>

#include "capt_page.h"

#define LBP_OEM_SIGNATURE  'CAPT'
#define LBP_OEM_VERSION    0x00010000L

/* *CallbackID values used in lbp1120.gpd */
#define CB_START_PAGE      1   /* *Command: CmdStartPage { *Order: PAGE_SETUP.1 *CallbackID: 1 } */

/* *IPCallbackID of the ColorMode.Mono option */
#define IPCB_MONO_1BPP     1

/* Tuning values read from the printer's PrinterDriverData (GetPrinterData) at EnablePDEV.
 * All optional; defaults reproduce captsend exactly. */
#define REG_TOPSKIP   L"CaptTopSkip"    /* DWORD rows,   default 0 (captsend --topskip)  */
#define REG_LEFTSKIP  L"CaptLeftSkip"   /* DWORD pixels, default 0 (captsend --leftskip) */
#define REG_POLARITY  L"CaptPolarity"   /* DWORD 0 = auto (palette), 1 = band bit 1 is WHITE (invert), 2 = band bit 1 is BLACK */
#define REG_BANDORDER L"CaptBandOrder"  /* DWORD 0 = top-down bands (default), 1 = bottom-up      */

void LbpTrace(const char *fmt, ...);

/* Private devmode (no private settings; kept for the mandatory DevMode method). */
typedef struct tagLBPDEVMODE {
    OEM_DMEXTRAHEADER dmOEMExtra;
} LBPDEVMODE, *PLBPDEVMODE;

/* Private PDEV */
class CLbpPdev {
public:
    CLbpPdev();
    ~CLbpPdev();

    /* Unidrv DDI entry points we chain to from our hooks */
    PFN_DrvStartDoc pfnStartDoc;
    PFN_DrvEndDoc   pfnEndDoc;

    IPrintOemDriverUni *pHelp;   /* AddRef'ed copy of the plug-in's helper interface */

    /* settings */
    capt::EncodeOptions opt;
    DWORD polarity;              /* REG_POLARITY */
    DWORD bandOrder;             /* REG_BANDORDER */
    HPALETTE hpalDefault;
    int  autoInvert;             /* -1 unknown, 0/1 decided from the palette */

    /* job / page state */
    capt::PageAccumulator page;
    bool pageOpen;               /* a page is being collected and not yet emitted */
    bool sawStartPageCmd;        /* CmdStartPage callback observed (else heuristic page split) */
    bool carriedFromReset;       /* state moved in by ResetPDEV: StartDoc must not reset pageIndex */
    bool failed;                 /* a write failed: drop the rest of the document */
    int  pageIndex;              /* 0 = next page emitted carries a1 a1 + first-page init */
    int  nextY;                  /* end row of the previous band of this page (sequential position) */
    bool seqMode;                /* IPPARAMS.ptOffset found not page-relative: stack bands sequentially */

    void InitializeDDITable(DRVENABLEDATA *pded);
    void ReadSettings(HANDLE hPrinter);
    bool ResolveInvert(const BYTE *pColorTable, const BITMAPINFOHEADER *bih);

    void BeginDocument();
    void OpenPage();
    bool FlushPage(PDEVOBJ pdevobj);       /* encode + DrvWriteSpoolBuf the pending page */
    bool Write(PDEVOBJ pdevobj, const BYTE *p, size_t n);
};
typedef CLbpPdev *PLBPPDEV;

/* DDI hooks (prototypes in printoem.h) */
BOOL APIENTRY OEMStartDoc(SURFOBJ *pso, PWSTR pwszDocName, DWORD dwJobId);
BOOL APIENTRY OEMEndDoc(SURFOBJ *pso, FLONG fl);

/* IPrintOemUni2 implementation */
class CLbpOemUni2 : public IPrintOemUni2 {
public:
    CLbpOemUni2();
    virtual ~CLbpOemUni2();

    STDMETHODIMP QueryInterface(REFIID riid, LPVOID *ppvObj);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    STDMETHODIMP GetInfo(DWORD dwMode, PVOID pBuffer, DWORD cbSize, PDWORD pcbNeeded);
    STDMETHODIMP DevMode(DWORD dwMode, POEMDMPARAM pOemDMParam);
    STDMETHODIMP EnableDriver(DWORD DriverVersion, DWORD cbSize, PDRVENABLEDATA pded);
    STDMETHODIMP DisableDriver();
    STDMETHODIMP EnablePDEV(PDEVOBJ pdevobj, PWSTR pPrinterName, ULONG cPatterns, HSURF *phsurfPatterns,
                            ULONG cjGdiInfo, GDIINFO *pGdiInfo, ULONG cjDevInfo, DEVINFO *pDevInfo,
                            DRVENABLEDATA *pded, OUT PDEVOEM *pDevOem);
    STDMETHODIMP DisablePDEV(PDEVOBJ pdevobj);
    STDMETHODIMP ResetPDEV(PDEVOBJ pdevobjOld, PDEVOBJ pdevobjNew);

    STDMETHODIMP PublishDriverInterface(IUnknown *pIUnknown);
    STDMETHODIMP GetImplementedMethod(PSTR pMethodName);
    STDMETHODIMP DriverDMS(PVOID pDevObj, PVOID pBuffer, DWORD cbSize, PDWORD pcbNeeded);
    STDMETHODIMP CommandCallback(PDEVOBJ pdevobj, DWORD dwCallbackID, DWORD dwCount, PDWORD pdwParams, OUT INT *piResult);
    STDMETHODIMP ImageProcessing(PDEVOBJ pdevobj, PBYTE pSrcBitmap, PBITMAPINFOHEADER pBitmapInfoHeader,
                                 PBYTE pColorTable, DWORD dwCallbackID, PIPPARAMS pIPParams, OUT PBYTE *ppbResult);
    STDMETHODIMP FilterGraphics(PDEVOBJ pdevobj, PBYTE pBuf, DWORD dwLen);
    STDMETHODIMP Compression(PDEVOBJ pdevobj, PBYTE pInBuf, PBYTE pOutBuf, DWORD dwInLen, DWORD dwOutLen, OUT INT *piResult);
    STDMETHODIMP HalftonePattern(PDEVOBJ pdevobj, PBYTE pHTPattern, DWORD dwHTPatternX, DWORD dwHTPatternY,
                                 DWORD dwHTNumPatterns, DWORD dwCallbackID, PBYTE pResource, DWORD dwResourceSize);
    STDMETHODIMP MemoryUsage(PDEVOBJ pdevobj, POEMMEMORYUSAGE pMemoryUsage);
    STDMETHODIMP TTYGetInfo(PDEVOBJ pdevobj, DWORD dwInfoIndex, PVOID pOutputBuf, DWORD dwSize, DWORD *pcbcNeeded);
    STDMETHODIMP DownloadFontHeader(PDEVOBJ pdevobj, PUNIFONTOBJ pUFObj, OUT DWORD *pdwResult);
    STDMETHODIMP DownloadCharGlyph(PDEVOBJ pdevobj, PUNIFONTOBJ pUFObj, HGLYPH hGlyph, PDWORD pdwWidth, OUT DWORD *pdwResult);
    STDMETHODIMP TTDownloadMethod(PDEVOBJ pdevobj, PUNIFONTOBJ pUFObj, OUT DWORD *pdwResult);
    STDMETHODIMP OutputCharStr(PDEVOBJ pdevobj, PUNIFONTOBJ pUFObj, DWORD dwType, DWORD dwCount, PVOID pGlyph);
    STDMETHODIMP SendFontCmd(PDEVOBJ pdevobj, PUNIFONTOBJ pUFObj, PFINVOCATION pFInv);
    STDMETHODIMP TextOutAsBitmap(SURFOBJ *pso, STROBJ *pstro, FONTOBJ *pfo, CLIPOBJ *pco, RECTL *prclExtra,
                                 RECTL *prclOpaque, BRUSHOBJ *pboFore, BRUSHOBJ *pboOpaque, POINTL *pptlOrg, MIX mix);
    STDMETHODIMP WritePrinter(PDEVOBJ pdevobj, PVOID pBuf, DWORD cbBuffer, PDWORD pcbWritten);

private:
    LONG m_cRef;
    IPrintOemDriverUni *m_pOEMHelp;
};

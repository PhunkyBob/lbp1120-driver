/*
 *  lbp1120rnd.cpp - IPrintOemUni2 render plug-in for the Canon LASER SHOT LBP-1120 (CAPT).
 *
 *  Byte flow (see README.md):
 *    GDI -> Unidrv (halftones to 1 bpp, bands) -> ImageProcessing(): bands copied into a
 *    page raster (PBM polarity, 1 = black) -> at the next CmdStartPage callback or at
 *    DrvEndDoc: capt::CompressPage + capt::AppendPageSequence -> DrvWriteSpoolBuf.
 *    Unidrv itself emits nothing: every GPD *Cmd is "" and DevBPP = 0 means the plug-in
 *    sends the raster. The spool stream therefore is exactly captsend's host->printer bytes.
 *
 *  Skeleton (COM object, class factory, DllGetClassObject, GetImplementedMethod, the
 *  ImageProcessing/EndDoc pattern) follows the Windows-driver-samples "bitmap" sample.
 *
 *  GPLv2 or later: links the CAPT encoder derived from capt-0.1 (see capt_encode.h).
 */
#include <initguid.h>
#include "lbp1120rnd.h"

#include <new>

/* ------------------------------------------------------------------ globals */
static long g_cComponents;
static long g_cServerLocks;

static const DRVFN s_aOemHookFuncs[] = {
    { INDEX_DrvStartDoc, (PFN)OEMStartDoc },
    { INDEX_DrvEndDoc,   (PFN)OEMEndDoc   },
};

void LbpTrace(const char *fmt, ...)
{
    char buf[512];
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "lbp1120rnd: ");
    va_list ap;
    va_start(ap, fmt);
    if (n > 0) _vsnprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

/* ------------------------------------------------------------------ CLbpPdev */
CLbpPdev::CLbpPdev()
    : pfnStartDoc(NULL), pfnEndDoc(NULL), pHelp(NULL), polarity(0), bandOrder(0),
      hpalDefault(NULL), autoInvert(-1), pageOpen(false), sawStartPageCmd(false),
      carriedFromReset(false), failed(false), pageIndex(0), nextY(0), seqMode(false)
{
}

CLbpPdev::~CLbpPdev()
{
    if (pHelp) { pHelp->Release(); pHelp = NULL; }
}

void CLbpPdev::InitializeDDITable(DRVENABLEDATA *pded)
{
    for (ULONG i = 0; i < pded->c; i++) {
        if (pded->pdrvfn[i].iFunc == INDEX_DrvStartDoc) pfnStartDoc = (PFN_DrvStartDoc)pded->pdrvfn[i].pfn;
        if (pded->pdrvfn[i].iFunc == INDEX_DrvEndDoc)   pfnEndDoc   = (PFN_DrvEndDoc)pded->pdrvfn[i].pfn;
    }
}

static DWORD ReadDword(HANDLE hPrinter, LPCWSTR name, DWORD def)
{
    DWORD type = 0, val = 0, needed = 0;
    if (hPrinter &&
        GetPrinterDataW(hPrinter, (LPWSTR)name, &type, (LPBYTE)&val, sizeof(val), &needed) == ERROR_SUCCESS &&
        type == REG_DWORD && needed == sizeof(DWORD))
        return val;
    return def;
}

void CLbpPdev::ReadSettings(HANDLE hPrinter)
{
    DWORD t = ReadDword(hPrinter, REG_TOPSKIP, 0);
    DWORD l = ReadDword(hPrinter, REG_LEFTSKIP, 0);
    opt.topskip  = (t <= 2000) ? (int)t : 0;
    opt.leftskip = (l / 8 + capt::LINE_SIZE + 4 <= 1024) ? (int)l : 0;
    polarity  = ReadDword(hPrinter, REG_POLARITY, 0);
    bandOrder = ReadDword(hPrinter, REG_BANDORDER, 0);
    LbpTrace("settings: topskip=%d leftskip=%d polarity=%lu bandorder=%lu\n",
             opt.topskip, opt.leftskip, polarity, bandOrder);
}

static int Luma(BYTE r, BYTE g, BYTE b) { return 30 * r + 59 * g + 11 * b; }

/* true -> band bit 1 is WHITE (needs inverting into PBM polarity). */
bool CLbpPdev::ResolveInvert(const BYTE *pColorTable, const BITMAPINFOHEADER *bih)
{
    if (polarity == 1) return true;
    if (polarity == 2) return false;
    if (autoInvert >= 0) return autoInvert != 0;

    int decided = -1;
    const char *how = "default (GDI 1bpp palette {black, white}, as in the WDK bitmap sample)";
    if (pColorTable && bih && bih->biBitCount == 1) {
        const RGBQUAD *q = (const RGBQUAD *)pColorTable;
        int l0 = Luma(q[0].rgbRed, q[0].rgbGreen, q[0].rgbBlue);
        int l1 = Luma(q[1].rgbRed, q[1].rgbGreen, q[1].rgbBlue);
        if (l0 != l1) { decided = (l1 > l0) ? 1 : 0; how = "band color table"; }
    }
    if (decided < 0 && hpalDefault) {
        PALETTEENTRY pe[2];
        if (GetPaletteEntries(hpalDefault, 0, 2, pe) == 2) {
            int l0 = Luma(pe[0].peRed, pe[0].peGreen, pe[0].peBlue);
            int l1 = Luma(pe[1].peRed, pe[1].peGreen, pe[1].peBlue);
            if (l0 != l1) { decided = (l1 > l0) ? 1 : 0; how = "device default palette"; }
        }
    }
    if (decided < 0) decided = 1;
    autoInvert = decided;
    LbpTrace("polarity: band bit 1 = %s (%s)\n", decided ? "white -> inverted" : "black", how);
    return decided != 0;
}

void CLbpPdev::BeginDocument()
{
    if (carriedFromReset) {          /* ResetDC in the middle of a document: keep counting */
        carriedFromReset = false;
        return;
    }
    page.Reset();
    pageOpen = false;
    failed = false;
    pageIndex = 0;
    seqMode = false;
}

void CLbpPdev::OpenPage()
{
    page.Reset();
    pageOpen = true;
    nextY = 0;
}

bool CLbpPdev::Write(PDEVOBJ pdevobj, const BYTE *p, size_t n)
{
    const size_t CHUNK = 65536;
    while (n > 0) {
        DWORD c = (DWORD)(n < CHUNK ? n : CHUNK), written = 0;
        HRESULT hr;
        if (pHelp) {
            hr = pHelp->DrvWriteSpoolBuf(pdevobj, (PVOID)p, c, &written);
        } else if (pdevobj->pDrvProcs && pdevobj->pDrvProcs->DrvWriteSpoolBuf) {
            written = pdevobj->pDrvProcs->DrvWriteSpoolBuf(pdevobj, (PVOID)p, c);
            hr = S_OK;
        } else {
            hr = E_FAIL;
        }
        if (FAILED(hr) || written != c) {
            LbpTrace("DrvWriteSpoolBuf failed (hr=0x%08lx, %lu/%lu)\n", (unsigned long)hr, written, c);
            return false;
        }
        p += c;
        n -= c;
    }
    return true;
}

bool CLbpPdev::FlushPage(PDEVOBJ pdevobj)
{
    if (!pageOpen) return true;
    pageOpen = false;
    if (failed) { page.Reset(); return false; }

    capt::Bytes out;
    std::string err;
    capt::EncodeStats st;
    bool ok;
    try {
        out.reserve(64 * 1024);
        ok = capt::EncodePageJob(page.GetRaster(), opt, pageIndex, out, &err, &st);
    } catch (const std::bad_alloc &) {
        ok = false;
        err = "out of memory";
    }
    if (!ok) {
        LbpTrace("page %d: encoder error: %s -- page dropped, rest of the document skipped\n", pageIndex, err.c_str());
        failed = true;
        page.Reset();
        return false;
    }
    LbpTrace("page %d: raster %dx%d bytes from %d band(s) -> %d CAPT bands, %zu stream bytes\n",
             pageIndex, page.WidthBytes(), page.Height(), page.Bands(), st.bands, out.size());
    page.Reset();
    if (!Write(pdevobj, out.empty() ? NULL : &out[0], out.size())) {
        failed = true;
        return false;
    }
    pageIndex++;
    return true;
}

/* ------------------------------------------------------------------ DDI hooks */
BOOL APIENTRY OEMStartDoc(SURFOBJ *pso, PWSTR pwszDocName, DWORD dwJobId)
{
    PDEVOBJ pdevobj = (PDEVOBJ)pso->dhpdev;
    PLBPPDEV pdev = (PLBPPDEV)pdevobj->pdevOEM;
    LbpTrace("DrvStartDoc job %lu\n", dwJobId);
    pdev->BeginDocument();
    return pdev->pfnStartDoc ? pdev->pfnStartDoc(pso, pwszDocName, dwJobId) : FALSE;
}

BOOL APIENTRY OEMEndDoc(SURFOBJ *pso, FLONG fl)
{
    PDEVOBJ pdevobj = (PDEVOBJ)pso->dhpdev;
    PLBPPDEV pdev = (PLBPPDEV)pdevobj->pdevOEM;
    if (fl & ED_ABORTDOC) {
        LbpTrace("DrvEndDoc: aborted, pending page discarded\n");
        pdev->page.Reset();
        pdev->pageOpen = false;
    } else {
        pdev->FlushPage(pdevobj);   /* last page of the document */
        LbpTrace("DrvEndDoc: %d page(s) emitted\n", pdev->pageIndex);
    }
    pdev->carriedFromReset = false;
    return pdev->pfnEndDoc ? pdev->pfnEndDoc(pso, fl) : FALSE;
}

/* ------------------------------------------------------------------ CLbpOemUni2 */
CLbpOemUni2::CLbpOemUni2() : m_cRef(1), m_pOEMHelp(NULL)
{
    InterlockedIncrement(&g_cComponents);
}

CLbpOemUni2::~CLbpOemUni2()
{
    if (m_pOEMHelp) { m_pOEMHelp->Release(); m_pOEMHelp = NULL; }
    InterlockedDecrement(&g_cComponents);
}

STDMETHODIMP CLbpOemUni2::QueryInterface(REFIID riid, LPVOID *ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown)            *ppv = static_cast<IUnknown *>(this);
    else if (riid == IID_IPrintOemUni2)  *ppv = static_cast<IPrintOemUni2 *>(this);
    else if (riid == IID_IPrintOemUni)   *ppv = static_cast<IPrintOemUni *>(this);
    else { *ppv = NULL; return E_NOINTERFACE; }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CLbpOemUni2::AddRef() { return (ULONG)InterlockedIncrement(&m_cRef); }

STDMETHODIMP_(ULONG) CLbpOemUni2::Release()
{
    ULONG c = (ULONG)InterlockedDecrement(&m_cRef);
    if (c == 0) delete this;
    return c;
}

STDMETHODIMP CLbpOemUni2::GetInfo(DWORD dwMode, PVOID pBuffer, DWORD cbSize, PDWORD pcbNeeded)
{
    if (!pcbNeeded || (dwMode != OEMGI_GETSIGNATURE && dwMode != OEMGI_GETVERSION)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return E_FAIL;
    }
    *pcbNeeded = sizeof(DWORD);
    if (cbSize < sizeof(DWORD) || !pBuffer) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return E_FAIL;
    }
    *(PDWORD)pBuffer = (dwMode == OEMGI_GETSIGNATURE) ? LBP_OEM_SIGNATURE : LBP_OEM_VERSION;
    return S_OK;
}

STDMETHODIMP CLbpOemUni2::DevMode(DWORD dwMode, POEMDMPARAM p)
{
    if (!p) { SetLastError(ERROR_INVALID_PARAMETER); return E_FAIL; }
    PLBPDEVMODE out = (PLBPDEVMODE)p->pOEMDMOut;
    switch (dwMode) {
    case OEMDM_SIZE:
        p->cbBufSize = sizeof(LBPDEVMODE);
        return S_OK;
    case OEMDM_DEFAULT:
    case OEMDM_CONVERT:
    case OEMDM_MERGE:
        if (!out) { SetLastError(ERROR_INVALID_PARAMETER); return E_FAIL; }
        out->dmOEMExtra.dwSize = sizeof(LBPDEVMODE);
        out->dmOEMExtra.dwSignature = LBP_OEM_SIGNATURE;
        out->dmOEMExtra.dwVersion = LBP_OEM_VERSION;
        return S_OK;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return E_FAIL;
}

STDMETHODIMP CLbpOemUni2::EnableDriver(DWORD DriverVersion, DWORD cbSize, PDRVENABLEDATA pded)
{
    UNREFERENCED_PARAMETER(DriverVersion);
    UNREFERENCED_PARAMETER(cbSize);
    pded->iDriverVersion = PRINTER_OEMINTF_VERSION;
    pded->c = _countof(s_aOemHookFuncs);
    pded->pdrvfn = (DRVFN *)s_aOemHookFuncs;
    return S_OK;
}

STDMETHODIMP CLbpOemUni2::DisableDriver()
{
    if (m_pOEMHelp) { m_pOEMHelp->Release(); m_pOEMHelp = NULL; }
    return S_OK;
}

STDMETHODIMP CLbpOemUni2::EnablePDEV(PDEVOBJ pdevobj, PWSTR pPrinterName, ULONG cPatterns, HSURF *phsurfPatterns,
                                     ULONG cjGdiInfo, GDIINFO *pGdiInfo, ULONG cjDevInfo, DEVINFO *pDevInfo,
                                     DRVENABLEDATA *pded, OUT PDEVOEM *pDevOem)
{
    UNREFERENCED_PARAMETER(pPrinterName);
    UNREFERENCED_PARAMETER(cPatterns);
    UNREFERENCED_PARAMETER(phsurfPatterns);
    UNREFERENCED_PARAMETER(cjGdiInfo);
    UNREFERENCED_PARAMETER(cjDevInfo);

    PLBPPDEV pdev = new (std::nothrow) CLbpPdev();
    if (!pdev) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return E_FAIL; }
    pdev->InitializeDDITable(pded);
    if (m_pOEMHelp) { pdev->pHelp = m_pOEMHelp; pdev->pHelp->AddRef(); }
    pdev->hpalDefault = pDevInfo ? pDevInfo->hpalDefault : NULL;
    pdev->ReadSettings(pdevobj->hPrinter);
    if (pGdiInfo)
        LbpTrace("EnablePDEV: %lux%lu px, %lu bpp, %lux%lu dpi\n", pGdiInfo->ulHorzRes, pGdiInfo->ulVertRes,
                 pGdiInfo->cBitsPixel, pGdiInfo->ulLogPixelsX, pGdiInfo->ulLogPixelsY);
    *pDevOem = (PDEVOEM)pdev;
    return S_OK;
}

STDMETHODIMP CLbpOemUni2::DisablePDEV(PDEVOBJ pdevobj)
{
    PLBPPDEV pdev = (PLBPPDEV)pdevobj->pdevOEM;
    if (pdev && pdev->pageOpen) LbpTrace("DisablePDEV: unflushed page discarded\n");
    delete pdev;
    return S_OK;
}

STDMETHODIMP CLbpOemUni2::ResetPDEV(PDEVOBJ pdevobjOld, PDEVOBJ pdevobjNew)
{
    PLBPPDEV o = (PLBPPDEV)pdevobjOld->pdevOEM;
    PLBPPDEV n = (PLBPPDEV)pdevobjNew->pdevOEM;
    if (o && n) {   /* ResetDC between pages: carry the document state over (no output here) */
        n->page = o->page;
        o->page.Reset();
        n->pageOpen = o->pageOpen;
        n->sawStartPageCmd = o->sawStartPageCmd;
        n->failed = o->failed;
        n->pageIndex = o->pageIndex;
        n->autoInvert = o->autoInvert;
        n->nextY = o->nextY;
        n->seqMode = o->seqMode;
        n->carriedFromReset = true;
        o->pageOpen = false;
    }
    return S_OK;
}

STDMETHODIMP CLbpOemUni2::PublishDriverInterface(IUnknown *pIUnknown)
{
    if (!m_pOEMHelp) {
        HRESULT hr = pIUnknown->QueryInterface(IID_IPrintOemDriverUni, (void **)&m_pOEMHelp);
        if (FAILED(hr)) { m_pOEMHelp = NULL; return E_FAIL; }
    }
    return S_OK;
}

STDMETHODIMP CLbpOemUni2::GetImplementedMethod(PSTR pMethodName)
{
    if (!pMethodName) return S_FALSE;
    if (!strcmp(pMethodName, "CommandCallback")) return S_OK;
    if (!strcmp(pMethodName, "ImageProcessing")) return S_OK;
    return S_FALSE;
}

STDMETHODIMP CLbpOemUni2::DriverDMS(PVOID, PVOID, DWORD, PDWORD) { return E_NOTIMPL; }

STDMETHODIMP CLbpOemUni2::CommandCallback(PDEVOBJ pdevobj, DWORD dwCallbackID, DWORD dwCount, PDWORD pdwParams, OUT INT *piResult)
{
    UNREFERENCED_PARAMETER(dwCount);
    UNREFERENCED_PARAMETER(pdwParams);
    if (piResult) *piResult = 0;
    PLBPPDEV pdev = (PLBPPDEV)pdevobj->pdevOEM;
    switch (dwCallbackID) {
    case CB_START_PAGE:
        /* Everything of the previous page has been rendered by now: emit it, then start
         * collecting this one. Deferring the emission (instead of doing it at CmdFF) keeps
         * the stream correct whatever order Unidrv uses for FF vs. the last band. */
        pdev->sawStartPageCmd = true;
        pdev->FlushPage(pdevobj);
        pdev->OpenPage();
        return S_OK;
    }
    return S_OK;
}

STDMETHODIMP CLbpOemUni2::ImageProcessing(PDEVOBJ pdevobj, PBYTE pSrcBitmap, PBITMAPINFOHEADER bih,
                                          PBYTE pColorTable, DWORD dwCallbackID, PIPPARAMS ip, OUT PBYTE *ppbResult)
{
    UNREFERENCED_PARAMETER(dwCallbackID);
    *ppbResult = NULL;
    PLBPPDEV pdev = (PLBPPDEV)pdevobj->pdevOEM;
    if (!bih || !ip) return E_FAIL;
    if (bih->biBitCount != 1 || bih->biPlanes != 1 || bih->biCompression != BI_RGB) {
        LbpTrace("ImageProcessing: unsupported band format %u bpp / %u planes / compression %lu\n",
                 bih->biBitCount, bih->biPlanes, bih->biCompression);
        return E_FAIL;
    }

    const int width = (int)bih->biWidth;
    const int height = bih->biHeight < 0 ? -(int)bih->biHeight : (int)bih->biHeight;
    const bool bottomUp = (bih->biHeight > 0) && (pdev->bandOrder == 1);
    int ox = ip->ptOffset.x, oy = ip->ptOffset.y;
    if (ox < 0 || oy < 0) {
        LbpTrace("ImageProcessing: negative band offset (%d,%d) clamped\n", ox, oy);
        if (ox < 0) ox = 0;
        if (oy < 0) oy = 0;
    }

    if (!pdev->pageOpen) {
        pdev->OpenPage();              /* no CmdStartPage seen yet: open implicitly */
    } else if (!pdev->sawStartPageCmd && oy == 0 && pdev->page.Bands() > 0) {
        pdev->FlushPage(pdevobj);      /* fallback page split when the GPD callback never fires */
        pdev->OpenPage();
    }

    /* IPPARAMS.ptOffset is taken as the band's position on the page. Safety net: if a later
     * band of the same page claims row 0 again, the offsets are not page-relative; from then
     * on stack the bands top to bottom in arrival order (what the WDK bitmap sample does). */
    if (!pdev->seqMode && pdev->sawStartPageCmd && oy == 0 && pdev->page.Bands() > 0 && pdev->nextY > 0) {
        LbpTrace("ImageProcessing: band offset (%d,%d) after row %d -> sequential band placement\n", ox, oy, pdev->nextY);
        pdev->seqMode = true;
    }
    if (pdev->seqMode) { ox = 0; oy = pdev->nextY; }

    capt::BandDesc b;
    b.bits = pSrcBitmap;
    b.stride = (size_t)(((width + 31) / 32) * 4);   /* DIB scanlines are DWORD aligned */
    b.widthPx = width;
    b.height = height;
    b.offsetX = ox;
    b.offsetY = oy;
    b.blank = ip->bBlankBand ? true : false;
    b.invert = pdev->ResolveInvert(pColorTable, bih);
    b.bottomUp = bottomUp;
    if (!b.blank && !pSrcBitmap) return E_FAIL;
    if (!pdev->page.AddBand(b)) {
        LbpTrace("ImageProcessing: AddBand failed (memory?)\n");
        return E_FAIL;
    }
    pdev->nextY = oy + height;
    *ppbResult = (PBYTE)(INT_PTR)TRUE;   /* we send the data ourselves (DevBPP = 0) */
    return S_OK;
}

STDMETHODIMP CLbpOemUni2::FilterGraphics(PDEVOBJ, PBYTE, DWORD) { return E_NOTIMPL; }
STDMETHODIMP CLbpOemUni2::Compression(PDEVOBJ, PBYTE, PBYTE, DWORD, DWORD, OUT INT *) { return E_NOTIMPL; }
STDMETHODIMP CLbpOemUni2::HalftonePattern(PDEVOBJ, PBYTE, DWORD, DWORD, DWORD, DWORD, PBYTE, DWORD) { return E_NOTIMPL; }
STDMETHODIMP CLbpOemUni2::MemoryUsage(PDEVOBJ, POEMMEMORYUSAGE) { return E_NOTIMPL; }
STDMETHODIMP CLbpOemUni2::TTYGetInfo(PDEVOBJ, DWORD, PVOID, DWORD, DWORD *) { return E_NOTIMPL; }
STDMETHODIMP CLbpOemUni2::DownloadFontHeader(PDEVOBJ, PUNIFONTOBJ, OUT DWORD *) { return E_NOTIMPL; }
STDMETHODIMP CLbpOemUni2::DownloadCharGlyph(PDEVOBJ, PUNIFONTOBJ, HGLYPH, PDWORD, OUT DWORD *) { return E_NOTIMPL; }
STDMETHODIMP CLbpOemUni2::TTDownloadMethod(PDEVOBJ, PUNIFONTOBJ, OUT DWORD *) { return E_NOTIMPL; }
STDMETHODIMP CLbpOemUni2::OutputCharStr(PDEVOBJ, PUNIFONTOBJ, DWORD, DWORD, PVOID) { return E_NOTIMPL; }
STDMETHODIMP CLbpOemUni2::SendFontCmd(PDEVOBJ, PUNIFONTOBJ, PFINVOCATION) { return E_NOTIMPL; }
STDMETHODIMP CLbpOemUni2::TextOutAsBitmap(SURFOBJ *, STROBJ *, FONTOBJ *, CLIPOBJ *, RECTL *, RECTL *,
                                          BRUSHOBJ *, BRUSHOBJ *, POINTL *, MIX) { return E_NOTIMPL; }
STDMETHODIMP CLbpOemUni2::WritePrinter(PDEVOBJ, PVOID, DWORD, PDWORD) { return E_NOTIMPL; }

/* ------------------------------------------------------------------ class factory */
class CLbpClassFactory : public IClassFactory {
public:
    CLbpClassFactory() : m_cRef(1) {}
    virtual ~CLbpClassFactory() {}

    STDMETHODIMP QueryInterface(REFIID riid, LPVOID *ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = static_cast<IClassFactory *>(this); AddRef(); return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release()
    {
        ULONG c = (ULONG)InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }
    STDMETHODIMP CreateInstance(IUnknown *pOuter, REFIID riid, LPVOID *ppv)
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (pOuter) return CLASS_E_NOAGGREGATION;
        CLbpOemUni2 *p = new (std::nothrow) CLbpOemUni2();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL bLock)
    {
        if (bLock) InterlockedIncrement(&g_cServerLocks);
        else InterlockedDecrement(&g_cServerLocks);
        return S_OK;
    }

private:
    LONG m_cRef;
};

/* ------------------------------------------------------------------ exports */
STDAPI DllCanUnloadNow(void)
{
    return (g_cComponents == 0 && g_cServerLocks == 0) ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID *ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (clsid != CLSID_OEMRENDER) return CLASS_E_CLASSNOTAVAILABLE;
    CLbpClassFactory *cf = new (std::nothrow) CLbpClassFactory();
    if (!cf) return E_OUTOFMEMORY;
    HRESULT hr = cf->QueryInterface(iid, ppv);
    cf->Release();
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(lpReserved);
    if (dwReason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hInst);
    return TRUE;
}

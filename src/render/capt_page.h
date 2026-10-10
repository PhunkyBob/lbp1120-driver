/*
 *  capt_page.h - assembles the 1-bpp bands Unidrv hands to IPrintOemUni::ImageProcessing
 *                into one page raster in PBM polarity (1 = black) for capt::CompressPage.
 *  No Unidrv / Win32 dependency, so test\encode_test.exe exercises it with simulated bands.
 *
 *  Part of the LBP-1120 CAPT Unidrv plugin. GPLv2 or later (see capt_encode.h).
 */
#pragma once

#include "capt_encode.h"

namespace capt {

struct BandDesc {
    const uint8_t *bits;     /* band scanlines (ignored when blank)                         */
    size_t         stride;   /* bytes per scanline (DIB rows are DWORD aligned)             */
    int            widthPx;  /* band width in pixels                                         */
    int            height;   /* band height in rows                                          */
    int            offsetX;  /* band origin on the page, pixels (IPPARAMS.ptOffset.x)        */
    int            offsetY;  /* band origin on the page, rows   (IPPARAMS.ptOffset.y)        */
    bool           blank;    /* IPPARAMS.bBlankBand: whole band is white, bits not valid     */
    bool           invert;   /* source bit 1 = WHITE (GDI/Unidrv 1bpp palette {black,white}) */
    bool           bottomUp; /* source scanline 0 is the band's BOTTOM row                    */
};

class PageAccumulator {
public:
    PageAccumulator() : widthBytes(0), height(0), bandCount(0) {}

    void Reset() { rows.clear(); widthBytes = 0; height = 0; bandCount = 0; }

    /* Copies one band into the page (clipped to MAX_SOURCE_LINE_BYTES, x >= 0, y >= 0).
     * Rows never touched stay white. Returns false only on allocation failure / bad args. */
    bool AddBand(const BandDesc &b);

    /* Raster view for CompressPage; valid until the next AddBand/Reset. */
    Raster GetRaster() const;

    int Bands() const { return bandCount; }
    int Height() const { return height; }
    int WidthBytes() const { return widthBytes; }

private:
    Bytes rows;        /* height * STRIDE bytes, 1 = black */
    int   widthBytes;  /* widest data seen (bytes) */
    int   height;      /* rows allocated */
    int   bandCount;
    static const int STRIDE = MAX_SOURCE_LINE_BYTES;
};

} /* namespace capt */

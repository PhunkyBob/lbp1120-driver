/*
 *  capt_page.cpp - band -> page raster assembly for the LBP-1120 CAPT Unidrv plugin.
 *  GPLv2 or later (see capt_encode.h).
 */
#include "capt_page.h"

#include <cstring>
#include <new>

namespace capt {

bool PageAccumulator::AddBand(const BandDesc &b)
{
    if (b.height <= 0 || b.widthPx <= 0) { bandCount++; return true; }
    if (!b.blank && (b.bits == 0 || b.stride < (size_t)((b.widthPx + 7) / 8))) return false;
    if (b.offsetY < 0 || b.offsetX < 0) return false;   /* not expected from Unidrv */

    const int bitShift = b.offsetX & 7;                 /* sub-byte X offset          */
    const int dstX0    = b.offsetX >> 3;                /* first destination byte     */
    if (dstX0 >= STRIDE) { bandCount++; return true; }  /* entirely right of the page */

    int bandBytes = (b.widthPx + 7) / 8;
    int needBytes = dstX0 + bandBytes + (bitShift ? 1 : 0);
    if (needBytes > STRIDE) needBytes = STRIDE;

    const int needRows = b.offsetY + b.height;
    try {
        if (needRows > height) {
            rows.resize((size_t)needRows * STRIDE, 0);  /* new rows are white */
            height = needRows;
        }
    } catch (const std::bad_alloc &) {
        return false;
    }
    if (needBytes > widthBytes) widthBytes = needBytes;
    bandCount++;

    if (b.blank) return true;                           /* white: rows already 0 (or keep prior data) */

    /* mask for the last partial source byte (pixels beyond widthPx are not page content) */
    const int tailBits = b.widthPx & 7;
    const uint8_t tailMask = tailBits ? (uint8_t)(0xff << (8 - tailBits)) : (uint8_t)0xff;

    for (int y = 0; y < b.height; y++) {
        const int srcRow = b.bottomUp ? (b.height - 1 - y) : y;
        const uint8_t *s = b.bits + (size_t)srcRow * b.stride;
        uint8_t *d = &rows[(size_t)(b.offsetY + y) * STRIDE];
        for (int i = 0; i < bandBytes; i++) {
            uint8_t v = s[i];
            if (b.invert) v = (uint8_t)~v;
            if (i == bandBytes - 1) v &= tailMask;
            if (bitShift == 0) {
                if (dstX0 + i < STRIDE) d[dstX0 + i] = v;
            } else {
                int k = dstX0 + i;
                if (k < STRIDE) d[k] = (uint8_t)((d[k] & (uint8_t)(0xff << (8 - bitShift))) | (v >> bitShift));
                if (k + 1 < STRIDE) d[k + 1] = (uint8_t)((d[k + 1] & (uint8_t)(0xff >> bitShift)) | (uint8_t)(v << (8 - bitShift)));
            }
        }
    }
    return true;
}

Raster PageAccumulator::GetRaster() const
{
    Raster r;
    r.bits = rows.empty() ? 0 : &rows[0];
    r.stride = STRIDE;
    r.widthBytes = widthBytes;
    r.height = height;
    return r;
}

} /* namespace capt */

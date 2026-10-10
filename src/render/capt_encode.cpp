/*
 *  capt_encode.cpp - Canon CAPT page encoder + job-stream builder.
 *
 *  Line-for-line port of capt-0.1 capt.c (compress_bitmap, get_line, last_difference,
 *  out_packet, out_packet_buf, write_command_packet_buf, print_page) by way of the
 *  byte-identical C# port lbp1120-x64\captsend\captsend.cs (class Compressor / CaptJob).
 *  The output is byte-identical to captsend --dry-run (checked by test\encode_test.exe).
 *
 *    Copyright (C) 2004 Nicolas Boichat <nicolas@boichat.ch>
 *    Some functions are copied from a printer driver for Samsung ML-85G
 *    laser printer, (C) Copyleft, 2000 Rildo Pragana <rpragana@acm.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */
#include "capt_encode.h"

#include <cstring>

namespace capt {

namespace {

struct Abort {
    const char *msg;
    explicit Abort(const char *m) : msg(m) {}
};

class Compressor {
public:
    Compressor(const Raster &r, const EncodeOptions &o)
        : src(r), opt(o), cbmbuf(0), bmoff(0), bandptr(0), bsize(0),
          cur(0), cbmp(0), csize(0), linecnt(0), bands(0)
    {
        memset(bmbuf, 0, sizeof(bmbuf));
        band.resize(65536 + 1024); /* +slack: capt.c writes, then checks bsize > 65535 */
    }

    void Run(std::vector<Bytes> &result);
    int Bands() const { return bands; }
    int LinesConsumed() const { return linecnt; }

private:
    const Raster       &src;
    const EncodeOptions opt;

    /* capt.c: unsigned char bmbuf[2][1024]; bmptr[x] = bmbuf[x] + leftskip/8 */
    uint8_t bmbuf[2][1024];
    int     cbmbuf;
    int     bmoff;

    Bytes   band;
    int     bandptr, bsize;

    std::vector<Bytes> cbm;   /* each 65536 bytes, as capt.c's malloc'ed cbm[] */
    uint8_t *cur;             /* cbm[ccbm] */
    int      cbmp;            /* write index into cur */
    int      csize;
    int      linecnt;
    int      bands;

    void NewPacketBuffer()
    {
        cbm.push_back(Bytes(65536, 0));
        cur = &cbm.back()[0];
        cbmp = 0;
    }

    /* capt.c get_line() with the stdio file replaced by the in-memory raster. Row r of the
     * PBM stream after the topskip seek is page row (r + topskip). */
    void GetLine()
    {
        cbmbuf = (cbmbuf == 1) ? 0 : 1;
        memset(bmbuf[cbmbuf], 0, 800);
        if (linecnt < (src.height - opt.topskip)) {
            const uint8_t *row = src.bits + (size_t)(linecnt + opt.topskip) * src.stride;
            memcpy(bmbuf[cbmbuf], row, (size_t)src.widthBytes); /* widthBytes <= 800 (checked) */
        }
        bmoff = opt.leftskip / 8;
        linecnt++;
    }

    int LastDifference() const
    {
        int diff = -1;
        const uint8_t *p1 = bmbuf[cbmbuf] + bmoff;
        const uint8_t *p2 = bmbuf[(cbmbuf == 1) ? 0 : 1] + bmoff;
        for (int i = 0; i < LINE_SIZE; i++)
            if (p1[i] != p2[i]) diff = i;
        return diff;
    }

    void OutPacketBuf(int cnt, const uint8_t *s)
    {
        if (bandptr + cnt > (int)band.size()) throw Abort("Error, band too big.");
        memcpy(&band[bandptr], s, (size_t)cnt);
        bandptr += cnt;
        bsize += cnt;
        if (bsize > 65535) throw Abort("Error, band too big.");
    }

    int OutPacket(int cnt, uint8_t c1, uint8_t c2, uint8_t c3, uint8_t c4)
    {
        if (cnt == 0) {
            if (c1 == 1) { /* band end */
                if (csize + bsize > 65500) OutPacket(0, 0, 0, 0, 0);
                /* A band larger than a fresh packet's room would overrun the 65536-byte packet
                 * buffer (capt.c has the same overflow). Reject it instead; valid pages never
                 * get here, so the emitted stream is unchanged. */
                if (csize + bsize > 65500) throw Abort("Error, band too big for one packet.");
                csize += bsize;
                bands++;
                memcpy(cur + cbmp, &band[0], (size_t)bsize);
                cbmp += bsize;
                bandptr = 0;
                bsize = 0;
            } else if (c1 == 0) { /* flush packet */
                if (c2 == 1) cur[cbmp++] = 0x42; /* last packet (not counted in csize, as in capt.c) */
                cur[2] = (uint8_t)(csize & 0x00FF);
                cur[3] = (uint8_t)((csize & 0xFF00) >> 8);
                if (cbm.size() >= 99) throw Abort("Too much packets to send. Aborting.");
                if (c2 != 1) {
                    NewPacketBuffer();
                    csize = 4;
                    cur[cbmp++] = 0xa0; cur[cbmp++] = 0xc0; cur[cbmp++] = 0x00; cur[cbmp++] = 0x00;
                }
            }
            return 1;
        }
        switch (cnt) {
        case 4: band[bandptr++] = c1; band[bandptr++] = c2; band[bandptr++] = c3; band[bandptr++] = c4; break;
        case 3: band[bandptr++] = c1; band[bandptr++] = c2; band[bandptr++] = c3; break;
        case 2: band[bandptr++] = c1; band[bandptr++] = c2; break;
        case 1: band[bandptr++] = c1; break;
        default: throw Abort("Error, wrong packet size...");
        }
        bsize += cnt;
        if (bsize > 65535) throw Abort("Error, band too big...");
        return 0;
    }
};

inline uint8_t U8(int v) { return (uint8_t)(v & 0xff); }

void Compressor::Run(std::vector<Bytes> &result)
{
    NewPacketBuffer();

    /* capt.c: if (topskip) bitmap_seek(bmwidth * topskip); -- folded into GetLine()'s row index.
     * (capt.c reads the skipped bytes into bmbuf[0]; that buffer is cleared by the 2nd
     *  get_line() before it is ever compared, so the output does not depend on it.) */
    GetLine();
    const uint8_t *cl = bmbuf[cbmbuf] + bmoff; /* cline */
    int diff = LINE_SIZE;                      /* The first line must be completly sent */

    csize = 4;
    cur[cbmp++] = 0xa0; cur[cbmp++] = 0xc0; cur[cbmp++] = 0x00; cur[cbmp++] = 0x00;

    uint8_t c1, c2;
    for (int bandNo = 0; linecnt < LINES_BY_PAGE; bandNo++) {
        OutPacket(1, 0x40, 0, 0, 0);
        int cnt = ((LINES_BY_PAGE - linecnt) > ROWS_BY_BAND) ? ROWS_BY_BAND : LINES_BY_PAGE - linecnt;

        for (;;) {
            int rep = 0;
            int state = 0;
            int lpos;
            for (lpos = 0; lpos < diff; lpos++) {
                if ((cl[lpos] == cl[lpos + 1]) && ((lpos + 1) < diff)) {
                    if (state == -1) {
                        if (rep == 255) {
                            c1 = U8(0xa0 | (rep >> 3));
                            c2 = U8(0x80 | (rep & 0x07) << 3);
                            OutPacket(3, c1, c2, cl[lpos], 0);
                            rep = 0;
                            state = 0;
                        } else {
                            rep++;
                        }
                    } else {
                        state = -1;
                        rep = 2;
                    }
                } else { /* lpos and lpos+1 different */
                    if (state == 0) {
                        rep = 1;
                        const uint8_t *c = cl + lpos; /* c = &(cline[lpos]) */
                        while (rep < 255) {
                            if (lpos + rep + 2 >= diff) { /* End of line */
                                rep += diff - (lpos + rep);
                                break;
                            } else if ((c[rep] != c[rep + 1]) || (c[rep + 1] != c[rep + 2])) {
                                rep++;
                            } else {
                                break;
                            }
                        }
                        if (rep > 255) rep = 255;

                        lpos += rep - 1; /* lpos is incremented later */
                        if (rep < 8) {
                            c1 = U8(rep << 3);
                            OutPacket(1, c1, 0, 0, 0);
                        } else {
                            c1 = U8(0xa0 | (rep >> 3));
                            c2 = U8(0xc0 | ((rep & 0x07) << 3));
                            OutPacket(2, c1, c2, 0, 0);
                        }
                        OutPacketBuf(rep, c);
                        rep = 0;
                    } else { /* state == -1 */
                        if (rep < 8) {
                            if (((cl[lpos + 1] != cl[lpos + 2]) || ((lpos + 2) >= diff)) && ((lpos + 1) < diff)) {
                                c1 = U8(0xc1 | (rep << 3));
                                OutPacket(3, c1, cl[lpos], cl[lpos + 1], 0);
                                lpos++;
                            } else {
                                c1 = U8(0x40 | (rep << 3));
                                OutPacket(2, c1, cl[lpos], 0, 0);
                            }
                        } else {
                            if (((cl[lpos + 1] != cl[lpos + 2]) || ((lpos + 2) >= diff)) && ((lpos + 1) < diff)) {
                                if (((cl[lpos + 2] != cl[lpos + 3]) || ((lpos + 3) >= diff)) && ((lpos + 2) < diff)) {
                                    c1 = U8(0xa0 | (rep >> 3));
                                    c2 = U8(0x02 | ((rep & 0x07) << 3));
                                    OutPacket(3, c1, c2, cl[lpos], 0);
                                    OutPacket(2, cl[lpos + 1], cl[lpos + 2], 0, 0);
                                    lpos += 2;
                                } else {
                                    c1 = U8(0xa0 | (rep >> 3));
                                    c2 = U8(0x01 | ((rep & 0x07) << 3));
                                    OutPacket(4, c1, c2, cl[lpos], cl[lpos + 1]);
                                    lpos++;
                                }
                            } else {
                                c1 = U8(0xa0 | (rep >> 3));
                                c2 = U8(0x80 | (rep & 0x07) << 3);
                                OutPacket(3, c1, c2, cl[lpos], 0);
                            }
                        }
                        state = 0;
                    }
                }
            }

            if (diff < LINE_SIZE) OutPacket(1, 0x41, 0, 0, 0);

            GetLine();
            cl = bmbuf[cbmbuf] + bmoff;

            if (cnt > 0) {
                diff = LastDifference() + 1;
            } else {
                diff = LINE_SIZE;
                break;
            }
            cnt--;
        }
        OutPacket(1, 0x40, 0, 0, 0);
        OutPacket(0, 1, 0, 0, 0); /* Flush band */
        (void)bandNo;
    }
    OutPacket(0, 0, 1, 0, 0); /* Flush packet and set size */

    result.clear();
    for (size_t i = 0; i < cbm.size(); i++) {
        Bytes &p = cbm[i];
        int size = p[2] | (p[3] << 8);
        if (opt.endMarkerInSize && i == cbm.size() - 1) {
            size++;
            p[2] = (uint8_t)(size & 0xff);
            p[3] = (uint8_t)((size >> 8) & 0xff);
        }
        result.push_back(Bytes(p.begin(), p.begin() + size));
    }
}

} /* anonymous namespace */

bool CompressPage(const Raster &page, const EncodeOptions &opt,
                  std::vector<Bytes> &packets, std::string *err, EncodeStats *stats)
{
    packets.clear();
    if (opt.topskip < 0 || opt.leftskip < 0) {
        if (err) *err = "topskip and leftskip must be >= 0";
        return false;
    }
    if (opt.leftskip / 8 + LINE_SIZE + 4 > 1024) {
        if (err) *err = "leftskip too large";
        return false;
    }
    if (page.widthBytes < 0 || page.widthBytes > MAX_SOURCE_LINE_BYTES || page.height < 0 ||
        (page.height > 0 && (page.bits == 0 || page.stride < (size_t)page.widthBytes))) {
        if (err) *err = "invalid raster (width must be <= 800 bytes / 6400 px)";
        return false;
    }
    try {
        Compressor c(page, opt);
        c.Run(packets);
        if (stats) { stats->bands = c.Bands(); stats->linesConsumed = c.LinesConsumed(); }
    } catch (const Abort &a) {
        packets.clear();
        if (err) *err = a.msg;
        return false;
    } catch (const std::bad_alloc &) {
        packets.clear();
        if (err) *err = "out of memory";
        return false;
    }
    return true;
}

void AppendCommand(Bytes &out, uint8_t one, uint8_t two, const uint8_t *payload, size_t len)
{
    out.push_back(one);
    out.push_back(two);
    out.push_back((uint8_t)(0x04 + len)); /* capt.c: buffer[2] = 0x04 + len; buffer[3] = 0x00; */
    out.push_back(0x00);
    for (size_t i = 0; i < len; i++) out.push_back(payload[i]);
}

void AppendPageSequence(Bytes &out, int pageIndex, const std::vector<Bytes> &packets)
{
    static const uint8_t a5e0[8] = { 0xee, 0xdb, 0xea, 0xad, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t a0d0[34] = {
        0x00, 0x00, 0xa4, 0x01, 0x02, 0x01, 0x00, 0x00, 0x1f, 0x1f,
        0x1f, 0x1f, 0x00, 0x11, 0x03, 0x01, 0x01, 0x01, 0x02, 0x00,
        0x00, 0x00, 0x70, 0x00, 0x78, 0x00, 0x50, 0x02, 0x7a, 0x1a,
        0x60, 0x13, 0x67, 0x1b };

    if (pageIndex == 0)
        AppendCommand(out, 0xa1, 0xa1);      /* reply[0..1] must be a1 a1 */

    /* waitforpaper(): repeated by the language monitor until reply #2 byte[10] == 0xfd */
    AppendCommand(out, 0xa0, 0xa0);
    AppendCommand(out, 0xa1, 0xa0);

    if (pageIndex == 0) {                    /* First page initialization */
        AppendCommand(out, 0xa0, 0xa2);
        AppendCommand(out, 0xa0, 0xe0);
        AppendCommand(out, 0xa1, 0xa0);
        AppendCommand(out, 0xa4, 0xe0);
        AppendCommand(out, 0xa5, 0xe0, a5e0, sizeof(a5e0));
        AppendCommand(out, 0xa0, 0xe0);
        AppendCommand(out, 0xa0, 0xa0);
        AppendCommand(out, 0xa1, 0xa0);
        AppendCommand(out, 0xa0, 0xe0);
    }

    AppendCommand(out, 0xa0, 0xd0, a0d0, sizeof(a0d0));
    AppendCommand(out, 0xa0, 0xe0);
    AppendCommand(out, 0xa1, 0xd0);
    AppendCommand(out, 0xa0, 0xe0);
    AppendCommand(out, 0xa0, 0xa0);
    AppendCommand(out, 0xa1, 0xa0);

    for (size_t i = 0; i < packets.size(); i++) {
        AppendCommand(out, 0xa0, 0xe0);      /* gate: repeated by the LM while reply byte[4] & 0x08 */
        out.insert(out.end(), packets[i].begin(), packets[i].end()); /* a0 c0 <len> data */
    }

    AppendCommand(out, 0xa0, 0xe0);
    AppendCommand(out, 0xa2, 0xd0);
    AppendCommand(out, 0xa0, 0xe0);
    AppendCommand(out, 0xa1, 0xa0);
}

bool EncodePageJob(const Raster &page, const EncodeOptions &opt, int pageIndex,
                   Bytes &out, std::string *err, EncodeStats *stats)
{
    std::vector<Bytes> packets;
    if (!CompressPage(page, opt, packets, err, stats)) return false;
    AppendPageSequence(out, pageIndex, packets);
    return true;
}

} /* namespace capt */

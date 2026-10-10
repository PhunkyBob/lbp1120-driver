/*
 *  capt_encode.h - Canon CAPT (LBP-810 / LASER SHOT LBP-1120) page encoder and
 *                  host->printer job-stream builder. No Unidrv / Win32 dependency.
 *
 *  Derived from the Linux Canon CAPT driver capt-0.1 (capt.c / capt.h):
 *    Copyright (C) 2004 Nicolas Boichat <nicolas@boichat.ch>
 *    Some functions are copied from a printer driver for Samsung ML-85G
 *    laser printer, (C) Copyleft, 2000 Rildo Pragana <rpragana@acm.org>
 *  via its byte-identical C# port captsend.cs (lbp1120-x64\captsend).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details (captsend\COPYING).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace capt {

/* capt.h */
const int LINE_SIZE     = 592;   /* bytes per encoded line = 4736 px @ 600 dpi */
const int LINES_BY_PAGE = 6968;  /* lines encoded per page                      */
const int ROWS_BY_BAND  = 104;   /* "cnt" per band: each band carries 105 lines */

/* capt.c reads at most 800 bytes of a bitmap line (6400 px); wider input garbles every
 * line in capt.c, so this port refuses it instead of reproducing the garbage. */
const int MAX_SOURCE_LINE_BYTES = 800;

typedef std::vector<uint8_t> Bytes;

/* A 1-bpp page raster in PBM polarity: bit = 1 means BLACK, MSB = leftmost pixel,
 * row 0 = top of the page. Rows >= height and bytes >= widthBytes read as white (0),
 * exactly like capt.c's zero-filled line buffer at EOF / beyond the bitmap width. */
struct Raster {
    const uint8_t *bits;       /* may be NULL when height == 0 */
    size_t         stride;     /* bytes between consecutive rows */
    int            widthBytes; /* valid bytes in a row (<= MAX_SOURCE_LINE_BYTES) */
    int            height;     /* rows */
};

struct EncodeOptions {
    int  topskip;          /* rows skipped at the top    (capt.c -t, captsend --topskip,  default 0) */
    int  leftskip;         /* pixels skipped at the left (capt.c -l, captsend --leftskip, default 0; only leftskip/8 is used) */
    bool endMarkerInSize;  /* NON-faithful captsend --end-marker: also send the trailing 0x42. Default false. */
    EncodeOptions() : topskip(0), leftskip(0), endMarkerInSize(false) {}
};

struct EncodeStats {
    int bands;          /* bands flushed */
    int linesConsumed;  /* capt.c linecnt at the end (LINES_BY_PAGE + 1) */
    EncodeStats() : bands(0), linesConsumed(0) {}
};

/* capt.c compress_bitmap(): compresses ONE page into the list of "a0 c0" data packets,
 * each trimmed to the length in its own header (= exactly the bytes print_page() writes).
 * Returns false (and sets *err) on capt.c's abort conditions ("band too big",
 * "Too much packets") or invalid options. */
bool CompressPage(const Raster &page, const EncodeOptions &opt,
                  std::vector<Bytes> &packets, std::string *err, EncodeStats *stats = 0);

/* capt.c write_command_packet_buf(): [one][two][4+len][0x00][payload] */
void AppendCommand(Bytes &out, uint8_t one, uint8_t two, const uint8_t *payload = 0, size_t len = 0);

/* The host->printer bytes of capt.c print_page(pageIndex) for the given data packets, i.e.
 * every command and data byte in write order, assuming the printer answers "ready" to the
 * first a0 e0 gate poll and "paper present" to the first a0 a0/a1 a0 pair. The polls that a
 * real printer may require to be REPEATED (paper wait, a0 e0 gate before each a0 c0 packet)
 * appear exactly once; repeating them is the language monitor's job.
 *   pageIndex == 0 : a1 a1 + paper wait + first-page initialisation (this is what captsend sends)
 *   pageIndex  > 0 : capt.c's sequence for the following pages (UNVERIFIED on hardware) */
void AppendPageSequence(Bytes &out, int pageIndex, const std::vector<Bytes> &packets);

/* Convenience: CompressPage + AppendPageSequence. */
bool EncodePageJob(const Raster &page, const EncodeOptions &opt, int pageIndex,
                   Bytes &out, std::string *err, EncodeStats *stats = 0);

} /* namespace capt */

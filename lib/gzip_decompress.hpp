/*
 * gzip_decompress.c - decompress with a gzip wrapper
 *
 * Originally public domain; changes after 2016-09-07 are copyrighted.
 *
 * Copyright 2016 Eric Biggers
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "gzip_constants.h"
#include "unaligned.h"

#include "libdeflate.h"
#include <vector>
#include <thread>

#include "deflate_decompress.hpp" //FIXME

template<typename T>
bool
is_set(T word, T flag)
{
    return word & flag != T{ 0 };
}

LIBDEFLATEAPI enum libdeflate_result
libdeflate_gzip_decompress(struct libdeflate_decompressor* d,
                           const byte* in,
                           size_t in_nbytes,
                           byte* out,
                           size_t out_nbytes_avail,
                           size_t* actual_out_nbytes_ret,
                           unsigned nthreads,
                           size_t skip,
                           size_t until)
{
    const byte* in_next = in;
    const byte* const in_end = in_next + in_nbytes;
    byte flg;
    //	size_t actual_out_nbytes;
    enum libdeflate_result result;

    if (in_nbytes < GZIP_MIN_OVERHEAD)
        return LIBDEFLATE_BAD_DATA;

    /* ID1 */
    if (*in_next++ != GZIP_ID1)
        return LIBDEFLATE_BAD_DATA;
    /* ID2 */
    if (*in_next++ != GZIP_ID2)
        return LIBDEFLATE_BAD_DATA;
    /* CM */
    if (*in_next++ != GZIP_CM_DEFLATE)
        return LIBDEFLATE_BAD_DATA;
    flg = *in_next++;
    /* MTIME */
    in_next += 4;
    /* XFL */
    in_next += 1;
    /* OS */
    in_next += 1;

    if (bool(flg & GZIP_FRESERVED))
        return LIBDEFLATE_BAD_DATA;

    /* Extra field */
    if (bool(flg & GZIP_FEXTRA)) {
        u16 xlen = get_unaligned_le16(in_next);
        in_next += 2;

        if (in_end - in_next < (u32)xlen + GZIP_FOOTER_SIZE)
            return LIBDEFLATE_BAD_DATA;

        in_next += xlen;
    }

    /* Original file name (zero terminated) */
    if (bool(flg & GZIP_FNAME)) {
        while (*in_next++ != byte(0) && in_next != in_end)
            ;
        if (in_end - in_next < GZIP_FOOTER_SIZE)
            return LIBDEFLATE_BAD_DATA;
    }

    /* File comment (zero terminated) */
    if (bool(flg & GZIP_FCOMMENT)) {
        while (*in_next++ != byte(0) && in_next != in_end)
            ;
        if (in_end - in_next < GZIP_FOOTER_SIZE)
            return LIBDEFLATE_BAD_DATA;
    }

    /* CRC16 for gzip header */
    if (bool(flg & GZIP_FHCRC)) {
        in_next += 2;
        if (in_end - in_next < GZIP_FOOTER_SIZE)
            return LIBDEFLATE_BAD_DATA;
    }

    nthreads = std::min(1 + unsigned(in_nbytes >> 21), nthreads);
    if (nthreads <= 1) { // FIXME
    } else {
        PRINT_DEBUG("Using %u threads\n", nthreads);

        std::vector<std::thread> threads;
        std::vector<DeflateThreadBase*> deflate_threads(nthreads);

        size_t n_ready = 0;
        std::condition_variable ready;
        std::mutex ready_mtx;

        threads.reserve(nthreads);

        // FIXME: the gzip header code is duplicated inside the InputStream
        // This section of code test that. The goal is to parse them from DeflateThread to handle multipart gzip file
        InputStream in_stream2(in, in_end - in);
        bool headerok = in_stream2.consume_header();
        assert(headerok);
        assert(in_stream2.position_bits() == 8 * size_t(in_next - in));

        InputStream in_stream(in_next, in_end - GZIP_FOOTER_SIZE - in_next);

        size_t in_size = in_end - GZIP_FOOTER_SIZE - in_next;

        // Sections of file decompressed sequentially
        size_t max_section_size = nthreads * (32ull << 20); // 32MB per thread
        size_t section_size = std::min(max_section_size, in_size);
        size_t n_sections = (in_size + section_size - 1) / section_size;
        section_size = in_size / n_sections;

        // Section are chunked to nethreads
        size_t chunk_size = section_size / nthreads;
        // The first thread is working with resolved context so its faster
        size_t first_chunk_size = chunk_size + (1UL << 22); // FIXME: ratio instead of delta
        chunk_size = (nthreads * chunk_size - first_chunk_size) / (nthreads - 1);

        for (unsigned chunk_idx = 0; chunk_idx < nthreads; chunk_idx++) {
            if (chunk_idx == 0) {

                threads.emplace_back([&]() {
                    DeflateThreadFirstBlock deflate_thread(in_stream);
                    PRINT_DEBUG("chunk 0 is %p\n", (void*)&deflate_thread);
                    {
                        std::unique_lock<std::mutex> lock{ ready_mtx };
                        deflate_threads[0] = &deflate_thread;
                        n_ready++;
                        ready.notify_all();

                        while (n_ready != nthreads)
                            ready.wait(lock);
                    }

                    const byte* last_unmapped = details::round_up<details::huge_page_size>(in);

                    // First chunk of first section: no context needed
                    deflate_thread.go(0);

                    // First chunks of next sections get their contexts from the last chunk of the previous section
                    auto& prev_chunk = *deflate_threads[nthreads - 1];
                    for (unsigned section_idx = 1; section_idx < n_sections; section_idx++) {
                        // Get the context and position of the first block of the section
                        size_t resume_bitpos;
                        { // Synchronization point
                            auto ctx = prev_chunk.get_context();
                            deflate_thread.set_initial_context(ctx.first);
                            resume_bitpos = ctx.second;
                        }

                        // Unmmap the part previously decompressed (free RSS, usefull for large files)
                        const byte* unmap_end = details::round_down<details::huge_page_size>(in_next + resume_bitpos / 8);
                        sys::check_ret(munmap(const_cast<byte*>(last_unmapped), unmap_end - last_unmapped), "munmap");
                        last_unmapped = unmap_end;

                        PRINT_DEBUG("%p chunk 0 of section %u: [%lu, TBD[\n", (void*)&deflate_thread, section_idx, resume_bitpos);
                        assert(resume_bitpos >= (section_idx - 1) * section_size * 8);
                        deflate_thread.go(resume_bitpos);
                    }
                });
            } else {
                threads.emplace_back([&, chunk_idx]() {
                    DeflateThreadRandomAccess deflate_thread{ in_stream };
                    PRINT_DEBUG("chunk %u is %p\n", chunk_idx, (void*)&deflate_thread);
                    {
                        std::unique_lock<std::mutex> lock{ ready_mtx };
                        deflate_threads[chunk_idx] = &deflate_thread;
                        n_ready++;
                        ready.notify_all();

                        while (n_ready != nthreads)
                            ready.wait(lock);

                        deflate_thread.set_upstream(deflate_threads[chunk_idx - 1]);
                    }

                    const size_t chunk_offset_start = first_chunk_size + chunk_size * (chunk_idx - 1);
                    const size_t chunk_offset_stop = first_chunk_size + chunk_size * chunk_idx;

                    for (unsigned section_idx = 0; section_idx < n_sections; section_idx++) {
                        const size_t section_offset = section_idx * section_size;
                        const size_t start = section_offset + chunk_offset_start;
                        const size_t stop = section_offset + chunk_offset_stop;

                        if (chunk_idx == nthreads - 1) { // The last chunk must be bounded to the section end
                            PRINT_DEBUG("%p chunk %u of section %u [%lu, %lu[\n", (void*)&deflate_thread, chunk_idx, section_idx, start * 8, stop * 8);
                            deflate_thread.set_end_block(stop * 8);
                        } else {
                            PRINT_DEBUG("%p chunk %u of section %u [%lu, TBD[\n", (void*)&deflate_thread, chunk_idx, section_idx, start * 8);
                        }

                        deflate_thread.go(start * 8);
                    }

                    if (chunk_idx == nthreads - 1)    // No one will need the context from the last chunk of the last section
                        deflate_thread.get_context(); // Releasing it will enable the imediate destruction of the DeflateThread
                });
            }
        }

        for (auto& thread : threads)
            thread.join();

        result = LIBDEFLATE_SUCCESS;
    }

    if (result != LIBDEFLATE_SUCCESS)
        return result;

    //	if (actual_out_nbytes_ret)
    //		actual_out_nbytes = *actual_out_nbytes_ret;
    //	else
    //		actual_out_nbytes = out_nbytes_avail;

    in_next = in_end - GZIP_FOOTER_SIZE;

// Rayan: skipping those checks as we may not be decompressing the whole file
#if 0
	/* CRC32 */
 	if (libdeflate_crc32(0, out, actual_out_nbytes) !=
 	    get_unaligned_le32(in_next))
 		return LIBDEFLATE_BAD_DATA;
	in_next += 4;

	/* ISIZE */
	if ((u32)actual_out_nbytes != get_unaligned_le32(in_next))
		return LIBDEFLATE_BAD_DATA;
#endif

    return LIBDEFLATE_SUCCESS;
}

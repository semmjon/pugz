/*
 * deflate_decompress.c - a decompressor for DEFLATE
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
 *
 * ---------------------------------------------------------------------------
 *
 * This is a highly optimized DEFLATE decompressor.  When compiled with gcc on
 * x86_64, it decompresses data in about 52% of the time of zlib (48% if BMI2
 * instructions are available).  On other architectures it should still be
 * significantly faster than zlib, but the difference may be smaller.
 *
 * Why this is faster than zlib's implementation:
 *
 * - Word accesses rather than byte accesses when reading input
 * - Word accesses rather than byte accesses when copying matches
 * - Faster Huffman decoding combined with various DEFLATE-specific tricks
 * - Larger bitbuffer variable that doesn't need to be filled as often
 * - Other optimizations to remove unnecessary branches
 * - Only full-buffer decompression is supported, so the code doesn't need to
 *   support stopping and resuming decompression.
 * - On x86_64, compile a version of the decompression routine using BMI2
 *   instructions and use it automatically at runtime when supported.
 */

#include <climits>
#include <pmmintrin.h>

#include <mutex>
#include <condition_variable>
#include <atomic>

#include "common/exceptions.hpp"
#include "memory.hpp"
#include "assert.hpp"

#include "input_stream.hpp"
#include "decompressor.hpp"

#include "libdeflate.h"

/** The main class for the deflate decompression
 * Holds the decompressor tables and the input stream, but not the deflate window or output buffers
 */
class DeflateParser
{
  public:
    DeflateParser(const InputStream& in_stream)
      : _in_stream(in_stream)
    {}

    enum class block_result : unsigned
    {
        SUCCESS = 0,              // Success, yet many work remaining
        LAST_BLOCK = 1,           // Last block had just been decoded
        CAUGHT_UP_DOWNSTREAM = 2, // Caught up downstream decoder
        FLUSH_FAIL = 3,           // Not enough space in the buffer
        INVALID_BLOCK_TYPE,
        INVALID_DYNAMIC_HT,
        INVALID_UNCOMPRESSED_BLOCK,
        INVALID_LITERAL,
        INVALID_MATCH,
        TOO_MUCH_INPUT,
        NOT_ENOUGH_INPUT,
        INVALID_PARSE,
    };

    static const char* block_result_to_cstr(block_result result) { return block_result_strings[static_cast<unsigned>(result)]; }

  protected:
    template<typename Window, typename Might = ShouldSucceed>
    __attribute__((noinline)) block_result do_block(Window& window, const Might& might_tag = {})
    {

        /* Starting to read the next block.  */
        if (unlikely(!_in_stream.ensure_bits<1 + 2 + 5 + 5 + 4>()))
            return block_result::NOT_ENOUGH_INPUT;

        /* BFINAL: 1 bit  */
        block_result success = _in_stream.pop_bits(1) ? block_result::LAST_BLOCK : block_result::SUCCESS;

        /* BTYPE: 2 bits  */
        libdeflate_decompressor* cur_d;
        switch (_in_stream.pop_bits(2)) {
            case DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN:
                if (Might::fail_if(!prepare_dynamic(might_tag)))
                    return block_result::INVALID_DYNAMIC_HT;
                cur_d = &_decompressor;
                break;

            case DEFLATE_BLOCKTYPE_UNCOMPRESSED:
                if (Might::fail_if(!do_uncompressed(window, might_tag)))
                    return block_result::INVALID_UNCOMPRESSED_BLOCK;

                return Might::succeed_if(window.notify_end_block(_in_stream)) ? success : block_result::INVALID_PARSE;

            case DEFLATE_BLOCKTYPE_STATIC_HUFFMAN:
                cur_d = &static_decompressor;
                break;

            default:
                return block_result::INVALID_BLOCK_TYPE;
        }

        /* Decompressing a Huffman block (either dynamic or static)  */
        PRINT_DEBUG_DECODING(fprintf(stderr, "trying to decode huffman block\n");)

        /* The main DEFLATE decode loop  */
        for (;;) {
            /* Decode a litlen symbol.  */
            _in_stream.ensure_bits<DEFLATE_MAX_LITLEN_CODEWORD_LEN>();
            // FIXME: entry should be const
            u32 entry = cur_d->u.litlen_decode_table[_in_stream.bits(LITLEN_TABLEBITS)];
            if (entry & HUFFDEC_SUBTABLE_POINTER) {
                /* Litlen subtable required (uncommon case)  */
                _in_stream.remove_bits(LITLEN_TABLEBITS);
                entry = cur_d->u.litlen_decode_table[((entry >> HUFFDEC_RESULT_SHIFT) & 0xFFFF) + _in_stream.bits(entry & HUFFDEC_LENGTH_MASK)];
            }
            _in_stream.remove_bits(entry & HUFFDEC_LENGTH_MASK);
            // PRINT_DEBUG("_in_stream position %x\n",_in_stream.in_next);
            if (entry & HUFFDEC_LITERAL) {
                /* Literal  */
                if (unlikely(window.available() == 0)) {
                    window.flush();
                    assert(window.available() != 0);
                }

                if (Might::fail_if(!window.push(byte(entry >> HUFFDEC_RESULT_SHIFT)))) {
                    return block_result::INVALID_LITERAL;
                }

                // fprintf(stderr,"literal: %c\n",byte(entry >> HUFFDEC_RESULT_SHIFT)); // this is indeed the plaintext decoded character, good to know
                continue;
            }

            /* Match or end-of-block  */
            entry >>= HUFFDEC_RESULT_SHIFT;
            _in_stream.ensure_bits<InputStream::bitbuf_max_ensure>();

            /* Pop the extra length bits and add them to the length base to
             * produce the full length.  */
            const u32 length = (entry >> HUFFDEC_LENGTH_BASE_SHIFT) + _in_stream.pop_bits(entry & HUFFDEC_EXTRA_LENGTH_BITS_MASK);
            assert(length <= 258);

            /* The match destination must not end after the end of the
             * output buffer.  For efficiency, combine this check with the
             * end-of-block check.  We're using 0 for the special
             * end-of-block length, so subtract 1 and it turn it into
             * SIZE_MAX.  */
            // static_assert(HUFFDEC_END_OF_BLOCK_LENGTH == 0);
            if (unlikely(length - 1 >= window.available())) {
                if (likely(length == HUFFDEC_END_OF_BLOCK_LENGTH)) { // Block done
                    return Might::succeed_if(window.notify_end_block(_in_stream)) ? success : block_result::INVALID_PARSE;
                } else { // Needs flushing
                    window.flush();
                    assert(length <= window.available());
                }
            }
            assert(length >= 3);

            // if we end up here, it means we're at a match

            /* Decode the match offset.  */
            entry = cur_d->offset_decode_table[_in_stream.bits(OFFSET_TABLEBITS)];
            if (entry & HUFFDEC_SUBTABLE_POINTER) {
                /* Offset subtable required (uncommon case)  */
                _in_stream.remove_bits(OFFSET_TABLEBITS);
                entry = cur_d->offset_decode_table[((entry >> HUFFDEC_RESULT_SHIFT) & 0xFFFF) + _in_stream.bits(entry & HUFFDEC_LENGTH_MASK)];
            }
            _in_stream.remove_bits(entry & HUFFDEC_LENGTH_MASK);
            entry >>= HUFFDEC_RESULT_SHIFT;

            /* Pop the extra offset bits and add them to the offset base to
             * produce the full offset.  */
            const u32 offset = (entry & HUFFDEC_OFFSET_BASE_MASK) + _in_stream.pop_bits(entry >> HUFFDEC_EXTRA_OFFSET_BITS_SHIFT);

            /* Copy the match: 'length' bytes at 'window_next - offset' to
             * 'window_next'.  */
            if (Might::fail_if(!window.copy_match(length, offset))) {
                return block_result::INVALID_MATCH;
            }
        }
    }

  private:
    static constexpr const char* block_result_strings[] = {
        "SUCCESS",
        "LAST_BLOCK",
        "CAUGHT_UP_DOWNSTREAM",
        "FLUSH_FAIL",
        "INVALID_BLOCK_TYPE",
        "INVALID_DYNAMIC_HT",
        "INVALID_UNCOMPRESSED_BLOCK",
        "INVALID_LITERAL",
        "INVALID_MATCH",
        "TOO_MUCH_INPUT",
        "NOT_ENOUGH_INPUT",
        "INVALID_PARSE",
    };

    template<typename Might = ShouldSucceed>
    bool prepare_dynamic(const Might& might_tag = {})
    {

        /* The order in which precode lengths are stored.  */
        static constexpr u8 deflate_precode_lens_permutation[DEFLATE_NUM_PRECODE_SYMS] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

        /* Read the codeword length counts.  */
        unsigned num_litlen_syms = _in_stream.pop_bits(5) + 257;
        unsigned num_offset_syms = _in_stream.pop_bits(5) + 1;
        const unsigned num_explicit_precode_lens = _in_stream.pop_bits(4) + 4;

        /* Read the precode codeword lengths.  */
        _in_stream.ensure_bits<DEFLATE_NUM_PRECODE_SYMS * 3>();

        for (unsigned i = 0; i < num_explicit_precode_lens; i++)
            _decompressor.u.precode_lens[deflate_precode_lens_permutation[i]] = _in_stream.pop_bits(3);

        for (unsigned i = num_explicit_precode_lens; i < DEFLATE_NUM_PRECODE_SYMS; i++)
            _decompressor.u.precode_lens[deflate_precode_lens_permutation[i]] = 0;

        /* Build the decode table for the precode.  */
        if (Might::fail_if(!build_precode_decode_table(&_decompressor, might_tag)))
            return false;

        /* Expand the literal/length and offset codeword lengths.  */
        for (unsigned i = 0; i < num_litlen_syms + num_offset_syms;) {
            _in_stream.ensure_bits<DEFLATE_MAX_PRE_CODEWORD_LEN + 7>();

            /* (The code below assumes that the precode decode table
             * does not have any subtables.)  */
            // static_assert(PRECODE_TABLEBITS == DEFLATE_MAX_PRE_CODEWORD_LEN);

            /* Read the next precode symbol.  */
            const u32 entry = _decompressor.u.l.precode_decode_table[_in_stream.bits(DEFLATE_MAX_PRE_CODEWORD_LEN)];
            _in_stream.remove_bits(entry & HUFFDEC_LENGTH_MASK);
            const unsigned presym = entry >> HUFFDEC_RESULT_SHIFT;

            if (presym < 16) {
                /* Explicit codeword length  */
                _decompressor.u.l.lens[i++] = presym;
                continue;
            }

            /* Run-length encoded codeword lengths  */

            /* Note: we don't need verify that the repeat count
             * doesn't overflow the number of elements, since we
             * have enough extra spaces to allow for the worst-case
             * overflow (138 zeroes when only 1 length was
             * remaining).
             *
             * In the case of the small repeat counts (presyms 16
             * and 17), it is fastest to always write the maximum
             * number of entries.  That gets rid of branches that
             * would otherwise be required.
             *
             * It is not just because of the numerical order that
             * our checks go in the order 'presym < 16', 'presym ==
             * 16', and 'presym == 17'.  For typical data this is
             * ordered from most frequent to least frequent case.
             */
            if (presym == 16) {
                /* Repeat the previous length 3 - 6 times  */
                if (Might::fail_if(!(i != 0))) {
                    PRINT_DEBUG_DECODING("fail at (i!=0)\n");
                    return false;
                }
                const u8 rep_val = _decompressor.u.l.lens[i - 1];
                const unsigned rep_count = 3 + _in_stream.pop_bits(2);
                _decompressor.u.l.lens[i + 0] = rep_val;
                _decompressor.u.l.lens[i + 1] = rep_val;
                _decompressor.u.l.lens[i + 2] = rep_val;
                _decompressor.u.l.lens[i + 3] = rep_val;
                _decompressor.u.l.lens[i + 4] = rep_val;
                _decompressor.u.l.lens[i + 5] = rep_val;
                i += rep_count;
            } else if (presym == 17) {
                /* Repeat zero 3 - 10 times  */
                const unsigned rep_count = 3 + _in_stream.pop_bits(3);
                _decompressor.u.l.lens[i + 0] = 0;
                _decompressor.u.l.lens[i + 1] = 0;
                _decompressor.u.l.lens[i + 2] = 0;
                _decompressor.u.l.lens[i + 3] = 0;
                _decompressor.u.l.lens[i + 4] = 0;
                _decompressor.u.l.lens[i + 5] = 0;
                _decompressor.u.l.lens[i + 6] = 0;
                _decompressor.u.l.lens[i + 7] = 0;
                _decompressor.u.l.lens[i + 8] = 0;
                _decompressor.u.l.lens[i + 9] = 0;
                i += rep_count;
            } else {
                /* Repeat zero 11 - 138 times  */
                const unsigned rep_count = 11 + _in_stream.pop_bits(7);
                memset(&_decompressor.u.l.lens[i], 0, rep_count * sizeof(_decompressor.u.l.lens[i]));
                i += rep_count;
            }
        }

        if (!build_offset_decode_table(&_decompressor, num_litlen_syms, num_offset_syms, might_tag)) {
            PRINT_DEBUG_DECODING("fail at build_offset_decode_table(_decompressor, num_litlen_syms, num_offset_syms)\n");
            return false;
        }
        if (!build_litlen_decode_table(&_decompressor, num_litlen_syms, num_offset_syms, might_tag)) {
            PRINT_DEBUG_DECODING("fail at build_litlen_decode_table(_decompressor, num_litlen_syms, num_offset_syms)\n");
            return false;
        }

        return true;
    }

    template<typename OutWindow, typename Might = ShouldSucceed>
    inline bool do_uncompressed(OutWindow& out, const Might& Might_tag = {})
    {
        /* Uncompressed block: copy 'len' bytes literally from the input
         * buffer to the output buffer.  */
        _in_stream.align_input();

        if (unlikely(_in_stream.available() < 4)) {
            PRINT_DEBUG_DECODING("bad block, uncompressed check less than 4 bytes in input\n");
            return false;
        }

        u16 len = _in_stream.pop_u16();
        u16 nlen = _in_stream.pop_u16();

        if (Might::fail_if(len != (u16)~nlen)) {
            // PRINT_DEBUG("bad uncompressed block: len encoding check\n");
            return false;
        }

        if (unlikely(len > _in_stream.available())) {
            PRINT_DEBUG_DECODING("bad uncompressed block: len bigger than input stream \n");
            return false;
        }

        if (Might::fail_if(!out.copy(_in_stream, len))) {
            PRINT_DEBUG_DECODING("bad uncompressed block: rejected by output window (non-ascii)\n");
            return false;
        };
        return true;
    }

  protected:
    InputStream _in_stream;

  private:
    static inline struct libdeflate_decompressor make_static_decompressor()
    {
        struct libdeflate_decompressor sd;
        prepare_static(&sd);
        return sd;
    }

    static libdeflate_decompressor static_decompressor;
    struct libdeflate_decompressor _decompressor = {};
};

libdeflate_decompressor DeflateParser::static_decompressor = DeflateParser::make_static_decompressor();

namespace details {

template<typename IntegerType>
struct Integer_helper;

template<>
struct Integer_helper<uint8_t>
{
    using double_ty = uint16_t;
};

template<>
struct Integer_helper<uint16_t>
{
    using double_ty = uint32_t;
};

template<>
struct Integer_helper<__m128i>
{
    static __m128i broadcast(uint8_t c) { return _mm_set1_epi8(c); }

    static __m128i broadcast(uint16_t c) { return _mm_set1_epi16(c); }

    static __m128i broadcast(uint32_t c) { return _mm_set1_epi32(c); }
};

template<std::size_t N, typename FunctionType, std::size_t I>
class repeat_t
{
  public:
    repeat_t(FunctionType function)
      : function_(function)
    {}
    forceinline void operator()()
    {
        function_(I);
        repeat_t<N, FunctionType, I + 1>{ function_ }();
    }

  private:
    FunctionType function_;
};

// Unrolled loops through template recurssion
template<std::size_t N, typename FunctionType>
class repeat_t<N, FunctionType, N>
{
  public:
    repeat_t(FunctionType) {}
    forceinline void operator()() {}
};

template<std::size_t N, typename FunctionType>
forceinline void
repeat(FunctionType function)
{
    repeat_t<N, FunctionType, 0>{ function }();
}

}

/** Context window for a deflate parse
 * Can be used with different symbols types, alllowing to handle symbolic backreferences
 * The window buffer is wrapped using virtual memory mapping
 */
template<typename _char_t = char, unsigned _context_bits = 15>
class Window
{
  public:
    // TODO: should we set these at runtime ? context_bits=15 should be fine for all compression levels
    // buffer_size could be adjusted on L3 cache size.
    // But: there is a runtime cost as we loose some optimizations
    static constexpr unsigned context_bits = _context_bits;

    using char_t = _char_t;
    // Ascii range accepted for literals
    static constexpr char_t max_value = char_t('~'), min_value = char_t('\t');

    using wsize_t = uint_fast32_t; /// Type for positive offset in buffer
    using wssize_t = int_fast32_t; /// Type for signed offsets in buffer

    static constexpr wsize_t context_size = wsize_t(1) << context_bits;
    // We add one 4k page for the actual size of the buffer, allowing to copy more than requested
    static constexpr wsize_t buffer_size = context_size + ::details::page_size / sizeof(char_t);

    using copy_match_ty = __m128i;
    static constexpr wsize_t batch_copymatch_size = sizeof(copy_match_ty) / sizeof(char_t);
    static_assert(sizeof(copy_match_ty) % sizeof(char_t) == 0, "Fractional number of symbols in copy match batches");

    static constexpr wsize_t max_match_lentgh = 258;

    /// Maximum effective copy match size: the length is rounded up to a multiple of batch_copymatch_size
    static constexpr wsize_t headroom = max_match_lentgh + batch_copymatch_size - 1;

    Window(Window&&) noexcept = default;
    Window& operator=(Window&&) noexcept = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) noexcept = delete;

    Window(const char* shm_name = nullptr)
      : _buffer(alloc_mirrored<char_t>(buffer_size, shm_name))
      , _midpoint(&_buffer[buffer_size])
      , waterline(_buffer.end() - headroom)
      , next(_midpoint)
    {
        clear();
    }

    void clear() { next = _midpoint; }

    // wsize_t capacity() const { return context_size; }

    //    wsize_t size() const
    //    {
    //        assert(next >= this->_buffer);
    //        return next - _midpoint;
    //    }

    wsize_t available() const
    {
        assert(next <= waterline);
        return waterline - next;
    }

    bool push(char_t c)
    {
        if (unlikely(char_t(c) > max_value || char_t(c) < min_value)) {
            PRINT_DEBUG("fail, unprintable literal unexpected in fastq\n");
            return false;
        }

        assert(available() >= 1); // Enforced by do_block
        *next++ = char_t(c);
        return true;
    }

    /* return true if it's a reasonable offset, otherwise false */
    __attribute__((hot, always_inline)) bool copy_match(wsize_t length, wsize_t offset)
    {
        if (unlikely(offset > context_size)) {
            PRINT_DEBUG("fail, copy_match, offset %d\n", (int)offset);
            return false;
        }

        // Could not happen with the way offset and length are encoded
        assert(length >= 3);
        assert(length <= max_match_lentgh);
        assert(offset != 0);
        assert(offset <= context_size);
        assert(available() >= length);

        char_t* dst = next;
        char_t* const dst_end = next + length;
        assert(_buffer.includes(dst, dst_end));

        const char_t* src = next - offset;
        assert(_buffer.includes(src, src + length));

        if (offset >= batch_copymatch_size) {
            do {
                memcpy(dst, src, batch_copymatch_size * sizeof(char_t));
                dst += batch_copymatch_size;
                src += batch_copymatch_size;
            } while (unlikely(dst < dst_end));
        } else if (offset == 1) {
            copy_match_ty repeats = details::Integer_helper<copy_match_ty>::broadcast(*src);
            do {
                memcpy(dst, &repeats, batch_copymatch_size * sizeof(char_t));
                dst += batch_copymatch_size;
            } while (unlikely(dst < dst_end));
        } else if (offset != 2) { // Universal case
            do {
                details::repeat<batch_copymatch_size>([&](size_t) { *dst++ = *src++; });
            } while (unlikely(dst < dst_end));
        } else {
            assert(offset == 2);
            typename details::Integer_helper<char_t>::double_ty two_symbols;
            memcpy(&two_symbols, src, 2 * sizeof(char_t));
            copy_match_ty repeats = details::Integer_helper<copy_match_ty>::broadcast(two_symbols);
            do {
                memcpy(dst, &repeats, batch_copymatch_size * sizeof(char_t));
                dst += batch_copymatch_size;
            } while (unlikely(dst < dst_end));
        }
        assert(_buffer.includes(dst));

        next = dst_end;
        return true;
    }

    bool copy(InputStream& in, wsize_t length)
    {
        if (unlikely(!in.check_ascii(length))) {
            PRINT_DEBUG("fail, unprintable uncompressed block unexpected in fastq\n");
            return false;
        }
        assert(available() >= length);
        in.copy(next, length);
        next += length;
        return true;
    }

    /// Move the 32K context to the start of the buffer
    size_t flush()
    {
        next -= buffer_size;
        assert(_buffer.includes(next));
        return buffer_size;
    }

    bool notify_end_block(InputStream& in_stream) const { return true; }

    span<const char_t> current_context() const { return { next - context_size, next }; }
    span<char_t> current_context() { return { next - context_size, next }; }

  protected:
    mmap_span<char_t> _buffer;
    char_t* const _midpoint;
    const char_t* const waterline;
    char_t* next;
};

/** A do nothing window for checking the validity of the stream while searching for a block during random access.
 */
struct DummyWindow
{
    static constexpr unsigned context_bits = 15;

    using char_t = uint8_t;
    static constexpr char_t max_value = char_t('~'), min_value = char_t('\t');

    using wsize_t = uint_fast32_t; /// Type for positive offset in buffer
    using wssize_t = int_fast32_t; /// Type for signed offsets in buffer

    static constexpr wsize_t context_size = wsize_t(1) << context_bits;

    void clear() { _size = 0; }

    wsize_t size() const { return _size; }

    wsize_t available() const { return context_size; }

    bool push(char_t c)
    {
        _size++;
        return (c <= max_value && c >= min_value);
    }

    /* return true if it's a reasonable offset, otherwise false */
    bool copy_match(wsize_t length, wsize_t offset)
    {
        assert(length >= 3);
        assert(length <= 258);
        assert(offset != 0);
        _size += length;
        return offset <= context_size;
    }

    bool copy(InputStream& in, wsize_t length)
    {
        _size += length;
        return in.check_ascii(length);
    }

    /// Move the 32K context to the start of the buffer
    size_t flush() { return _size; }

    bool notify_end_block(InputStream& in_stream) const { return true; }

  protected:
    size_t _size = 0;
};

/** Window that flush its content to a buffer
 * Cache lines are flushed with streaming non-temporal store using the write combining buffer
 */
template<typename Base>
class WindowToBuffer : public Base
{
  private:
    static constexpr size_t cache_line_size = details::cache_line_size;

  public:
    using typename Base::char_t;
    static_assert(cache_line_size % sizeof(char_t) == 0, "A integer number of char_t should fits in cache line");

    template<typename... Args>
    WindowToBuffer(Args&&... args)
      : Base(std::forward<Args>(args)...)
      , _in_pos(Base::next)
    {}

    WindowToBuffer(const WindowToBuffer&) = delete;
    WindowToBuffer& operator=(const WindowToBuffer&) = delete;

    void clear(span<char_t> buffer)
    {
        _out = buffer;
        Base::clear();
        _in_pos = Base::next;
    }

    size_t flush()
    {
        const char_t* src = _in_pos;
        const char_t* const src_end = ::details::round_up<cache_line_size>(Base::next);
        const size_t sz = (src_end - src);
        assert(src_end > src);

        if (unlikely(sz > _out.size()))
            return 0;

        span<char_t> dst = _out.sub_range(sz);
        _out.pop_front(sz);

        do {
            copy_cache_line(dst.begin(), src);
            dst.pop_front(cache_line_size / sizeof(char_t));
            src += cache_line_size / sizeof(char_t);
        } while (dst.size() > 0);
        assert(src == src_end);

        _in_pos = src_end - Base::flush();
        return sz;
    }

    /// Copy the remaining data at the end of decompression, returns the remaining buffer space aligned to the next cache line
    span<char_t> final_flush()
    { // We're leaving together
        assert(_in_pos <= Base::next);
        size_t sz = Base::next - _in_pos;

        if (_out.size() < sz)
            return {}; // Not enough space

        memcpy(_out.begin(), _in_pos, sz * sizeof(char_t));
        _out.pop_front(sz);

        return { ::details::round_up<cache_line_size>(_out.begin()), _out.end() };
    }

    // Return a pointer after the last written symbol in the buffer
    char_t* buf_ptr() const { return _out.begin(); }

  private:
    static void copy_cache_line(char_t* restrict _dst, const char_t* restrict _src)
    {
        using stream_ty = __m128i;
        static_assert(cache_line_size % sizeof(stream_ty) == 0, "A integer number of stream_ty should fits in cache line");

        assert(reinterpret_cast<uintptr_t>(_dst) % cache_line_size == 0);
        assert(reinterpret_cast<uintptr_t>(_src) % cache_line_size == 0);

        auto dst = reinterpret_cast<stream_ty*>(_dst);
        auto src = reinterpret_cast<const stream_ty*>(_src);

        details::repeat<cache_line_size / sizeof(stream_ty)>([&](size_t) { _mm_stream_si128(dst++, _mm_load_si128(src++)); });
    }

    const char_t* _in_pos;
    span<char_t> _out = {};
};

/** Window for direct consumption of its content at flush time
 */
class WindowToConsumer : Window<uint8_t>
{
  private:
    using Base = Window<uint8_t>;
    static constexpr size_t cache_line_size = details::cache_line_size;

  public:
    using consumer_type = void (*)(void*, span<const uint8_t>);

    using typename Base::char_t;
    static_assert(cache_line_size % sizeof(char_t) == 0, "A integer number of char_t should fits in cache line");

    template<typename... Args>
    WindowToConsumer(Args&&... args)
      : Base(std::forward<Args>(args)...)
      , _in_pos(Base::next)
    {}

    WindowToConsumer(const WindowToConsumer&) = delete;
    WindowToConsumer& operator=(const WindowToConsumer&) = delete;

    size_t flush()
    {
        span<const char_t> flushed = { _in_pos, Base::next };
        consumer_fun(consumer_ptr, flushed);

        Base::flush();
        _in_pos = flushed.end();
        return flushed.size();
    }

    consumer_type consumer_fun;
    void* consumer_ptr;

  private:
    const char_t* _in_pos;
};

/** Compresses the 16bits back-references symbols into 8bits using a lookup-table
 */
template<typename NarrowWindow = Window<uint8_t>, typename WideWindow = Window<uint16_t>>
struct BackrefMultiplexer
{

    using narrow_t = typename NarrowWindow::char_t;
    using wide_t = typename WideWindow::char_t;

    static constexpr narrow_t first_backref_symbol = NarrowWindow::max_value + 1;
    static constexpr narrow_t last_backref_symbol = std::numeric_limits<narrow_t>::max();
    static constexpr unsigned total_available_symbols = unsigned(last_backref_symbol) + 1;

    static_assert(WideWindow::context_size == NarrowWindow::context_size, "Both window should have the same context size");

    BackrefMultiplexer()
      : lkt(make_unique_span<wide_t>(total_available_symbols))
    {
        for (narrow_t i = 0; i < first_backref_symbol; i++) {
            lkt[i] = 0;
        }
    }

    /* Given an input_context with backref encoded as max_value+1+offset on 16bits, try to write
     * a 8bits context in output_context with backref encoded in [max_value+1, 255] through a lookup table
     */
    bool compress_backref_symbols(const WideWindow& input_context, NarrowWindow& output_context)
    {
        assert(lkt);

        narrow_t next_symbol = first_backref_symbol;

        narrow_t* output_p = output_context.current_context().begin();
        for (wide_t c_from : input_context.current_context()) {
            narrow_t c_to;
            if (c_from < first_backref_symbol) {
                c_to = narrow_t(c_from);        // An in range (resolved) character
            } else {                            // Or a backref indexing the initial (unknown) context
                c_from -= first_backref_symbol; // Get the back-ref offset
                // Linear scan looking for an already allocated backref symbol
                c_to = narrow_t(0);
                for (unsigned i = first_backref_symbol; i < next_symbol; i++) {
                    if (lkt[i] == c_from) {
                        c_to = narrow_t(i);
                        assert(c_to != narrow_t(0));
                        break;
                    }
                }
                if (c_to == narrow_t(0)) { // Not found
                    // Try to allocate a new symbol
                    if (next_symbol == 0) { // wrapped arround at previous allocation
                        return false;
                    }
                    c_to = narrow_t(next_symbol);
                    lkt[next_symbol++] = c_from;
                }
                assert(c_to >= first_backref_symbol);
            }
            *output_p++ = c_to;
        }
        assert(output_p == output_context.current_context().end());

        _allocated_symbols = next_symbol != 0 ? next_symbol : total_available_symbols;
        check_compression(input_context, output_context);
        return true;
    }

    void check_compression(const WideWindow& input_context, NarrowWindow& compressed)
    {
#ifndef NDEBUG
        auto* pcomp = compressed.current_context().begin();
        for (wide_t cin : input_context.current_context()) {
            assert(*pcomp < _allocated_symbols);
            if (*pcomp < first_backref_symbol) {
                assert(*pcomp == cin);
            } else {
                assert(lkt[*pcomp] == cin - first_backref_symbol);
            }
            pcomp++;
        }
        assert(pcomp == compressed.current_context().end());
#endif
    }

    /** Given a context (ie. a lkt: offset -> char) and the compression lkt (8bits code -> offset 16bit) computed by compress_backref_symbols,
     * returns a lkt: 8bits code -> char
     */
    unique_span<narrow_t> context_to_lkt(span<const narrow_t> context)
    {
        auto res = make_unique_span<narrow_t>(total_available_symbols);

        unsigned i = 0;
        for (; i < first_backref_symbol; i++) {
            res[i] = narrow_t(i);
        }

        for (; i < total_available_symbols; i++) {
            assert(lkt[i] < NarrowWindow::context_size);
            assert(context[lkt[i]] < first_backref_symbol);
            res[i] = context[lkt[i]];
        }

        return res;
    }

    unique_span<wide_t> lkt; // 8bits code -> offset 16bit
    unsigned _allocated_symbols = 0;
};

/// Monomorphic base for passing information accross threads
class DeflateThreadBase : public DeflateParser
{
  public:
    using DeflateParser::DeflateParser;

    // Return the context and the position of the next block in the stream
    std::pair<locked_span<uint8_t>, size_t> get_context()
    {
        auto lock = std::unique_lock<std::mutex>(_mut);
        while (_context.empty())
            _cond.wait(lock);
        span<uint8_t> context;
        std::swap(context, _context);
        // Signal that the context is used: the thread will be resumed once we release the lock
        _borrowed_context = false;
        _cond.notify_all();
        PRINT_DEBUG("%p give context for block %lu\n", (void*)this, _stoped_at);
        return { locked_span<uint8_t>{ context, std::move(lock) }, _stoped_at };
    }

    /// Set the position of the first synced block upstream, so that this thread stops before this block */
    void set_end_block(size_t synced_pos)
    {
        PRINT_DEBUG("%p set to stop after %lu\n", (void*)this, synced_pos);
        _stop_after.store(synced_pos, std::memory_order_release);
    }

    size_t get_position_bits() { return _in_stream.position_bits(); }

  protected:
    static constexpr size_t unset_stop_pos = ~0UL;
    void wait_for_context_borrow()
    {
        auto lock = std::unique_lock<std::mutex>(_mut);
        bool was_borrowed = _borrowed_context;
        while (_borrowed_context)
            _cond.wait(lock);

        if (was_borrowed)
            PRINT_DEBUG("%p get context borrow back\n", (void*)this);
    }

    // Post context and wait for it to be consumed
    void set_context(span<uint8_t> ctx)
    {
        PRINT_DEBUG("%p stoped at %lu\n", (void*)this, _in_stream.position_bits());
#ifndef NDEBUG
        for (uint8_t c : ctx) {
            assert(c >= Window<uint8_t>::min_value && c <= Window<uint8_t>::max_value);
        }
#endif

        auto lock = std::unique_lock<std::mutex>(_mut);
        _context = ctx;
        _stoped_at = _in_stream.position_bits();
        _borrowed_context = true; // FIXME: wrong place
        _cond.notify_all();
    }

    size_t get_stop_pos() const { return _stop_after.load(std::memory_order_acquire); }

    template<typename Window, typename Predicate>
    block_result decompress_loop(Window& window, Predicate&& predicate)
    {
        for (;;) {
            if (unlikely(predicate()))
                return block_result::SUCCESS;
            if (unlikely(_in_stream.position_bits() >= get_stop_pos())) {
                _stop_after.store(unset_stop_pos, std::memory_order_relaxed);
                return block_result::CAUGHT_UP_DOWNSTREAM;
            }
            block_result res = do_block(window, ShouldSucceed{});
            if (unlikely(res != block_result::SUCCESS)) {
                return res;
            }
        }
    }

  private:
    /* Members for synchronization and communication */
    std::mutex _mut{};
    std::condition_variable _cond{};
    std::atomic<size_t> _stop_after = { unset_stop_pos }; // Where we should stop
    size_t _stoped_at = unset_stop_pos;                   // Where we stopped
    span<uint8_t> _context = {};
    bool _borrowed_context = false;
};

class DeflateThreadRandomAccess : public DeflateThreadBase
{

  public:
    static constexpr size_t buffer_virtual_size = 128ull << 20;

    DeflateThreadRandomAccess(const InputStream& in_stream)
      : DeflateThreadBase(in_stream)
      , buffer(alloc_huge<uint8_t>(buffer_virtual_size))
    {}

    DeflateThreadRandomAccess(const DeflateThreadRandomAccess&) = delete;
    DeflateThreadRandomAccess& operator=(const DeflateThreadRandomAccess&) = delete;

    ~DeflateThreadRandomAccess()
    {
        wait_for_context_borrow();
        PRINT_DEBUG("~DeflateThreadRandomAccess\n");
    }

    void set_upstream(DeflateThreadBase* up_stream) { _up_stream = up_stream; }

    size_t sync(size_t skip,
                const size_t max_bits_skip = size_t(1) << (3 + 20), // 1MiB
                const size_t min_block_size = 1 << 13               // 8KiB
    )
    {
        _in_stream.set_position_bits(skip);

        DummyWindow dummy_win;

        size_t pos = skip;
        const size_t max_pos = pos + std::min(8 * _in_stream.size(), max_bits_skip);

        for (_in_stream.ensure_bits<1>(); pos < max_pos; pos++) {
            assert(pos == _in_stream.position_bits());

            if (_in_stream.bits(1)) { // We don't except to find a final block
                _in_stream.remove_bits(1);
                _in_stream.ensure_bits<1>();
                continue;
            }

            block_result res = do_block(dummy_win, ShouldFail{});

            if (unlikely(res == block_result::SUCCESS) && dummy_win.size() >= min_block_size) {
                PRINT_DEBUG("%p Candidate block start at %lubits\n", (void*)this, pos);
                _in_stream.set_position_bits(pos);
                _up_stream->set_end_block(pos);
                return pos;
            }

            dummy_win.clear();
            if (unlikely(!_in_stream.set_position_bits(pos + 1))) {
                break;
            }
        }
        return 8 * _in_stream.size();
    }

    void go(size_t skipbits)
    {
        assert(_up_stream != nullptr);

        size_t sync_bitpos = sync(skipbits);
        if (sync_bitpos >= 8 * _in_stream.size()) {
            assert(false); // FIXME: Could not find a block in the region [skipbits, eof[
            // We should let the previous thread terminate and do nothing perhaps ?
        }

        size_t stop_bitpos = get_stop_pos();
        if (stop_bitpos != unset_stop_pos && sync_bitpos >= stop_bitpos) {
            assert(false); // FIXME: We found our first block after where we are supposed to stop
        }

        size_t block_count = 0;
        span<uint16_t> wide_buffer = buffer.reinterpret<uint16_t>();
        wide_window.clear(wide_buffer);
        uint16_t sym = wide_window.max_value + 1;
        for (auto& c : wide_window.current_context())
            c = sym++;
        auto res = decompress_loop(wide_window, [&]() {
            block_count++;
            if (block_count <= 8 || block_count % 2 == 0)
                return false;

            wait_for_context_borrow(); // the narrow_window is mutated from this point
            return multiplexer.compress_backref_symbols(wide_window, narrow_window);
        });

        span<uint8_t> narrow_buffer = wide_window.final_flush().reinterpret<uint8_t>();
        if (unlikely(narrow_buffer.empty() && res <= block_result::CAUGHT_UP_DOWNSTREAM)) // Not enough space for the final flush
            res = block_result::FLUSH_FAIL;
        wide_buffer = { wide_buffer.begin(), wide_window.buf_ptr() };

        if (res == block_result::SUCCESS) {
            narrow_window.clear(narrow_buffer);
            res = this->decompress_loop(narrow_window, []() { return false; });

            narrow_buffer = { narrow_buffer.begin(), narrow_window.buf_ptr() };

            if (res == block_result::CAUGHT_UP_DOWNSTREAM || res == block_result::LAST_BLOCK) {
                unique_span<uint8_t> lkt;
                {
                    auto upstream_context = _up_stream->get_context();
                    lkt = multiplexer.context_to_lkt(upstream_context.first);
                    assert(sync_bitpos == upstream_context.second);
                    // FIXME: if this not the case: redecompress from this position (with resolved context this time)
                }

                for (auto& c : narrow_window.current_context())
                    c = lkt[c];

                this->set_context(narrow_window.current_context());

                write(STDOUT_FILENO, narrow_window.current_context().begin(), narrow_window.current_context().size());
            } else if (res == block_result::FLUSH_FAIL) {
                assert(false); // FIXME: buffer too small for input buffer_virtual_size = 128MiB for 32MiB of input => max compression ratio of 4x
                // The simplest way to handle that is stop everything and retry with smaller sections/chunks
            } else {
                assert(false); // FIXME: find a way to popagate errors...
                // At this point we have narrowed down the back-reference count to 126 and decoded more than 8 block, so it's really unlikely that the
                // synchronisation is a false positive
            }

        } else if (res == block_result::CAUGHT_UP_DOWNSTREAM || res == block_result::LAST_BLOCK) {
            assert(false); // FIXME: not enough input to compress backref: we have to translate the wide window into the narrow_window to yield the next context
            wait_for_context_borrow(); // the narrow_window is mutated from this point
            narrow_window.clear(narrow_buffer);
            //        auto prev_ctx = _up_stream->get_context();
            //        auto my_ctx = wide_window.current_context();
            //        auto trans_ctx = std::make_unique<uint8_t[]>(narrow_window.context_size);

            //        for (unsigned i = 0; i < narrow_window.context_size; i++) {
            //            if (my_ctx[i] <= narrow_window.max_value) {
            //                trans_ctx[i] = my_ctx[i];
            //            } else {
            //                size_t offset = my_ctx[i] - (narrow_window.max_value + 1);
            //                assert(offset < narrow_window.context_size);
            //                trans_ctx[i] = prev_ctx[offset];
            //            }
            //        }

            //        flush(stdout);

            //        write(STDOUT_FILENO, trans_ctx.get(), narrow_window.context_size);

        } else if (res == block_result::FLUSH_FAIL) {
            assert(false); // FIXME: buffer overflow, see above
        } else {
            assert(false); // FIXME: parse error in the first block
            // Maybe the synchronisation is a false positive and we should retry at sync_bitpos + 1
        }
    }

    using consumer_16bits_type = void (*)(span<uint8_t>, span<const uint8_t> lkt);
    using consumer_8bits_type = void (*)(span<uint8_t>, span<const uint8_t> lkt);

  private:
    malloc_span<uint8_t> buffer;
    WindowToBuffer<Window<uint16_t>> wide_window = {};
    WindowToBuffer<Window<uint8_t>> narrow_window = {};
    BackrefMultiplexer<Window<uint8_t>, Window<uint16_t>> multiplexer = {};
    DeflateThreadBase* _up_stream = nullptr;
    void* consumer = nullptr;
    consumer_16bits_type consumer_16bits = nullptr;
    consumer_16bits_type consumer_8bits = nullptr;
};

class DeflateThreadFirstBlock : public DeflateThreadBase
{
  public:
    using DeflateThreadBase::DeflateThreadBase;

    DeflateThreadFirstBlock(const DeflateThreadFirstBlock&) = delete;
    DeflateThreadBase& operator=(const DeflateThreadFirstBlock&) = delete;

    void set_initial_context(span<uint8_t> context = {})
    {
        wait_for_context_borrow();
        if (context) {
            memcpy(_window.current_context().begin(), context.begin(), _window.current_context().size());
        }
    }

    void go(size_t position_bits = 0)
    {
        wait_for_context_borrow();

        _in_stream.set_position_bits(position_bits);

        _window.clear();
        auto res = this->decompress_loop(_window, []() { return false; });

        if (res > block_result::CAUGHT_UP_DOWNSTREAM) {
            assert(false); // FIXME: find a way to popagate errors...
        }

        this->set_context(_window.current_context());
    }

    ~DeflateThreadFirstBlock()
    {
        wait_for_context_borrow();
        PRINT_DEBUG("~DeflateThreadFirstBlock");
    }

  private:
    Window<uint8_t> _window{};
};

/* namespace */

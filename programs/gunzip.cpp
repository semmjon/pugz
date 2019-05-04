/*
 * gzip.c - a file compression and decompression program
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

#include "../lib/gzip_decompress.hpp" //FIXME

#include "prog_util.h"

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <sys/time.h>
#include <unistd.h>
#include <utime.h>

struct options
{
    bool count_lines;
    unsigned nthreads;
};

static const tchar* const optstring = T(":hnlt:V");

static void
show_usage(FILE* fp)
{
    fprintf(fp,
            "Usage: %" TS " [-l] [-t n] FILE...\n"
            "Decompress the specified FILEs.\n"
            "\n"
            "Options:\n"
            "  -l        count line instead of content to standard output\n"
            "  -t n      use n threads\n"
            "  -h        print this help\n"
            "  -V        show version and legal information\n",
            program_invocation_name);
}

static void
show_version(void)
{
    printf("Pugz parallel gzip decompression program\n"
           "Copyright 2019 Maël Kerbiriou, Rayan Chikhi\n"
           "\n"
           "Based on:\n"
           "gzip compression program v" LIBDEFLATE_VERSION_STRING "\n"
           "Copyright 2016 Eric Biggers\n"
           "\n"
           "This program is free software which may be modified and/or redistributed\n"
           "under the terms of the MIT license.  There is NO WARRANTY, to the extent\n"
           "permitted by law.  See the COPYING file for details.\n");
}

static int
stat_file(struct file_stream* in, stat_t* stbuf, bool allow_hard_links)
{
    if (tfstat(in->fd, stbuf) != 0) {
        msg("%" TS ": unable to stat file", in->name);
        return -1;
    }

    if (!S_ISREG(stbuf->st_mode) && !in->is_standard_stream) {
        msg("%" TS " is %s -- skipping", in->name, S_ISDIR(stbuf->st_mode) ? "a directory" : "not a regular file");
        return -2;
    }

    if (stbuf->st_nlink > 1 && !allow_hard_links) {
        msg("%" TS " has multiple hard links -- skipping "
            "(use -f to process anyway)",
            in->name);
        return -2;
    }

    return 0;
}

static int
decompress_file(const tchar* path, const struct options* options)
{
    struct file_stream in;
    stat_t stbuf;
    int ret;
    const byte* in_p;

    ret = xopen_for_read(path, true, &in);
    if (ret != 0)
        goto out_free_paths;

    ret = stat_file(&in, &stbuf, true);
    if (ret != 0)
        goto out_close_in;

    /* TODO: need a streaming-friendly solution */
    ret = map_file_contents(&in, stbuf.st_size);
    if (ret != 0)
        goto out_close_in;

    in_p = static_cast<const byte*>(in.mmap_mem);
    if (options->count_lines) {
        LineCounter line_counter{};
        libdeflate_gzip_decompress(in_p, in.mmap_size, options->nthreads, line_counter, nullptr);
    } else {
        OutputConsumer output{};
        ConsumerSync sync{};
        libdeflate_gzip_decompress(in_p, in.mmap_size, options->nthreads, output, &sync);
    }

    ret = 0;

out_close_in:
    xclose(&in);
out_free_paths:
    return ret;
}

int
tmain(int argc, tchar* argv[])
{
    tchar* default_file_list[] = { NULL };
    struct options options;
    int opt_char;
    int i;
    int ret;

    _program_invocation_name = get_filename(argv[0]);

    options.count_lines = false;
    options.nthreads = 1;

    while ((opt_char = tgetopt(argc, argv, optstring)) != -1) {
        switch (opt_char) {
            case 'l':
                options.count_lines = true;
                break;

            case 'h':
                show_usage(stdout);
                return 0;
            case 'n':
                /*
                 * -n means don't save or restore the original filename
                 *  in the gzip header.  Currently this implementation
                 *  already behaves this way by default, so accept the
                 *  option as a no-op.
                 */
                break;

            case 't':
                options.nthreads = atoi(toptarg);
                fprintf(stderr, "using %d threads for decompression (experimental)\n", options.nthreads);
                break;
            case 'V':
                show_version();
                return 0;
            default:
                show_usage(stderr);
                return 1;
        }
    }

    argv += toptind;
    argc -= toptind;

    if (argc == 0) {
        argv = default_file_list;
        argc = sizeof(default_file_list)/sizeof(default_file_list);
    } else {
        for (i = 0; i < argc; i++)
            if (argv[i][0] == '-' && argv[i][1] == '\0')
                argv[i] = NULL;
    }

    ret = 0;
    for (i = 0; i < argc; i++) {
        ret |= -decompress_file(argv[i], &options);
    }

    /*
     * If ret=0, there were no warnings or errors.  Exit with status 0.
     * If ret=2, there was at least one warning.  Exit with status 2.
     * Else, there was at least one error.  Exit with status 1.
     */
    if (ret != 0 && ret != 2)
        ret = 1;

    return ret;
}

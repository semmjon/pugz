# pugz

Parallel decompression of gzipped text files.

Decompresses text files with a truly parallel algorithm in two passes. [(paper for details)](paper/paper.pdf)

## Getting Started

A Linux system on a recent x86_64 CPU is required.

### Installing

Type:

```
make
```

For maximal performance, disable assertions with:
```
make asserts=0
```

### Usage

```
./gunzip -t 8 file.gz
```

Counting lines is incredibly faster, because there is no thread synchronization:
```
./gunzip -l -t 8 file.gz
```

### Test

We provide a small example:

```
cd example
bash test.sh
``` 
## Decompression speed benchmark

| File size | Threads  |  pugz, only counting lines | pugz, full decompression | gunzip  |
| --------- | :------: | -------------------------- | ------------------------ |  ------ |
| 2.7 GB    | 1        | 145 MB/s                   | 147 MB/s                 | 55 MB/s |
|           | 3        | 291 MB/s                   | 205 MB/s                 | N/A     |
|           | 6        | 515 MB/s                   | 228 MB/s                 | N/A     |
|           | 12       | 769 MB/s                   | 248 MB/s                 | N/A     |
|           | 24       | 1052 MB/s                  | 251 MB/s                 | N/A     |
| 24 GB     | 1        | 137 MB/s                   | 144 MB/s                 | 46 MB/s |
|           | 3        | 249 MB/s                   | 183 MB/s                 | N/A     |
|           | 6        | 483 MB/s                   | 196 MB/s                 | N/A     |
|           | 12       | 865 MB/s                   | 212 MB/s                 | N/A     |
|           | 24       | 1068 MB/s                  | 212 MB/s                 | N/A     |

Script: `test/bigger_benchmark.sh`
Specs: 2x Xeon X5675, 32 GB RAM, SSD

 * Note that the synchronization required for writing to the standard output ("pugz, full decompression" case) diminishes a lot the speed up. This is not required if your application can process chunks out of order. Also, this issue can be improved in the future with better IO handling.

 * Contrary to gzip, we don't perform CRC32 calculation? It would roughly inflict a 33% slowdown.


## Algorithm overview

Contrary to the [`pigz`](https://github.com/madler/pigz/) program which does single-threaded decompression (see https://github.com/madler/pigz/blob/master/pigz.c#L232), pugz found a way to do truly parallel decompression. In a nutshell: the compressed file is splitted into consecutive sections, processed one after the other. Sections are in turn splitted into chunks (one chunk per thread) and will be decompressed in parallel. A first pass decompresses chunks and keeps track of back-references (see e.g. our paper for the definition of that term), but is unable to resolve them. Then, a quick sequential pass is done to resolve the contexts of all chunks. A final parallel pass translates all unresolved back-references and outputs the file.

## Roadmap/TODOs

This is a prototype for proof of concept, so expect some rough corners.

If pugz chokes on some of your large files that you are willing to share, please fill a issue !

- **Pugz is not yet a production-ready gzip decompressor**, and may still crash on some files. Or produce undefined behavior when compiled with `make asserts=0`. This is because blocked/multipart files are not currently supported. (support planned)

- This codebase is currently only a standalone decompression program, but we would like to turn it into a library with some sort of API (e.g. `parallel_gzread()`, see [issue #6](https://github.com/Piezoid/pugz/issues/6) for discussion) in order to faciliate integration into your favorite software. Right now, the code is a mix between the libdeflate code base (C with gotos) and prototyped C++. It is mostly organized as a header library; however since the source is quite large, we don't think this is the best distribution for it. The middle-ground would be a PIMPL approach with a virtual ABI and some utility wrappers.

- **Only text files with ASCII characters** in the range `['\t', '~']` are supported. There is two reasons for that: less false positives when scanning the bitstream for a deflate block, and allows to encode unresolved back-references on 8bits along with the decompressed text. Both are optional optimizations, so a binary mode is eventually conceivable.

- Proper error handling is non existent (relies on assertions). Propagating errors between threads can be hard but it must be done eventually.

- Could generate/use an index file for faster random access in two+ passes scenario.

## License

This project is licensed under the MIT License.

## Citation 

* [M. Kerbiriou, R. Chikhi, Parallel decompression of gzip-compressed files and random access to DNA sequences, HiCOMB 2019](paper/paper.pdf)

## Acknowledgements

[ebiggers](https://github.com/ebiggers) for writing [libdeflate](https://github.com/ebiggers/libdeflate)



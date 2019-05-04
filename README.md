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

## Roadmap/TODOs

This is a prototype for proof of concept, so expect some rough corners.

If pugz chokes on some of your large files that you are willing to share, please fill a issue !

- Right now, the code is a mix between the libdeflate code base (C with gotos) and prototyped C++. Currently it is mostly organized as a header library. However since the source is quite large, we don't think this is the best distribution for it. The middle-ground would be a PIMPL approach with a virtual ABI and some utility wrappers.

- Only text files with characters in the range `['\t', '~']` are supported. There is two reasons for that: less false positives when scanning the bitstream for a deflate block, and allows to encode unresolved back-references on 8bits along with the decompressed text. Both are optional optimizations, so a binary mode is eventually conceivable.

- Proper error handling is non existent (relies on assertions). Propagating errors between threads can be hard but it must be done eventually.

- Blocked/multipart gzip is not currently supported. (support planned)

- Could generate/use an index file for faster random access in two+ passes scenario.

## License

This project is licensed under the MIT License.

## Citation 

* [M. Kerbiriou, R. Chikhi, Parallel decompression of gzip-compressed files and random access to DNA sequences, HiCOMB 2019](paper/paper.pdf)

## Acknowledgements

[ebiggers](https://github.com/ebiggers) for writing [libdeflate](https://github.com/ebiggers/libdeflate)



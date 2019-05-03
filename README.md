# pugz

Parallel decompression of gzipped text files.

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

Counting lines is incredibly faster, because there is no need to synchronize threads for output consistency:
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

- Right now, the code is a mix between the old libdeflate code base (C with gotos) and C++ prototyped code. Currently it is mostly organized like a header library. However since the source is quite large, we don't think this is the best distribution for it. The middle-ground would be a PIMPL approach with a virtual ABI and some utility wrappers.

- Proper error handling is non existent (relies on assertions).

- Blocked/multipart gzip is not currently supported. (but will be in the future)

- Could generate/use an index file for faster random access.

## License

This project is licensed under the MIT License.

## Citation 

* [M. Kerbiriou, R. Chikhi, Parallel decompression of gzip-compressed files and random access to DNA sequences, HiCOMB 2019](paper/paper.pdf)

## Acknowledgements

[ebiggers](https://github.com/ebiggers) for writing [libdeflate](https://github.com/ebiggers/libdeflate)



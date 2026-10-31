Helix MP3 + AAC fixed-point decoders (RealNetworks; RPSL/RCSL, see the
license files in each directory), vendored from
github.com/pschatzmann/arduino-libhelix @ f6e55a307c7577c5e3dfe1bf424a61075235fa08
(src/libhelix-mp3, src/libhelix-aac, src/utils/helix_{pgm,memory}.h).
Only ConfigHelix.h and helix_memory.c are ours, plus one line in
libhelix-aac/assembly.h (`defined(__unix__)` added to the portable-C branch) (the wrapper's C++ config and
allocator replaced with plain C). Built with the portable __unix__ path of
assembly.h: clang emits smull/smlal for the 64-bit multiplies on ARMv5TE.

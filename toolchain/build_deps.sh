#!/bin/sh
# Builds the Chumby One sysroot: libkusersync (atomics shim), mbedTLS 3.6.4, and a
# minimal static ffmpeg 7.1.1 (HTTPS via mbedTLS, fixed-point mp3/aac decoders).
# Needs: zig 0.17 (brew install zig), python3 + venv, curl, patch. Output goes to
# $BUILD (default: ./deps in the repo), which the Makefile reads.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-$(dirname "$HERE")/deps}
mkdir -p "$BUILD/scripts" "$BUILD/sysroot/lib" "$BUILD/sysroot/include" "$BUILD/out"

# zig cross-compiler wrapper (ARM926EJ-S, soft-float, static musl) + the atomics shim
zig cc -target arm-linux-musleabi -mcpu=arm926ej_s -O2 -c -o "$BUILD/out/kuser_sync.o" "$HERE/kuser_sync.c"
rm -f "$BUILD/sysroot/lib/libkusersync.a"; zig ar rcs "$BUILD/sysroot/lib/libkusersync.a" "$BUILD/out/kuser_sync.o"
cat > "$BUILD/scripts/zcc" <<ZCC
#!/bin/sh
for a in "\$@"; do case "\$a" in -c|-E|-S|-M|-MM) exec zig cc -target arm-linux-musleabi -mcpu=arm926ej_s "\$@";; esac; done
exec zig cc -target arm-linux-musleabi -mcpu=arm926ej_s -static "\$@" $BUILD/sysroot/lib/libkusersync.a
ZCC
printf '#!/bin/sh\nexec zig ar "$@"\n' > "$BUILD/scripts/zar"
printf '#!/bin/sh\nexec zig ranlib "$@"\n' > "$BUILD/scripts/zranlib"
chmod +x "$BUILD/scripts/"*

# mbedTLS (its generators need jsonschema + jinja2)
cd "$BUILD"
[ -d mbedtls-3.6.4 ] || curl -sL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2 | tar xj
[ -x venv/bin/python3 ] || { python3 -m venv venv; venv/bin/pip -q install jsonschema jinja2; }
PATH="$BUILD/venv/bin:$PATH" make -C mbedtls-3.6.4/library -j8 CC="$BUILD/scripts/zcc" AR="$BUILD/scripts/zar" CFLAGS="-O2 -I../include" static
cp mbedtls-3.6.4/library/libmbed*.a sysroot/lib/
cp -R mbedtls-3.6.4/include/mbedtls mbedtls-3.6.4/include/psa sysroot/include/

# ffmpeg: trim the >=65536-point FFT tables (17 MB of bss on a 60 MB device)
[ -d ffmpeg-7.1.1 ] || { curl -sL https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz | tar xJ; (cd ffmpeg-7.1.1 && patch -p1 < "$HERE/ffmpeg-7.1.1-trim-fft-tables.patch"); }
cd ffmpeg-7.1.1
./configure --prefix="$BUILD/sysroot" \
  --enable-cross-compile --arch=arm --target-os=linux \
  --cc="$BUILD/scripts/zcc" --ar="$BUILD/scripts/zar" --ranlib="$BUILD/scripts/zranlib" --nm=nm \
  --disable-armv6 --disable-armv6t2 --disable-vfp --disable-neon --disable-vfpv3 \
  --enable-small --disable-debug --disable-doc --disable-everything \
  --disable-programs --enable-ffmpeg \
  --disable-avdevice --disable-swscale --disable-postproc \
  --enable-network --enable-mbedtls --enable-version3 \
  --enable-protocol=http,https,tcp,tls,file,pipe \
  --enable-demuxer=mp3,mov,wav,aac \
  --enable-decoder=mp3,aac_fixed,pcm_s16le,pcm_s24le,pcm_u8 \
  --enable-parser=mpegaudio,aac \
  --enable-muxer=wav,pcm_s16le --enable-encoder=pcm_s16le \
  --enable-filter=aresample,aformat,anull,volume,atrim \
  --extra-cflags="-O2 -I$BUILD/sysroot/include" \
  --extra-ldflags="-L$BUILD/sysroot/lib -static" \
  --extra-libs="-lmbedtls -lmbedx509 -lmbedcrypto" \
  --pkg-config=false
make -j8 ffmpeg_g LDEXEFLAGS=-s   # stripped: 12 MB -> 3 MB
cp ffmpeg_g "$BUILD/out/ffmpeg"
echo "done: $BUILD/out/ffmpeg, $BUILD/sysroot"

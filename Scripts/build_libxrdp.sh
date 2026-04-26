#!/bin/bash
# build_libxrdp.sh — builds xRDP's libxrdp + libcommon + librfxencode for macOS
# Outputs static .a archives + headers to Vendor/xrdp/build/{lib,include}/
#
# This is invoked from a Run Script Build Phase in Xcode (idempotent — only
# rebuilds if source has changed). Sources are vendored at Vendor/xrdp/.
#
# Configure flags are intentionally minimal: we only need libcommon, libxrdp,
# and librfxcodec. We disable X11 (--without-x), PAM (--disable-pam), FUSE,
# painter, and audio-in/sound modules, since those are higher layers we replace
# with our own macOS implementations.

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XRDP_SRC="${PROJECT_ROOT}/Vendor/xrdp"
XRDP_BUILD="${PROJECT_ROOT}/Vendor/xrdp/build"
XRDP_PATCHES="${PROJECT_ROOT}/Vendor/xrdp_patches"

# Idempotency: skip if all .a files exist and are newer than configure.ac
NEED_BUILD=0
for lib in libcommon.a libxrdp.a librfxencode.a; do
    if [ ! -f "${XRDP_BUILD}/lib/${lib}" ]; then
        NEED_BUILD=1
        break
    fi
    if [ "${XRDP_SRC}/configure.ac" -nt "${XRDP_BUILD}/lib/${lib}" ]; then
        NEED_BUILD=1
        break
    fi
done

if [ "$NEED_BUILD" = "0" ]; then
    echo "[build_libxrdp.sh] artifacts up to date, skipping"
    exit 0
fi

echo "[build_libxrdp.sh] building libxrdp from ${XRDP_SRC}"

# Auto-clone xRDP source on first run. The repo intentionally doesn't vendor
# upstream xrdp to keep its size down — we just clone shallow + recursively
# (librfxcodec is a submodule).
if [ ! -d "${XRDP_SRC}" ]; then
    echo "[build_libxrdp.sh] cloning xrdp v0.10 into ${XRDP_SRC}"
    git clone --depth 1 --branch v0.10 \
        https://github.com/neutrinolabs/xrdp.git "${XRDP_SRC}"
    (cd "${XRDP_SRC}" && git submodule update --init --recursive)
fi

cd "${XRDP_SRC}"

# Apply patches (idempotent — only apply if not already applied)
echo "[build_libxrdp.sh] applying patches"
for patch in "${XRDP_PATCHES}"/*.patch; do
    [ -f "$patch" ] || continue
    # Check if patch already applied by trying to reverse-apply it (--dry-run + --reverse)
    if patch -p1 --reverse --dry-run --force <"$patch" >/dev/null 2>&1; then
        echo "  $(basename "$patch") already applied"
    else
        echo "  applying $(basename "$patch")"
        if ! patch -p1 --forward <"$patch"; then
            echo "[build_libxrdp.sh] ERROR: failed to apply $(basename "$patch")"
            exit 1
        fi
        # Bootstrap output is now stale — force re-run
        rm -f configure Makefile
    fi
done

# Find OpenSSL via Homebrew
if [ -d "/opt/homebrew/opt/openssl@3" ]; then
    OPENSSL_PREFIX="/opt/homebrew/opt/openssl@3"
elif [ -d "/usr/local/opt/openssl@3" ]; then
    OPENSSL_PREFIX="/usr/local/opt/openssl@3"
else
    echo "[build_libxrdp.sh] ERROR: openssl@3 not found via Homebrew. Run: brew install openssl@3"
    exit 1
fi

# Need Homebrew libtool for bootstrap (gnu libtoolize)
LIBTOOL_PATH=""
if [ -d "/opt/homebrew/opt/libtool/libexec/gnubin" ]; then
    LIBTOOL_PATH="/opt/homebrew/opt/libtool/libexec/gnubin"
elif [ -d "/usr/local/opt/libtool/libexec/gnubin" ]; then
    LIBTOOL_PATH="/usr/local/opt/libtool/libexec/gnubin"
fi
if [ -z "$LIBTOOL_PATH" ]; then
    echo "[build_libxrdp.sh] ERROR: libtool not found. Run: brew install libtool"
    exit 1
fi

# Bootstrap (generates configure script)
if [ ! -f "configure" ]; then
    echo "[build_libxrdp.sh] running bootstrap"
    PATH="${LIBTOOL_PATH}:$PATH" ./bootstrap >/tmp/xrdp-bootstrap.log 2>&1
fi

# Configure with minimal feature set
if [ ! -f "Makefile" ]; then
    echo "[build_libxrdp.sh] running configure"
    PKG_CONFIG_PATH="${OPENSSL_PREFIX}/lib/pkgconfig" \
    ./configure \
        --without-x \
        --disable-pam \
        --disable-fuse \
        --disable-painter \
        --disable-rdpsndaudin \
        --disable-tcp-nodelay \
        CFLAGS="-I${OPENSSL_PREFIX}/include -DXRDP_NO_X11 -fPIC" \
        LDFLAGS="-L${OPENSSL_PREFIX}/lib" \
        >/tmp/xrdp-configure.log 2>&1
fi

# Build the three target libraries
echo "[build_libxrdp.sh] building common/"
make -C common -j"$(sysctl -n hw.ncpu)" >/tmp/xrdp-make-common.log 2>&1

echo "[build_libxrdp.sh] building libxrdp/"
make -C libxrdp -j"$(sysctl -n hw.ncpu)" >/tmp/xrdp-make-libxrdp.log 2>&1

echo "[build_libxrdp.sh] building librfxcodec/"
make -C librfxcodec -j"$(sysctl -n hw.ncpu)" >/tmp/xrdp-make-rfx.log 2>&1

# Stage outputs
mkdir -p "${XRDP_BUILD}/lib" "${XRDP_BUILD}/include"

cp "${XRDP_SRC}/common/.libs/libcommon.a" "${XRDP_BUILD}/lib/libcommon.a"
cp "${XRDP_SRC}/libxrdp/.libs/libxrdp.a" "${XRDP_BUILD}/lib/libxrdp.a"
cp "${XRDP_SRC}/librfxcodec/src/.libs/librfxencode.a" "${XRDP_BUILD}/lib/librfxencode.a"

# Stage public headers
cp "${XRDP_SRC}/libxrdp/libxrdp.h" "${XRDP_BUILD}/include/libxrdp.h" 2>/dev/null || true
cp "${XRDP_SRC}/libxrdp/libxrdpinc.h" "${XRDP_BUILD}/include/libxrdpinc.h" 2>/dev/null || true

# Copy ALL common headers (not just the ones we explicitly use - libxrdp's
# public headers chain-include many of these internally).
cp "${XRDP_SRC}/common/"*.h "${XRDP_BUILD}/include/" 2>/dev/null || true

# config_ac.h is generated by configure at the source root; libxrdp + common
# headers reference it directly.
cp "${XRDP_SRC}/config_ac.h" "${XRDP_BUILD}/include/" 2>/dev/null || true

echo "[build_libxrdp.sh] success — artifacts in ${XRDP_BUILD}/"
ls -la "${XRDP_BUILD}/lib/"

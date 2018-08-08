#!/usr/bin/env sh

set -e

# Amend PATH with known location of LLVM tools
BREW="$(which brew || true)"
if [ -n "${BREW}" ]; then
    PATH="$PATH:`${BREW} --prefix`/opt/llvm/bin"
fi
# Default location on Ubuntu
PATH="$PATH:/usr/lib/llvm-3.7/bin"

# If we have an included copy of FileCheck, use that.
FILECHECK="${SRCROOT}/llbuild-test-tools/utils/Xcode/FileCheck"

# If not locally, look for FileCheck in the path.
if [ ! -f "${FILECHECK}" ]; then
    FILECHECK="$(which FileCheck || true)"
fi

# If not in the path, search for it in the Jenkins ${WORKSPACE}
if [ ! -f "${FILECHECK}" ]; then
    if [ ! -z "${WORKSPACE}" ]; then
        FILECHECK="$(find ${WORKSPACE} -name FileCheck -and -type f | tail -n 1)"
    fi
fi

# If we still haven't found FileCheck, bail
if [ ! -f "${FILECHECK}" ]; then
    echo "$0: error: unable to find 'FileCheck' testing utility"
    exit 1
fi

mkdir -p "${BUILT_PRODUCTS_DIR}/tests/Unit"

sed < "${SRCROOT}/tests/lit.site.cfg.in" \
      > "${BUILT_PRODUCTS_DIR}/tests/lit.site.cfg" \
    -e "s=@LLBUILD_SRC_DIR@=${SRCROOT}=g" \
    -e "s=@LLBUILD_OBJ_DIR@=${BUILT_PRODUCTS_DIR}=g" \
    -e "s=@LLBUILD_OUTPUT_DIR@=${BUILT_PRODUCTS_DIR}=g" \
    -e "s=@LLBUILD_TOOLS_DIR@=${BUILT_PRODUCTS_DIR}=g" \
    -e "s=@LLBUILD_LIBS_DIR@=${BUILT_PRODUCTS_DIR}=g" \
    -e "s=@FILECHECK_EXECUTABLE@=${FILECHECK}=g"

sed < "${SRCROOT}/tests/Unit/lit.site.cfg.in" \
      > "${BUILT_PRODUCTS_DIR}/tests/Unit/lit.site.cfg" \
    -e "s=@LLBUILD_SRC_DIR@=${SRCROOT}=g" \
    -e "s=@LLBUILD_OBJ_DIR@=${BUILT_PRODUCTS_DIR}=g" \
    -e "s=@LLBUILD_OUTPUT_DIR@=${BUILT_PRODUCTS_DIR}=g" \
    -e "s=@LLBUILD_TOOLS_DIR@=${BUILT_PRODUCTS_DIR}=g" \
    -e "s=@LLBUILD_LIBS_DIR@=${BUILT_PRODUCTS_DIR}=g" \
    -e "s=@LLBUILD_BUILD_MODE@=.=g" \
    -e "s=@FILECHECK_EXECUTABLE@=${FILECHECK}=g"

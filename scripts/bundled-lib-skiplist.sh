#!/bin/bash
# Shared skip list for bundled-binary shared-library handling.
#
# Sourced by both scripts/bundle-binaries.sh (which decides which libs
# to copy into resources/bin/) and scripts/build-common.sh (which
# audits that every NEEDED SONAME is satisfied). Keeping the list in
# one place prevents drift: a name present in only one copy would make
# the bundler and the audit disagree about which libs must be present.
#
# These are the low-level libc / loader pieces that MUST come from the
# user's own glibc — bundling them across distros breaks the dynamic
# linker.

is_skipped_lib() {
    case "$1" in
        libc.so*|libm.so*|libpthread.so*|libdl.so*|librt.so*|\
        ld-linux*|libresolv.so*|linux-vdso*|linux-gate*|\
        libnsl.so*|libutil.so*|libgcc_s.so*)
            return 0 ;;
    esac
    return 1
}

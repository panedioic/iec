#!/bin/bash
# build_git_deps.sh — build the static libraries the git shared object needs.
#
# Each dependency is built only when its archive is missing. Sources are
# downloaded or cloned into IEC_THIRDPARTY by the individual scripts.
#
# Order matters: curl links OpenSSL and zlib, so those are built first.
set -euo pipefail

IEC_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENV_FILE="$IEC_ROOT/scripts/env.sh"
[ -f "$ENV_FILE" ] || ENV_FILE="$IEC_ROOT/scripts/env.sh.example"
# shellcheck disable=SC1090
source "$ENV_FILE"
# shellcheck disable=SC1091
source "$IEC_ROOT/scripts/lib/common.sh"

[ -f "$IEC_DEPS_LIB/libz.a" ] || "$IEC_ROOT/scripts/optional/build_zlib.sh"
[ -f "$IEC_DEPS_LIB/libpcre2-8.a" ] || "$IEC_ROOT/scripts/optional/build_pcre2.sh"
[ -f "$IEC_OPENSSL_DIR/lib/libssl.a" ] || "$IEC_ROOT/scripts/optional/build_openssl.sh"
[ -f "$IEC_CURL_DIR/lib/libcurl.a" ] || "$IEC_ROOT/scripts/optional/build_curl.sh"

iec_log "git dependencies ready:"
iec_log "  zlib   $IEC_DEPS_LIB/libz.a"
iec_log "  pcre2  $IEC_DEPS_LIB/libpcre2-8.a"
iec_log "  openssl $IEC_OPENSSL_DIR/lib/libssl.a"
iec_log "  curl   $IEC_CURL_DIR/lib/libcurl.a"

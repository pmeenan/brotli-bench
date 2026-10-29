#!/usr/bin/env bash
# Re-downloads the real-world benchmark corpora into `corpora/` and
# regenerates the stand-alone HTML dictionary in `corpora/dictionary/`.
#
# Usage: scripts/fetch_corpora.sh [--force]
#
#   (default)  Idempotent: files that already exist and match
#              `scripts/corpora.sha256` are left alone; missing or
#              mismatching cdnjs files are (re)downloaded and must verify.
#   --force    Re-download every file and regenerate the dictionary even if
#              the committed copies verify.
#
# cdnjs files are pinned to exact versions and verified strictly
# (`sha256sum -c`); a mismatch is fatal. Wikipedia pages are fetched by
# `oldid`, but the rendered desktop HTML (skin, banners, load.php URLs) can
# drift over time, so for Wikipedia pages and the dictionary trained from them
# a checksum mismatch is only a warning. Existing Wikipedia pages and the
# dictionary are never overwritten unless `--force` is given, so the committed
# copies remain the canonical benchmark inputs.
#
# Requirements: bash, curl, sha256sum, g++, cmake (for libdivsufsort).
# See `corpora/SOURCES.md` for provenance and licenses.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPORA="${ROOT}/corpora"
DICT_DIR="${CORPORA}/dictionary"
SUMS="${ROOT}/scripts/corpora.sha256"
BROTLI_C="${ROOT}/vendor/brotli-c"

CDNJS="https://cdnjs.cloudflare.com/ajax/libs"
WIKI="https://en.wikipedia.org/w/index.php"
UA="brotli-bench/0.1 (pmeenan@google.com)"

# libdivsufsort (MIT) is needed by the `durchschlag` dictionary engine.
DIVSUFSORT_REV="5f60d6f026c30fb4ac296f696b3c8b0eb71bd428"
DIVSUFSORT_URL="https://github.com/y-256/libdivsufsort/archive/${DIVSUFSORT_REV}.tar.gz"
DIVSUFSORT_SHA256="705a122a142eda16f05996e56b45247319d140928eec32bd9db6753da13f5b5b"

# Dictionary generator parameters (see `corpora/SOURCES.md`).
DICT_ENGINE="--dsh"
DICT_TARGET="64K"

FORCE=0
case "${1:-}" in
  "") ;;
  --force) FORCE=1 ;;
  *) echo "usage: $0 [--force]" >&2; exit 2 ;;
esac

# Destination (relative to `corpora/`) and cdnjs path (relative to `$CDNJS`).
CDNJS_FILES=(
  "jquery-3.7.1.min.js                     jquery/3.7.1/jquery.min.js"
  "jquery-3.7.1.js                         jquery/3.7.1/jquery.js"
  "react-dom-18.3.1.production.min.js      react-dom/18.3.1/umd/react-dom.production.min.js"
  "lodash-4.17.21.min.js                   lodash.js/4.17.21/lodash.min.js"
  "vue-3.4.38.global.prod.js               vue/3.4.38/vue.global.prod.js"
  "three-r128.min.js                       three.js/r128/three.min.js"
  "moment-2.30.1.min.js                    moment.js/2.30.1/moment.min.js"
  "bootstrap-5.3.3.min.css                 bootstrap/5.3.3/css/bootstrap.min.css"
  "font-awesome-6.5.2.all.min.css          font-awesome/6.5.2/css/all.min.css"
  "foundation-6.8.1.min.css                foundation/6.8.1/css/foundation.min.css"
  # Previous versions, used as raw dictionaries for delta-update cases.
  "dictionary/jquery-3.6.4.min.js          jquery/3.6.4/jquery.min.js"
  "dictionary/react-dom-18.2.0.production.min.js react-dom/18.2.0/umd/react-dom.production.min.js"
  "dictionary/lodash-4.17.20.min.js        lodash.js/4.17.20/lodash.min.js"
  "dictionary/vue-3.3.13.global.prod.js    vue/3.3.13/vue.global.prod.js"
  "dictionary/bootstrap-5.3.2.min.css      bootstrap/5.3.2/css/bootstrap.min.css"
)

# Held-out Wikipedia pages committed as corpora: "title oldid".
WIKI_PAGES=(
  "Brotli 1382373437"
  "Huffman_coding 1379018579"
  "Espresso 1381363121"
)

# Training set for `dictionary/wiki-html.dict` (NOT committed): "title oldid".
WIKI_TRAIN=(
  "Albert_Einstein 1382671920"
  "Amazon_River 1382171474"
  "C_(programming_language) 1382678438"
  "CSS 1382423783"
  "Data_compression 1382027270"
  "Deflate 1377071705"
  "Google_Chrome 1381749616"
  "Gzip 1373208344"
  "HTML 1382636951"
  "HTTP 1377007807"
  "JavaScript 1379385036"
  "Jazz 1381387713"
  "Linux 1381784225"
  "LZ77_and_LZ78 1374388117"
  "Photosynthesis 1379238357"
  "Python_(programming_language) 1382352332"
  "Rust_(programming_language) 1381627603"
  "Unicode 1382629125"
  "World_Wide_Web 1379635447"
  "Zstd 1382272412"
)

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

log() { echo "[fetch_corpora] $*" >&2; }
warn() { echo "[fetch_corpora] WARNING: $*" >&2; }

# Prints the expected checksum line for `$1` (path relative to `corpora/`).
sum_line() {
  awk -v f="$1" '$2 == f || $2 == "*" f { print; found = 1 } END { exit !found }' \
    "${SUMS}"
}

# Returns 0 if `corpora/$1` exists and matches `scripts/corpora.sha256`.
verify() {
  local rel="$1" line
  line="$(sum_line "${rel}")" || { warn "no checksum listed for ${rel}"; return 1; }
  [[ -f "${CORPORA}/${rel}" ]] || return 1
  (cd "${CORPORA}" && sha256sum -c --quiet --status <<<"${line}")
}

download() {
  local url="$1" out="$2"
  curl -sSfL --retry 3 --compressed -A "${UA}" -o "${out}" "${url}"
}

fetch_cdnjs() {
  local rel="$1" path="$2"
  if [[ "${FORCE}" -eq 0 ]] && verify "${rel}"; then
    log "ok      ${rel}"
    return
  fi
  log "fetch   ${rel}"
  mkdir -p "$(dirname "${CORPORA}/${rel}")"
  download "${CDNJS}/${path}" "${TMP}/dl"
  mv "${TMP}/dl" "${CORPORA}/${rel}"
  # Strict: fail on mismatch.
  (cd "${CORPORA}" && sum_line "${rel}" | sha256sum -c --quiet)
}

fetch_wiki() {
  local title="$1" oldid="$2" out="$3"
  download "${WIKI}?title=${title}&oldid=${oldid}" "${out}"
  # Be polite to Wikipedia.
  sleep 0.5
}

fetch_wiki_page() {
  local title="$1" oldid="$2" rel="wiki-$1.html"
  if [[ -f "${CORPORA}/${rel}" && "${FORCE}" -eq 0 ]]; then
    if verify "${rel}"; then
      log "ok      ${rel}"
    else
      warn "${rel} does not match recorded checksum (kept; use --force to re-fetch)"
    fi
    return
  fi
  log "fetch   ${rel} (oldid ${oldid})"
  fetch_wiki "${title}" "${oldid}" "${TMP}/dl"
  mv "${TMP}/dl" "${CORPORA}/${rel}"
  verify "${rel}" ||
    warn "${rel} differs from recorded checksum (Wikipedia rendering drift?)"
}

build_dictionary_generator() {
  local out="$1" research="${BROTLI_C}/research" dss="${TMP}/divsufsort"
  log "build   libdivsufsort ${DIVSUFSORT_REV:0:12}"
  download "${DIVSUFSORT_URL}" "${TMP}/divsufsort.tgz"
  echo "${DIVSUFSORT_SHA256}  ${TMP}/divsufsort.tgz" | sha256sum -c --quiet
  mkdir -p "${dss}/src"
  tar -xzf "${TMP}/divsufsort.tgz" -C "${dss}/src" --strip-components=1
  # `CMAKE_POLICY_VERSION_MINIMUM` lets CMake >= 4 configure the old project.
  if ! { cmake -S "${dss}/src" -B "${dss}/build" -DCMAKE_BUILD_TYPE=Release \
           -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF \
           -DCMAKE_POLICY_VERSION_MINIMUM=3.5 &&
         cmake --build "${dss}/build" -j; } >"${TMP}/cmake.log" 2>&1; then
    cat "${TMP}/cmake.log" >&2
    exit 1
  fi

  # `deorummolae` needs esaxx (not vendored); stub it out since only
  # `durchschlag` is used.
  cat >"${TMP}/dm_stub.cc" <<'EOF'
#include "deorummolae.h"
#include <cstdio>
#include <cstdlib>
std::string DM_generate(size_t, const std::vector<size_t>&, const uint8_t*) {
  std::fprintf(stderr, "--dm is not available in this build\n");
  std::abort();
}
EOF
  log "build   dictionary_generator"
  g++ -O2 -I"${research}" -I"${dss}/build/include" -o "${out}" \
    "${research}/dictionary_generator.cc" "${research}/durchschlag.cc" \
    "${research}/sieve.cc" "${TMP}/dm_stub.cc" \
    "${dss}/build/lib/libdivsufsort.a"
}

generate_dictionary() {
  local rel="dictionary/wiki-html.dict" entry title oldid
  if [[ -f "${CORPORA}/${rel}" && "${FORCE}" -eq 0 ]]; then
    if verify "${rel}"; then
      log "ok      ${rel}"
    else
      warn "${rel} does not match recorded checksum (kept; use --force to regenerate)"
    fi
    return
  fi
  if [[ ! -f "${BROTLI_C}/research/dictionary_generator.cc" ]]; then
    echo "vendor/brotli-c is missing; run 'git submodule update --init'" >&2
    exit 1
  fi
  build_dictionary_generator "${TMP}/dictionary_generator"
  mkdir -p "${TMP}/train"
  for entry in "${WIKI_TRAIN[@]}"; do
    read -r title oldid <<<"${entry}"
    log "train   ${title} (oldid ${oldid})"
    fetch_wiki "${title}" "${oldid}" "${TMP}/train/wiki-${title}.html"
  done
  log "gen     ${rel} (${DICT_ENGINE} -t${DICT_TARGET})"
  # Samples are passed in sorted (glob) order; the generator is deterministic.
  "${TMP}/dictionary_generator" "${DICT_ENGINE}" "-t${DICT_TARGET}" \
    "${TMP}/wiki-html.dict" "${TMP}"/train/wiki-*.html
  mkdir -p "${DICT_DIR}"
  mv "${TMP}/wiki-html.dict" "${CORPORA}/${rel}"
  verify "${rel}" ||
    warn "${rel} differs from recorded checksum (training pages drifted?)"
}

main() {
  local entry rel path title oldid
  mkdir -p "${CORPORA}" "${DICT_DIR}"
  for entry in "${CDNJS_FILES[@]}"; do
    read -r rel path <<<"${entry}"
    fetch_cdnjs "${rel}" "${path}"
  done
  for entry in "${WIKI_PAGES[@]}"; do
    read -r title oldid <<<"${entry}"
    fetch_wiki_page "${title}" "${oldid}"
  done
  generate_dictionary
  log "done"
}

main

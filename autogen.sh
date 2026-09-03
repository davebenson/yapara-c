#!/bin/sh
# SPDX-License-Identifier: 0BSD
# Regenerate the configure script and Makefile.in files.  Only needed
# when building from git -- a 'make dist' tarball ships them already.
set -e

cd "$(dirname "$0")"
mkdir -p build-aux m4

# pkg-config installs pkg.m4 outside automake's own aclocal directory on
# some systems (homebrew, notably), and PKG_CHECK_MODULES will not be
# found without it.
if [ -z "$ACLOCAL_PATH" ]; then
  for dir in /opt/homebrew/share/aclocal /usr/local/share/aclocal; do
    if [ -f "$dir/pkg.m4" ]; then
      ACLOCAL_PATH="$dir"
      export ACLOCAL_PATH
      break
    fi
  done
fi

autoreconf --install --force --warnings=all "$@"

echo
echo "Now run ./configure && make && make check"

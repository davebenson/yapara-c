dnl SPDX-License-Identifier: 0BSD
AC_PREREQ([2.69])
AC_INIT([yapara], [0.1.0], [daveb@ffem.org])
AC_CONFIG_SRCDIR([src/yapara-main.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])

dnl 'foreign' because this tree has README.md rather than the GNU set of
dnl README/NEWS/AUTHORS/ChangeLog.
AM_INIT_AUTOMAKE([1.11 foreign -Wall])
AM_SILENT_RULES([yes])

AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB

dnl libuv does the process spawning and the event loop.  1.0 is far older
dnl than anything in use; the floor is here to give a clear error rather
dnl than a pile of missing symbols.
PKG_CHECK_MODULES([LIBUV], [libuv >= 1.0])

AC_CONFIG_FILES([
  Makefile
  src/Makefile
])
AC_OUTPUT

AC_MSG_NOTICE([

  $PACKAGE_NAME $PACKAGE_VERSION

  prefix .......... ${prefix}
  compiler ........ ${CC}
  libuv cflags .... ${LIBUV_CFLAGS}
  libuv libs ...... ${LIBUV_LIBS}

  Now run 'make', and 'make check' to run the test-suite.
])

# SPDX-License-Identifier: 0BSD
AM_CPPFLAGS = $(LIBUV_CFLAGS)

# -Wno-unused-parameter: a vtable implementation takes the whole
# signature whether or not it uses every argument.
AM_CFLAGS = -W -Wall -Wno-unused -Wno-unused-parameter

# Not installed: the yc-* interfaces are internal and free to change
# shape.  It exists so that yapara and the three test programs share
# one compile of the library rather than four.
noinst_LIBRARIES = libyc.a

libyc_a_SOURCES = \
	yc-alloc.c \
	yc-alloc.h \
	yc-buffer.c \
	yc-buffer.h \
	yc-child.c \
	yc-child.h \
	yc-cmdline.c \
	yc-cmdline.h \
	yc-common.c \
	yc-common.h \
	yc-rbtree-macros.h \
	yc-shell.c \
	yc-shell.h \
	yc-ui.c \
	yc-ui.h \
	yc-ui-headless-jobs.c \
	yc-ui-plain.c \
	yc-ui-passthrough.c \
	yc-ui-prefix.c

LDADD = libyc.a $(LIBUV_LIBS)

bin_PROGRAMS = yapara
yapara_SOURCES = yapara-main.c

check_PROGRAMS = test-shell test-child test-ui
TESTS = $(check_PROGRAMS)

test_shell_SOURCES = test-shell.c
test_child_SOURCES = test-child.c
test_ui_SOURCES = test-ui.c

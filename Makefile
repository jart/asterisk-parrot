#
# Asterisk Phone ParrotMakefile
#
# Copyright (C) 2005-2009 J.A. Roberts Tunney
#
# J.A. Roberts Tunney <jtunney@lobstertech.com>
#
# This program is free software, distributed under the terms of
# the GNU General Public License
#

CC          = gcc

PREFIX      = /usr
MODULES_DIR = $(PREFIX)/lib/asterisk/modules

MODS        = app_parrot.so
CFLAGS      = -O -g -D_GNU_SOURCE -shared -fpic
LDFLAGS     = 

ifneq ($(wildcard /usr/include/soundtouch4c.h /usr/local/include/soundtouch4c.h),)
CFLAGS+=-D_LIBSOUNDTOUCH4C_
SOLINK+=-lsoundtouch4c
endif

include Makefile.inc

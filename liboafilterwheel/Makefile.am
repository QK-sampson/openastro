#
# Makefile.am -- liboafilterwheel Makefile template
#
# Copyright 2014,2015,2017 James Fidell (james@openastroproject.org)
#
# License:
#
# This file is part of the Open Astro Project.
#
# The Open Astro Project is free software: you can redistribute it and/or
# modify it under the terms of the GNU General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# The Open Astro Project is distributed in the hope that it will be
# useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with the Open Astro Project.  If not, see
# <http://www.gnu.org/licenses/>.
#

AM_CPPFLAGS = \
  -I$(top_srcdir)/include \
  $(LIBHIDAPI_CFLAGS) \
  $(LIBUSB_CFLAGS)

if LIBUDEV_COND
XAGYLSRC = xagylfw-udev.c xagylConnect-udev.c xagylIO-udev.c 
  # xagylControl-udev.c

else
if LIBFTDI_COND
XAGYLSRC = xagylfw-ftdi.c xagylIO-ftdi.c xagylConnect-ftdi.c
  # xagylControl-ftdi.c
endif
endif


lib_LTLIBRARIES = liboafilterwheel.la

liboafilterwheel_la_SOURCES = \
	oafw.c debug.c unimplemented.c utils.c \
	sxfw.c sxConnect.c sxControl.c sxController.c sxCallback.c sxConfig.c \
        sxIO.c \
	xagylControl.c xagylConfig.c xagylController.c xagylCallback.c \
	$(XAGYLSRC)

WARNINGS = -g -O -Wall -Werror -Wpointer-arith -Wuninitialized -Wsign-compare -Wformat-security -Wno-pointer-sign $(OSX_WARNINGS)

warnings:
	$(MAKE) V=0 CFLAGS='$(WARNINGS)' CXXFLAGS='$(WARNINGS)'
	$(MAKE) V=0 CFLAGS='$(WARNINGS)' CXXFLAGS='$(WARNINGS)' $(check_PROGRAMS)

verbose-warnings:
	$(MAKE) V=1 CFLAGS='$(WARNINGS)' CXXFLAGS='$(WARNINGS)'
	$(MAKE) V=1 CFLAGS='$(WARNINGS)' CXXFLAGS='$(WARNINGS)' $(check_PROGRAMS)

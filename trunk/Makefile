# Makefile for clone(1)
#
# Created by Dr. Rolf Jansen on 2013-01-13.
# Copyright (c) 2013 Cyclaero Ltda.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
# OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
# OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
# IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
# THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


.ifmake update
SVNREV != svn update > /dev/null && svnversion
.else
SVNREV != svnversion
.endif

CC      = clang
CFLAGS  = $(CDEFS) -DSVNREV="$(SVNREV)" -O3 -std=c99 -Wno-switch -Wno-parentheses -Wno-empty-body
LDFLAGS = -lpthread

HEADER  = utils.h
TOOLSRC = utils.c clone.c
TOOLOBJ = $(TOOLSRC:.c=.o)
TOOL    = clone

all: $(HEADER) $(TOOLSRC) $(TOOLOBJ) $(TOOL)

depend:
	$(CC) $(CFLAGS) -E -MM *.c > .depend

$(TOOL): $(TOOLOBJ)
	$(CC) $(TOOLOBJ) $(LDFLAGS) -o $@

$(TOOLOBJ):
	$(CC) $(CFLAGS) $< -c -o $@

clean:
	rm -rf *.o *.core $(TOOL)

update: clean all

install: $(TOOL)
	strip -x -o /usr/local/bin/$(TOOL) $(TOOL)
	cp $(TOOL).1 /usr/local/man/man1/$(TOOL).1

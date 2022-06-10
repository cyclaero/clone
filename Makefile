# Makefile for clone(1)
#
# Created by Dr. Rolf Jansen on 2013-01-13.
# Copyright (c) 2013-2022. All rights reserved.
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


CC ?= clang

.if exists(.git)
.ifmake update
COMCNT != git pull origin >/dev/null 2>&1; git rev-list --count HEAD
.else
COMCNT != git rev-list --count HEAD
.endif
REVNUM != echo $(COMCNT) + 1 | bc
MODCNT != git status -s | wc -l
.if $(MODCNT) > 0
MODIED  = m
.else
MODIED  =
.endif
.else
REVNUM != cut -d= -f2 scmrev.xcconfig
.endif

.ifmake debug
CFLAGS  = $(CDEFS) -g -O0
STRIP   =
.else
CFLAGS  = $(CDEFS) -g0 -O3
STRIP   = -s
.endif

CFLAGS += -DSCMREV="$(REVNUM)$(MODIED)" -std=gnu11 -fsigned-char -fno-pic -fvisibility=hidden -fstrict-aliasing -fno-common -fstack-protector \
          -Wno-switch -Wno-parentheses
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

$(TOOLOBJ): Makefile
	$(CC) $(CFLAGS) $< -c -o $@

clean:
	rm -rf *.o *.core $(TOOL)

debug: all

update: clean all

install: $(TOOL)
	install $(STRIP) $(TOOL) /usr/local/bin/
	cp $(TOOL).1 /usr/local/man/man1/$(TOOL).1

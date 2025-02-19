# bulk_extractor Makefile.am
# This file is compiled with automake to create Makefile.in.
# Makefile.in is transformed by "configure" to create Makefile
#
# (C) 2020 Simson L. Garfinkel
# https://www.gnu.org/licenses/lgpl-3.0.en.html

SUBDIRS = doc doc/latex_manuals man src src/tests python specfiles tests

# don't include bootstrap. People run it, and they shoudln't
# It's only for people who check out the git repo

EXTRA_DIST = \
	.gitignore \
	licenses/LICENSE.Autoconf_exception \
	licenses/LICENSE.CPL1 \
	licenses/LICENSE.GPLv3 \
	licenses/LICENSE.LGPLv2.1 \
	licenses/LICENSE.LGPLv3 \
	m4/ac_check_classpath.m4 \
	m4/ac_check_junit.m4 \
	m4/ac_check_rqrd_class.m4 \
	m4/ac_java_options.m4 \
	m4/ac_prog_jar.m4 \
	m4/ac_prog_java.m4 \
	m4/ac_prog_java_cc.m4 \
	m4/ac_prog_java_works.m4 \
	m4/ac_prog_javac.m4 \
	m4/ac_prog_javac_works.m4 \
	m4/ac_prog_javadoc.m4 \
	m4/ac_prog_javah.m4 \
	m4/ac_try_compile_java.m4 \
	m4/ac_try_run_javac.m4 \
	m4/ax_pthread.m4 \
	bootstrap.sh \
	README.md \
	$(SRC_WIN_DIST)

ACLOCAL_AMFLAGS = -I m4

distclean2:
	@echo Deleting:
	@find . \
        \( -name '*.orig' -o -name '*.rej' -o -name '*~' \
        -o -name '*.bak' -o -name '#*#' -o -name '.*.orig' \
        -o -name '.*.rej' -o -size 0 \
        -o -name '*%' -o -name '.*.cmd' -o -name 'core' \) \
        -type f -print | tee /dev/tty | xargs rm -f

.PHONY: exels
exels:
	/bin/ls -l */*exe

.PHONY: execlean
execlean:
	/bin/rm -f */*exe

.PHONY: win32
win32: win32/bulk_extractor32.exe

.PHONY: win64
win64: win64/bulk_extractor64.exe

win32/bulk_extractor32.exe:
	@echo '****************************************************'
	@echo '*** making win32 with mingw32-configure and make ***'
	@echo '****************************************************'
	if [ -r Makefile ]; then $(MAKE) distclean ; fi
	if [ ! -d win32  ]; then mkdir win32 ; fi
	if [ ! -r win32/Makefile ]; then (cd win32;mingw32-configure) ; fi
	(cd win32;$(MAKE))
	(cp win32/src/bulk_extractor.exe win32/bulk_extractor32.exe)
	ls -l win32/bulk_extractor32.exe

win64/bulk_extractor64.exe:
	@echo '****************************************************'
	@echo '*** making win64 with mingw32-configure and make ***'
	@echo '****************************************************'
	if [ -r Makefile ]; then $(MAKE) distclean ; fi
	if [ ! -d win64  ]; then mkdir win64 ; fi
	if [ ! -r win64/Makefile ]; then (cd win64;mingw64-configure) ; fi
	(cd win64;$(MAKE))
	(cp win64/src/bulk_extractor.exe win64/bulk_extractor64.exe)
	ls -l win64/bulk_extractor64.exe

winlibs:
	@echo '*****************************************'
	@echo '*** making win32 and win64 libraries  ***'
	@echo '*****************************************'
	if [ -r Makefile ]; then $(MAKE) distclean ; fi
	if [ ! -d win32  ]; then mkdir win32 ; fi
	if [ ! -r win32/Makefile ]; then (cd win32;mingw32-configure --disable-libewf --disable-afflib) ; fi
	(cd win32/src;$(MAKE) lib;mv libbulkextractor.so ../../libbulkextractor32.DLL)
	if [ ! -d win64  ]; then mkdir win64 ; fi
	if [ ! -r win64/Makefile ]; then (cd win64;mingw64-configure --disable-libewf --disable-afflib) ; fi
	(cd win64/src;$(MAKE) lib;mv libbulkextractor.so ../../libbulkextractor64.DLL)
	ls -l libbulk*.DLL

.PHONY: lib
lib:
	(cd src; $(MAKE) lib)

.PHONY: libinstall
libinstall:
	install -D src/libbulkextractor.so $(prefix)/lib/libbulkextractor.so
	install -D python/module/bulkextractor.py $(prefix)/lib/python2.7/dist-packages/bulkextractor.py
	if [ -r /sbin/ldconfig ]; /sbin/ldconfig -v ; fi

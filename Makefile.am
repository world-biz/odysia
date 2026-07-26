bin_PROGRAMS = odysia
check_PROGRAMS = test_indexer
TESTS = $(check_PROGRAMS)
dist_man_MANS = odysia.1
AM_CFLAGS = @ODYSIA_WARN_CFLAGS@ $(GTK_CFLAGS) $(SQLITE3_CFLAGS)
AM_CPPFLAGS = -I$(top_srcdir)/src
BUILT_SOURCES = odysia-resources.c odysia-resources.h
CLEANFILES = $(BUILT_SOURCES) odysia-icon.o

odysia_SOURCES = \
	main.c \
	src/indexer.c \
	src/indexer.h \
	src/ui.c \
	src/ui.h
nodist_odysia_SOURCES = odysia-resources.c odysia-resources.h
odysia_CPPFLAGS = $(AM_CPPFLAGS) -Wno-overlength-strings
odysia_LDADD = $(GTK_LIBS) $(SQLITE3_LIBS)

odysia-resources.c: $(srcdir)/assets/odysia.gresource.xml $(srcdir)/assets/odysia-icon.png
	$(GLIB_COMPILE_RESOURCES) $< --sourcedir=$(srcdir)/assets --generate-source --target=$@

odysia-resources.h: $(srcdir)/assets/odysia.gresource.xml $(srcdir)/assets/odysia-icon.png
	$(GLIB_COMPILE_RESOURCES) $< --sourcedir=$(srcdir)/assets --generate-header --target=$@

if ODYSIA_WINDOWS
odysia_DEPENDENCIES = odysia-icon.o
odysia_LDADD += odysia-icon.o

odysia-icon.o: $(srcdir)/assets/odysia.rc $(srcdir)/assets/odysia.ico
	cd $(srcdir)/assets && $(WINDRES) -i odysia.rc -o $(abs_builddir)/$@
endif

desktopdir = $(datadir)/applications
dist_desktop_DATA = data/org.odysia.Odysia.desktop
icondir = $(datadir)/icons/hicolor/512x512/apps
dist_icon_DATA = assets/odysia-icon.png

macos-bundle: odysia
	$(MKDIR_P) dist/Odysia.app/Contents/MacOS dist/Odysia.app/Contents/Resources
	$(INSTALL_PROGRAM) odysia dist/Odysia.app/Contents/MacOS/odysia
	$(INSTALL_DATA) $(srcdir)/data/Info.plist dist/Odysia.app/Contents/Info.plist
	$(INSTALL_DATA) $(srcdir)/assets/odysia.icns dist/Odysia.app/Contents/Resources/odysia.icns

.PHONY: macos-bundle

test_indexer_SOURCES = \
	tests/test_indexer.c \
	src/indexer.c \
	src/indexer.h
test_indexer_CPPFLAGS = $(AM_CPPFLAGS) -DTEST_FIXTURE_DIR='"$(abs_top_srcdir)/tests/fixtures/linux_sample"'
test_indexer_LDADD = $(GTK_LIBS) $(SQLITE3_LIBS)

EXTRA_DIST = \
	README.md \
	assets/odysia-icon.svg \
	assets/odysia-icon.png \
	assets/odysia.gresource.xml \
	assets/odysia.ico \
	assets/odysia.icns \
	assets/odysia.rc \
	data/Info.plist \
	tests/fixtures/linux_sample/Kconfig.sample \
	tests/fixtures/linux_sample/Makefile.sample \
	tests/fixtures/linux_sample/arch/sample.S \
	tests/fixtures/linux_sample/arch/sample.lds.S \
	tests/fixtures/linux_sample/drivers/sample.dts \
	tests/fixtures/linux_sample/kernel/sample.c \
	tests/fixtures/linux_sample/include/sample.h \
	tests/fixtures/linux_sample/Documentation/sample.txt \
	tests/fixtures/linux_sample/rust/sample.rs \
	tests/fixtures/linux_sample/scripts/sample-tool \
	tests/fixtures/linux_sample/scripts/sample.awk \
	tests/fixtures/linux_sample/scripts/sample.cocci \
	tests/fixtures/linux_sample/scripts/sample.pl \
	tests/fixtures/linux_sample/scripts/sample.py \
	tests/fixtures/linux_sample/scripts/sample.y
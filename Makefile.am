bin_PROGRAMS = odysia
check_PROGRAMS = test_indexer
TESTS = $(check_PROGRAMS)
AM_CFLAGS = @ODYSIA_WARN_CFLAGS@ $(GTK_CFLAGS) $(SQLITE3_CFLAGS)

odysia_SOURCES = \
	main.c \
	src/indexer.c \
	src/indexer.h \
	src/ui.c \
	src/ui.h
odysia_LDADD = $(GTK_LIBS) $(SQLITE3_LIBS)

test_indexer_SOURCES = \
	tests/test_indexer.c \
	src/indexer.c \
	src/indexer.h
test_indexer_LDADD = $(GTK_LIBS) $(SQLITE3_LIBS)

EXTRA_DIST = \
	README.md \
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
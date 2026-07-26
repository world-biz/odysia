AC_INIT([odysia], [0.5.0-beta5], [support@example.invalid])
AC_CONFIG_SRCDIR([main.c])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_CANONICAL_HOST
AC_PROG_CC
PKG_PROG_PKG_CONFIG

PKG_CHECK_MODULES([GTK], [gtk+-3.0 gio-2.0])
PKG_CHECK_MODULES([SQLITE3], [sqlite3])
AC_PATH_PROG([GLIB_COMPILE_RESOURCES], [glib-compile-resources])
AS_IF([test -z "$GLIB_COMPILE_RESOURCES"],
			[AC_MSG_ERROR([glib-compile-resources is required])])

case "$host_os" in
	mingw* | cygwin*) odysia_windows=yes ;;
	*) odysia_windows=no ;;
esac
AM_CONDITIONAL([ODYSIA_WINDOWS], [test "x$odysia_windows" = "xyes"])
AS_IF([test "x$odysia_windows" = "xyes"], [AC_CHECK_TOOL([WINDRES], [windres])])

ODYSIA_WARN_CFLAGS='-std=c99 -Wall -Wextra -pedantic'
AC_SUBST([ODYSIA_WARN_CFLAGS])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
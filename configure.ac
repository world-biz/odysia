AC_INIT([odysia], [0.3.0-beta3], [support@example.invalid])
AC_CONFIG_SRCDIR([main.c])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_PROG_CC
PKG_PROG_PKG_CONFIG

PKG_CHECK_MODULES([GTK], [gtk+-3.0 gio-2.0])
PKG_CHECK_MODULES([SQLITE3], [sqlite3])

ODYSIA_WARN_CFLAGS='-std=c99 -Wall -Wextra -pedantic -Isrc'
AC_SUBST([ODYSIA_WARN_CFLAGS])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
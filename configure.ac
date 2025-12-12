AC_PREREQ([2.69])
AC_INIT([flexchat],[2.16.2],[https://github.com/hexchat/hexchat])
AC_CONFIG_SRCDIR([src/fe-fltk/fe-fltk.cpp])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign subdir-objects tar-ustar])

AC_PROG_CC
AC_PROG_CXX
AM_PROG_AR
AC_PROG_RANLIB
AC_PROG_INSTALL
AC_PATH_PROG([GLIB_GENMARSHAL],[glib-genmarshal])
AM_PATH_PYTHON([3.0])
AS_IF([test -z "$GLIB_GENMARSHAL"],[
  AC_MSG_ERROR([glib-genmarshal not found; install glib2 development tools])
])
AC_DEFINE([GETTEXT_PACKAGE],["flexchat"],[Package name for gettext])

PKG_CHECK_MODULES([GLIB],[gio-2.0 >= 2.36 glib-2.0])

AC_PATH_PROG([FLTK_CONFIG],[fltk-config])
AS_IF([test -z "$FLTK_CONFIG"],[
  AC_MSG_ERROR([fltk-config not found (install FLTK development files)])
])
FLTK_CXXFLAGS="$($FLTK_CONFIG --cxxflags)"
FLTK_LIBS="$($FLTK_CONFIG --ldflags)"
AC_SUBST([FLTK_CXXFLAGS])
AC_SUBST([FLTK_LIBS])

EXEC_COMMAND="flexchat %U"
AC_SUBST([EXEC_COMMAND])

AC_CHECK_FUNCS([memrchr])

AC_ARG_ENABLE([tls],
  AS_HELP_STRING([--disable-tls],[Build without TLS/SSL support]),
  [], [enable_tls=yes])
AS_IF([test "x$enable_tls" = "xyes"],[
  PKG_CHECK_MODULES([OPENSSL],[openssl >= 0.9.8])
  AC_DEFINE([HAVE_TLS],[1],[Enable TLS])
  AC_DEFINE([OPENSSL_API_COMPAT],[0x10000000L],[Use OpenSSL 1.0 API compat])
  AC_DEFINE([HAVE_X509_GET_SIGNATURE_NID],[1],[Have X509_get_signature_nid()])
  AC_DEFINE([HAVE_SSL_CTX_GET_SSL_METHOD],[1],[Have SSL_CTX_get_ssl_method()])
],[
  OPENSSL_LIBS=""
  OPENSSL_CFLAGS=""
])
AM_CONDITIONAL([ENABLE_TLS],[test "x$enable_tls" = "xyes"])
AC_SUBST([OPENSSL_LIBS])
AC_SUBST([OPENSSL_CFLAGS])

AC_CONFIG_FILES([
Makefile
src/Makefile
src/common/Makefile
src/fe-fltk/Makefile
src/fe-text/Makefile
data/Makefile
data/icons/Makefile
data/misc/Makefile
data/man/Makefile
])

AC_OUTPUT

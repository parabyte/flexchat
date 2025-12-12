AM_CPPFLAGS = \
	-I$(top_builddir) \
	-I$(top_srcdir)/src/common \
	$(GLIB_CFLAGS) \
	$(OPENSSL_CFLAGS) \
	-DHAVE_CONFIG_H

bin_PROGRAMS = flexchat-text

flexchat_text_SOURCES = fe-text.c fe-text.h
flexchat_text_LDADD = \
	$(top_builddir)/src/common/libhexchatcommon.a \
	$(GLIB_LIBS) \
	$(OPENSSL_LIBS)

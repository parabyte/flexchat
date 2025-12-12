AM_CPPFLAGS = \
	-I$(top_builddir) \
	-I$(top_srcdir)/src/common \
	$(GLIB_CFLAGS) \
	$(OPENSSL_CFLAGS) \
	$(FLTK_CXXFLAGS) \
	-DHAVE_CONFIG_H

bin_PROGRAMS = flexchat

flexchat_SOURCES = fe-fltk.cpp
flexchat_LDADD = \
	$(top_builddir)/src/common/libhexchatcommon.a \
	$(GLIB_LIBS) \
	$(OPENSSL_LIBS) \
	$(FLTK_LIBS)

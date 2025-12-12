AM_CPPFLAGS = \
	-I$(top_builddir) \
	-I$(top_srcdir) \
	$(GLIB_CFLAGS) \
	$(OPENSSL_CFLAGS)

noinst_LIBRARIES = libhexchatcommon.a

BUILT_SOURCES = marshal.c marshal.h textevents.h textenums.h
CLEANFILES = $(BUILT_SOURCES)

marshal.c: marshalers.list
	$(GLIB_GENMARSHAL) --prefix=_hexchat_marshal --body $< > $@

marshal.h: marshalers.list
	$(GLIB_GENMARSHAL) --prefix=_hexchat_marshal --header $< > $@

textevents.h textenums.h: textevents.in make-te.py
	$(PYTHON) $(srcdir)/make-te.py $(srcdir)/textevents.in textevents.h textenums.h

libhexchatcommon_a_SOURCES = \
	cfgfiles.c cfgfiles.h \
	chanopt.c chanopt.h \
	ctcp.c ctcp.h \
	dcc.c dcc.h \
	hexchat.c hexchat.h \
	history.c history.h \
	ignore.c ignore.h \
	inbound.c inbound.h \
	modes.c modes.h \
	network.c network.h \
	notify.c notify.h \
	outbound.c outbound.h \
	proto-irc.c proto-irc.h \
	scram.c scram.h \
	server.c server.h \
	servlist.c servlist.h \
	text.c text.h \
	tree.c tree.h \
	url.c url.h \
	userlist.c userlist.h \
	util.c util.h \
	$(BUILT_SOURCES)

if ENABLE_TLS
libhexchatcommon_a_SOURCES += ssl.c
endif

libhexchatcommon_a_CPPFLAGS = $(AM_CPPFLAGS) -DHAVE_CONFIG_H

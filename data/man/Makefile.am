man1dir = $(mandir)/man1
man_MANS = hexchat.1

EXTRA_DIST = hexchat.1.in
CLEANFILES = $(man_MANS)

hexchat.1: hexchat.1.in
	sed -e 's|@VERSION@|$(PACKAGE_VERSION)|g' $< > $@

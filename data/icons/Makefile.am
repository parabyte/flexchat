icondir = $(datadir)/icons/hicolor
appicon_scalable = $(icondir)/scalable/apps
appicon_256 = $(icondir)/256x256/apps
pixmapsdir = $(datadir)/pixmaps
pkgicondir = $(datadir)/hexchat/icons

dist_pkgdata_DATA = \
	book.png \
	tree_channel.png \
	tree_dialog.png \
	tree_server.png \
	tree_util.png \
	tray_fileoffer.png \
	tray_highlight.png \
	tray_message.png \
	ulist_founder.png \
	ulist_halfop.png \
	ulist_netop.png \
	ulist_op.png \
	ulist_owner.png \
	ulist_voice.png

EXTRA_DIST = hexchat-b.svg hexchat-shadowless.svg hexchat.svg hexchat.png hexchat.ico $(dist_pkgdata_DATA)

install-data-local:
	$(MKDIR_P) "$(DESTDIR)$(appicon_scalable)" "$(DESTDIR)$(appicon_256)" "$(DESTDIR)$(pixmapsdir)" "$(DESTDIR)$(pkgicondir)"
	$(INSTALL_DATA) $(srcdir)/hexchat.svg "$(DESTDIR)$(appicon_scalable)/hexchat.svg"
	$(INSTALL_DATA) $(srcdir)/hexchat-b.svg "$(DESTDIR)$(appicon_scalable)/hexchat-b.svg"
	$(INSTALL_DATA) $(srcdir)/hexchat-shadowless.svg "$(DESTDIR)$(appicon_scalable)/hexchat-shadowless.svg"
	$(INSTALL_DATA) $(srcdir)/hexchat.png "$(DESTDIR)$(appicon_256)/hexchat.png"
	$(INSTALL_DATA) $(srcdir)/hexchat.ico "$(DESTDIR)$(pixmapsdir)/hexchat.ico"
	for f in $(dist_pkgdata_DATA); do \
		$(INSTALL_DATA) "$(srcdir)/$$f" "$(DESTDIR)$(pkgicondir)/$$f"; \
	done

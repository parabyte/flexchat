appdatadir = $(datadir)/metainfo
applicationsdir = $(datadir)/applications

desktop_in_in = io.github.Flexchat.desktop.in.in
desktop_in = io.github.Flexchat.desktop.in
desktop_file = io.github.Flexchat.desktop

appdata_in = io.github.Flexchat.appdata.xml.in
appdata_file = io.github.Flexchat.appdata.xml

EXTRA_DIST = $(desktop_in_in) $(appdata_in) io.github.Hexchat.Plugin.metainfo.xml.in io.github.Hexchat.ThemeManager.desktop.in io.github.Hexchat.ThemeManager.xml

all: $(desktop_file) $(appdata_file)

$(desktop_in): $(desktop_in_in)
	sed -e 's|@exec_command@|$(EXEC_COMMAND)|g' $< > $@

$(desktop_file): $(desktop_in)
	cp $(desktop_in) $(desktop_file)

$(appdata_file): $(appdata_in)
	cp $(appdata_in) $(appdata_file)

install-data-local: all
	$(MKDIR_P) "$(DESTDIR)$(applicationsdir)" "$(DESTDIR)$(appdatadir)"
	$(INSTALL_DATA) $(desktop_file) "$(DESTDIR)$(applicationsdir)/$(desktop_file)"
	$(INSTALL_DATA) $(appdata_file) "$(DESTDIR)$(appdatadir)/$(appdata_file)"

CLEANFILES = $(desktop_in) $(desktop_file) $(appdata_file)

PROG = calendar
SRC  = calendar.c io.c day.c ostern.c paskha.c wcslib.c

topdir=../..
include $(topdir)/config.mk

find=cd calendars && find -not -path *.svn* -type $(1) -exec $(2) \;
caldir=$(datadir)/calendar

install-2:
	mkdir -p $(sysconfdir)/calendar $(datadir)/calendar
	install -o root -g root -m 644 default $(sysconfdir)/calendar

	# install the calendar files
	$(call find, d, mkdir -p $(caldir)/{})
	$(call find, f, install -o root -g root -m 644 {} $(caldir)/{})

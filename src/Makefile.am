lib_LTLIBRARIES = libdsss-transfer.la
libdsss_transfer_la_SOURCES = \
  dsssframe.h \
  dsssframegen.c \
  dsssframesync.c \
  dsss-transfer.c \
  dsss-transfer.h \
  gettext.h
libdsss_transfer_la_LDFLAGS = -version-info 1:0:0

bin_PROGRAMS = dsss-transfer dsss-transfer-gui
dsss_transfer_SOURCES = dsss-transfer.h gettext.h main.c
dsss_transfer_CFLAGS = -DLOCALEDIR=\"$(localedir)\"
dsss_transfer_LDADD = libdsss-transfer.la

dsss_transfer_gui_SOURCES = dsss-transfer.h gettext.h gui.c
dsss_transfer_gui_CFLAGS = -DLOCALEDIR=\"$(localedir)\" $(GTK_CFLAGS)
dsss_transfer_gui_LDADD = libdsss-transfer.la $(GTK_LIBS)

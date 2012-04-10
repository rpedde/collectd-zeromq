pkglibdir=$(libdir)/collectd
pkglib_LTLIBRARIES = zeromq.la
zeromq_la_SOURCES = zeromq.c zeromq.h
zeromq_la_LDFLAGS = -module -avoid-version -shared $(LIBZMQ_LIBS)
zeromq_la_CFLAGS = $(COLLECTD_FLAGS) $(LIBZMQ_CFLAGS)

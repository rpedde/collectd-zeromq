pkglibdir=$(libdir)/collectd
pkglib_LTLIBRARIES = zeromq.la
zeromq_la_SOURCES = zeromq.c zeromq.h
zeromq_la_LDFLAGS = -module -avoid-version -shared $(ZMQ_LIBS)
zeromq_la_CFLAGS = $(COLLECTD_CFLAGS) $(ZMQ_CFLAGS)

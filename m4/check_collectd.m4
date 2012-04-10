dnl Check for collectd
dnl On success, HAVE_COLLECTD is set to 1

AC_DEFUN([CHECK_COLLECTD],
[
  AC_MSG_CHECKING([collectd])
  if test x"$collectd_required" = "xyes"; then
    HAVE_COLLECTD=0

    # Search using pkg-config
    if test x"$HAVE_COLLECTD" = "x0"; then
      if test x"$PKG_CONFIG" != "x"; then
        PKG_CHECK_MODULES([COLLECTD], [collectd], [HAVE_COLLECTD=1], [HAVE_COLLECTD=0])
      fi
    fi

    for i in /usr /usr/local /opt/local /opt; do
      if test -f "$i/include/collectd/collectd.h"; then
        COLLECTD_CFLAGS="-I$i/include/collectd"
	HAVE_COLLECTD_HEADERS=1
	break;
      fi

      if test -f "$i/include/collectd.h"; then
        COLLECTD_CFLAGS="-I$i/include"
	HAVE_COLLECTD_HEADERS=1
	break;
      fi
    done

    if test x"$HAVE_COLLECTD_HEADERS" = "x1"; then
       dnl try and find the libs... we don't NEED these, unless someone
       dnl wants to links with an existing module for some reason
       for i in /usr /usr/local /opt/local /opt; do
         if test -f "$i/lib/collectd/load.so"; then
           COLLECTD_CFLAGS+=" -L$i/lib/collectd"
	   HAVE_COLLECTD=1
	   break;
	 fi

         if test -f "$i/lib/load.so"; then
           COLLECTD_CFLAGS+=" -L$i/lib"
	   HAVE_COLLECTD=1
	   break;
	 fi
       done
    fi

    if test x"$HAVE_COLLECTD" = "x0"; then
      AC_MSG_ERROR([collectd is mandatory.])
    fi

    AC_SUBST(COLLECTD_CFLAGS)
  fi
])

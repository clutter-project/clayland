AC_INIT([test-wayland-surface], 0.1)

AM_CONFIG_HEADER([config.h])

AM_INIT_AUTOMAKE([1.11 foreign])

m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_PROG_CC
AC_PROG_RANLIB

dnl Check if we need to link to -lm
AC_SEARCH_LIBS([atan2f], [m], [have_atan2f=yes], [have_atan2f=no])
AS_IF([test "x$have_atan2f" = "xno"],
      AC_MSG_ERROR([Could not find math library]))

PKG_CHECK_MODULES(CLUTTER, [clutter-1.0])
PKG_CHECK_MODULES(COGL, [cogl-2.0-experimental])

AC_PATH_PROG([GLIB_GENMARSHAL], [glib-genmarshal])
AC_PATH_PROG([GLIB_MKENUMS], [glib-mkenums])

dnl The test needs to refer to the xserver.xml protocol extension
dnl that's currently part of the wayland demo compositor repo, so we
dnl need this option to be told where that can be found.
AC_ARG_WITH([wayland-protocols],
            [AS_HELP_STRING([--with-wayland-protocols], [Location for wayland extension protocol specs])],
            [
              AC_PATH_PROG([WAYLAND_SCANNER],[wayland-scanner],[no])
              AS_IF([test "x$WAYLAND_SCANNER" = "xno"],
                    AC_MSG_ERROR([Could not find wayland-scanner in your PATH, required for parsing wayland extension protocols]))
              AC_SUBST([WAYLAND_SCANNER])
              AC_SUBST([WAYLAND_EXTENSION_PROTOCOLS_DIR],$withval)
            ],
            [
              AC_MSG_ERROR([Please specify the location of the Wayland protocol extensions with --with-wayland-protocols=DIR])
            ])

AC_CONFIG_FILES([
        Makefile
        src/Makefile
])

AC_OUTPUT

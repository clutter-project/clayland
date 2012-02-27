bin_PROGRAMS = test-wayland-surface

INCLUDES = \
	@CLUTTER_CFLAGS@ \
	@COGL_CFLAGS@ \
	-DXWAYLAND_PATH='"@XWAYLAND_PATH@"'

test_wayland_surface_SOURCES = \
	test-wayland-surface.c \
	tws-compositor.h \
	tws-input.c \
	tws-input.h \
	xserver-protocol.c \
	xserver-server-protocol.h

test-wayland-surface.c : xserver-server-protocol.h

test_wayland_surface_LDADD = \
	@CLUTTER_LIBS@ \
	@COGL_LIBS@

%-protocol.c : @WAYLAND_EXTENSION_PROTOCOLS_DIR@/%.xml
	$(AM_V_GEN)$(WAYLAND_SCANNER) code < $< > $@
%-server-protocol.h : @WAYLAND_EXTENSION_PROTOCOLS_DIR@/%.xml
	$(AM_V_GEN)$(WAYLAND_SCANNER) server-header < $< > $@
%-client-protocol.h : @WAYLAND_EXTENSION_PROTOCOLS_DIR@/%.xml'
	$(AM_V_GEN)$(WAYLAND_SCANNER) client-header < $< > $@

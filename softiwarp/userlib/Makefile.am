
lib_LTLIBRARIES = src/libsiw.la

AM_CFLAGS = -g -Wall -D_GNU_SOURCE
AM_CFLAGS += -I../common -I @RDMA_CORE_DIR@/build/include -DFREE_CONTEXT_IN_CTX_OPS=@FREE_CONTEXT_IN_CTX_OPS@ -DQUERY_DEVICE_IN_CTX_OPS=@QUERY_DEVICE_IN_CTX_OPS@

if HAVE_LD_VERSION_SCRIPT
	siw_version_script = -Wl,--version-script=$(srcdir)/src/siw.map
else
	siw_version_script =
endif

BUILT_SOURCES = siw_user_abi.h
CLEANFILES = siw_user_abi.h

siw_user_abi.h: ../common/siw_user.h
	python3 @RDMA_CORE_DIR@/buildlib/make_abi_structs.py ../common/siw_user.h ./siw_user_abi.h

src_libsiw_la_SOURCES = src/siw_verbs.c src/siw.c src/siw_uverbs.c

src_libsiw_la_LDFLAGS = -avoid-version -release @LIBSIW_RELEASE@ \
    $(siw_version_script)

siwconfdir = $(sysconfdir)/libibverbs.d
siwconf_DATA = siw.driver

EXTRA_DIST = src/siw.h src/siw_abi.h src/siw.map libsiw.spec.in \
	siw.driver

dist-hook: libsiw.spec
	cp libsiw.spec $(distdir)


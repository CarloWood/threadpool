AM_CPPFLAGS = -iquote $(top_srcdir) -iquote $(top_srcdir)/cwds

SUBDIRS =

noinst_LTLIBRARIES = libthreadpool.la

libthreadpool_la_SOURCES = \
	AIDelayedFunction.h \
	AIPackagedTask.h \
	AIThreadPool.cxx \
	AIThreadPool.h \
	Timer.cxx \
	Timer.h \
	TimerQueue.h \
	RunningTimers.cxx \
	RunningTimers.h

libthreadpool_la_CXXFLAGS = @LIBCWD_R_FLAGS@
libthreadpool_la_LIBADD = -lrt @LIBCWD_R_LIBS@

# --------------- Maintainer's Section

if MAINTAINER_MODE
distclean-local:
	-rm -f *.s *.ii

clean-local:
endif

MAINTAINERCLEANFILES = $(srcdir)/Makefile.in

#include <functional>

// Include this from the debug.h in the root of the project if your project uses colors for debug output.

#ifdef HAVE_UTILS_CONFIG_H
extern thread_local std::function<void()> g_thread_pool_use_color_tl;

// Redefined to call g_thread_pool_use_color_tl().
#undef LibcwDoutScopeBegin
#define LibcwDoutScopeBegin(dc_namespace, debug_obj, cntrl)                                                                     \
  do																\
  {																\
    LIBCWD_TSD_DECLARATION;													\
    LIBCWD_ASSERT_NOT_INTERNAL;													\
    LIBCWD_LibcwDoutScopeBegin_MARKER;												\
    if (LIBCWD_DO_TSD_MEMBER_OFF(debug_obj) < 0)										\
    {																\
      using namespace ::libcwd;												        \
      ::libcwd::channel_set_bootstrap_st __libcwd_channel_set(LIBCWD_DO_TSD(debug_obj) LIBCWD_COMMA_TSD);			\
      bool on;															\
      {																\
        using namespace dc_namespace;												\
	on = (__libcwd_channel_set|cntrl).on;											\
      }																\
      if (on)															\
      {																\
        if (g_thread_pool_use_color_tl)                                                                                         \
          g_thread_pool_use_color_tl();                                                                                         \
	::libcwd::debug_ct& __libcwd_debug_object(debug_obj);								        \
	LIBCWD_DO_TSD(__libcwd_debug_object).start(__libcwd_debug_object, __libcwd_channel_set LIBCWD_COMMA_TSD);

#endif // HAVE_UTILS_CONFIG_H

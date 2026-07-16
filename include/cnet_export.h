#ifndef CNET_EXPORT_H
#define CNET_EXPORT_H

/* Export macros for building cnet as shared library (for .NET P/Invoke and other hosts).
 * Usage: make cnet_dll
 * Then P/Invoke the CNET_API functions or the soul_host shim.
 */

#ifdef CNET_BUILD_DLL
#  ifdef _WIN32
#    define CNET_API __declspec(dllexport)
#    define CNET_INTERNAL
#  else
#    define CNET_API __attribute__((visibility("default")))
#    define CNET_INTERNAL __attribute__((visibility("hidden")))
#  endif
#else
#  define CNET_API
#  define CNET_INTERNAL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Marker for public ABI. */

#ifdef __cplusplus
}
#endif

#endif /* CNET_EXPORT_H */

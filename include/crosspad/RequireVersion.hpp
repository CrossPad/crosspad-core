/*
 * Compile-time dependency version gate. Deliberately has no include guard —
 * include it once per dependency, after defining what you have and what you
 * were written against:
 *
 *   #define CROSSPAD_REQ_HAVE_MAJOR CROSSPAD_CORE_VERSION_MAJOR
 *   #define CROSSPAD_REQ_HAVE_MINOR CROSSPAD_CORE_VERSION_MINOR
 *   #define CROSSPAD_REQ_HAVE_PATCH CROSSPAD_CORE_VERSION_PATCH
 *   #define CROSSPAD_REQ_WANT_MAJOR 0
 *   #define CROSSPAD_REQ_WANT_MINOR 1
 *   #define CROSSPAD_REQ_WANT_PATCH 0
 *   #include "crosspad/RequireVersion.hpp"
 *
 * Major differs      -> #error   (the API you compiled against is gone)
 * Minor/patch differ -> #warning (builds, but you are not on the version this
 *                                 code was tested with — bump the WANT_* or
 *                                 the submodule, whichever is stale)
 *
 * Preprocessor diagnostics cannot expand macros, so the message names the
 * including file rather than the dependency; the compiler's include chain
 * shows which one it was.
 *
 * Every macro is undefined at the end so the next include starts clean.
 */

#if !defined(CROSSPAD_REQ_HAVE_MAJOR) || !defined(CROSSPAD_REQ_WANT_MAJOR)
#error "crosspad/RequireVersion.hpp: define CROSSPAD_REQ_HAVE_* and CROSSPAD_REQ_WANT_* before including"
#endif

#if CROSSPAD_REQ_HAVE_MAJOR != CROSSPAD_REQ_WANT_MAJOR
#error "crosspad dependency MAJOR version mismatch: the submodule you have is incompatible with what this code was written for (see the file that included crosspad/RequireVersion.hpp, then update the submodule or the CROSSPAD_REQ_WANT_* it declares)"
#elif CROSSPAD_REQ_HAVE_MINOR != CROSSPAD_REQ_WANT_MINOR
#  if defined(_MSC_VER) && !defined(__clang__)   /* MSVC has no #warning */
#    pragma message("crosspad dependency MINOR version differs from what this code was written for (see the file that included crosspad/RequireVersion.hpp) — probably fine, but you are not on a tested pairing")
#  else
#    warning "crosspad dependency MINOR version differs from what this code was written for (see the file that included crosspad/RequireVersion.hpp) — probably fine, but you are not on a tested pairing"
#  endif
#elif CROSSPAD_REQ_HAVE_PATCH != CROSSPAD_REQ_WANT_PATCH
#  if defined(_MSC_VER) && !defined(__clang__)   /* MSVC has no #warning */
#    pragma message("crosspad dependency PATCH version differs from what this code was written for (see the file that included crosspad/RequireVersion.hpp)")
#  else
#    warning "crosspad dependency PATCH version differs from what this code was written for (see the file that included crosspad/RequireVersion.hpp)"
#  endif
#endif

#undef CROSSPAD_REQ_HAVE_MAJOR
#undef CROSSPAD_REQ_HAVE_MINOR
#undef CROSSPAD_REQ_HAVE_PATCH
#undef CROSSPAD_REQ_WANT_MAJOR
#undef CROSSPAD_REQ_WANT_MINOR
#undef CROSSPAD_REQ_WANT_PATCH

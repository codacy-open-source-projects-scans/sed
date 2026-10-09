# Copyright (C) 1993-2026 Free Software Foundation, Inc.

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

dnl Process this file with -*- autoconf -*- to produce a configure script.
AC_INIT([GNU sed],
        m4_esyscmd([build-aux/git-version-gen .tarball-version]),
        [bug-sed@gnu.org])
AC_CONFIG_AUX_DIR(build-aux)
AC_CONFIG_SRCDIR([sed/sed.c])
AC_CONFIG_HEADERS([config.h:config_h.in])
AC_PREREQ([2.64])
AM_INIT_AUTOMAKE([1.11.1 dist-xz color-tests parallel-tests subdir-objects])
AM_SILENT_RULES([yes]) # make --enable-silent-rules the default.
AC_CONFIG_MACRO_DIR([m4])

AC_PROG_CC
AM_PROG_CC_C_O
gl_EARLY
gl_INIT
gl_DISABLE_THREADS

AC_DEFINE([GNULIB_EXCLUDE_SINGLE_THREAD], [1],
  [Define if all programs in this package call functions of the Gnulib
   'exclude' module only from a single thread.])
AC_DEFINE([GNULIB_LOCALENAME_SINGLE_THREAD], [1],
  [Define if all programs in this package call functions of the Gnulib
   'getlocalename_l-simple', 'getlocalename_l-unsafe', and 'localename'
   modules only from a single thread.])
AC_DEFINE([GNULIB_MBRTOWC_SINGLE_THREAD], [1],
  [Define if all programs in this package call functions of the Gnulib
   'mbtowc' module only from a single thread.])
AC_DEFINE([GNULIB_REGEX_SINGLE_THREAD], [1],
  [Define if all programs in this package call functions of the Gnulib
   'regex' module only from a single thread.])
AC_DEFINE([GNULIB_WCHAR_SINGLE_LOCALE], [1],
  [Define if all programs in this package call locale-sensitive functions
   like mbrtowc only after setting the locale, and never change the
   locale once set.])

AC_DEFINE([GNULIB_MBRTOC32_REGULAR], [1],
  [Do not worry about rare encodings like CP864, EBCDIC, Johab, and Shift JIS
   that glibc does not support.])

# Ensure VLAs are not used.
# Note -Wvla is implicitly added by gl_MANYWARN_ALL_GCC
AC_DEFINE([GNULIB_NO_VLA], [1], [Define to 1 to disable use of VLAs])

# The test suite needs to know if we have a working perl.
AM_CONDITIONAL([HAVE_PERL], [test "$gl_cv_prog_perl" != no])

# gl_GCC_VERSION_IFELSE([major], [minor], [run-if-found], [run-if-not-found])
# ------------------------------------------------
# If $CPP is gcc-MAJOR.MINOR or newer, then run RUN-IF-FOUND.
# Otherwise, run RUN-IF-NOT-FOUND.
AC_DEFUN([gl_GCC_VERSION_IFELSE],
  [AC_PREPROC_IFELSE(
    [AC_LANG_PROGRAM(
      [[
#if ($1) < __GNUC__ || (($1) == __GNUC__ && ($2) <= __GNUC_MINOR__)
/* ok */
#else
# error "your version of gcc is older than $1.$2"
#endif
      ]]),
    ], [$3], [$4])
  ]
)

AC_CACHE_CHECK([whether -lcP is needed], [sed_cv_libcp_needed], [
AC_RUN_IFELSE(
  [AC_LANG_SOURCE([[
#include <stdio.h>
#include <errno.h>

int main()
{
  FILE *fp;
  int result;
  errno = 0;
  fp = fopen ("conftest.c", "r");
  if (!fp) return 0;	   /* error, assume not needed */
  result = fflush (fp) == EOF && errno == 0;
  fclose (fp);
  return result;
}]])], [sed_cv_libcp_needed=no],
    [sed_cv_libcp_needed=yes],
    [sed_cv_libcp_needed="assuming no"])
])
if test "$sed_cv_libcp_needed" = yes; then
  LIBS="-lcP $LIBS"
fi

AM_GNU_GETTEXT_VERSION([0.19.2])
AM_GNU_GETTEXT([external])

AC_CHECK_FUNCS_ONCE([readlink popen fchown setlocale])

AM_CONDITIONAL([TEST_SYMLINKS],
          [test "$ac_cv_func_readlink" = yes])

XFAIL_TESTS=

# Under MinGW, the bsd.sh test fails because of the EOF character (^Z).
case $host in
  *mingw*) XFAIL_TESTS="$XFAIL_TESTS bsd" ;;
esac
AC_SUBST([XFAIL_TESTS])

AC_ARG_ENABLE([gcc-warnings],
  [AS_HELP_STRING([--enable-gcc-warnings@<:@=TYPE@:>@],
    [control generation of GCC warnings.  The TYPE 'no' disables
     warnings (default for non-developer builds); 'yes' generates
     cheap warnings if available (default for developer builds);
     'expensive' in addition generates expensive-to-compute warnings
     if available.])],
  [case $enableval in
     no|yes|expensive) ;;
     *)      AC_MSG_ERROR([bad value $enableval for gcc-warnings option]) ;;
   esac
   gl_gcc_warnings=$enableval],
  [
   # GCC provides fine-grained control over diagnostics which
   # is used in gnulib for example to suppress warnings from
   # certain sections of code.  So if this is available and
   # we're running from a git repo, then auto enable the warnings.
   gl_gcc_warnings=no
   gl_GCC_VERSION_IFELSE([4], [6],
                         [test -d "$srcdir"/.git \
                          && ! test -f "$srcdir"/.tarball-version \
                          && gl_gcc_warnings=yes])]
)

# clang is unduly picky about some things, even by default.
if test "$gl_cv_compiler_clang" = yes; then
  gl_WARN_ADD([-Wno-format-extra-args])
  gl_WARN_ADD([-Wno-implicit-const-int-float-conversion])
  gl_WARN_ADD([-Wno-tautological-constant-out-of-range-compare])
fi

if test $gl_gcc_warnings != no; then
  gl_WARN_ADD([-Werror], [WERROR_CFLAGS])
  AC_SUBST([WERROR_CFLAGS])

  ew=
  AS_IF([test $gl_gcc_warnings != expensive],
    [# -fanalyzer and related options slow GCC considerably.
     ew="$ew -fanalyzer -Wno-analyzer-malloc-leak"])

  # This, $nw, is the list of warnings we disable.
  nw=$ew
  nw="$nw -Wstack-protector"        # not worth working around for pre GCC 15
  nw="$nw -Wformat-overflow=2"      # False alarms due to GCC bug 110333
  nw="$nw -Wformat-truncation=2"    # False alarm in ls.c, probably related
  nw="$nw -Winline"                 # system.h's readdir_ignoring_dot_and_dotdot

  # Using -Wstrict-overflow is a pain, but the alternative is worse.
  # For an example, see the code that provoked this report:
  # https://gcc.gnu.org/PR33498
  # Code like that still infloops with gcc-4.6.0 and -O2.  Scary indeed.

  gl_MANYWARN_ALL_GCC([ws])
  AS_VAR_APPEND([ws], [' -Wswitch-enum'])
  AS_VAR_APPEND([ws], [' -Wtrailing-whitespace'])
  gl_MANYWARN_COMPLEMENT([ws], [$ws], [$nw])
  for w in $ws; do
    gl_WARN_ADD([$w])
  done
  gl_WARN_ADD([-Wno-sign-compare])     # Too many warnings for now
  gl_WARN_ADD([-Wno-format-nonliteral])

  AC_SUBST([WARN_CFLAGS])

  AC_DEFINE([lint], [1], [Define to 1 if the compiler is checking for lint.])
  AH_VERBATIM([FORTIFY_SOURCE],
  [/* Enable compile-time and run-time bounds-checking, and some warnings,
      without upsetting glibc 2.15+. */
   #if !defined _FORTIFY_SOURCE && defined __OPTIMIZE__ && __OPTIMIZE__
   # define _FORTIFY_SOURCE 2
   #endif
])
  AC_DEFINE([GNULIB_PORTCHECK], [1], [enable some gnulib portability checks])

  # We use a slightly smaller set of warning options for lib/.
  # Remove the following and save the result in GNULIB_WARN_CFLAGS.
  nw=$ew
  nw="$nw -Wduplicated-branches"    # Too many false alarms
  nw="$nw -Wformat-truncation=2"
  nw="$nw -Wunused-macros"

  gl_MANYWARN_COMPLEMENT([GNULIB_WARN_CFLAGS], [$WARN_CFLAGS], [$nw])
  AC_SUBST([GNULIB_WARN_CFLAGS])

  # For gnulib-tests, the set is slightly smaller still.
  nw=
  # It's not worth being this picky about test programs.
  nw="$nw -Wmissing-variable-declarations"
  nw="$nw -Wsuggest-attribute=cold"
  nw="$nw -Wsuggest-attribute=const"
  nw="$nw -Wsuggest-attribute=format"
  nw="$nw -Wsuggest-attribute=pure"
  gl_MANYWARN_COMPLEMENT([GNULIB_TEST_WARN_CFLAGS],
                         [$GNULIB_WARN_CFLAGS], [$nw])
  AC_SUBST([GNULIB_TEST_WARN_CFLAGS])
fi

AC_ARG_ENABLE([bold-man-page-references],
  [AS_HELP_STRING([--disable-bold-man-page-references],
     [When generating man pages, do not apply bold style around any
      references like name(1) etc.])],
  [gl_bold_manpages=yes ;
   case $enableval in
     no|yes) gl_bold_manpages=$enableval ;;
     *)      AC_MSG_ERROR([bad value $enableval for bold-man-page-references.
                           Options are: yes, no.]) ;;
   esac],
  [gl_bold_manpages=yes]
)
AM_CONDITIONAL([BOLD_MAN_REFS], [test "$gl_bold_manpages" != no])

AM_CONDITIONAL([CROSS_COMPILING], [test "$cross_compiling" = yes])

# Perl is needed for help2man
AC_PATH_PROG([PERL], [perl])

# This is needed when building outside the source dir
# with --disable-dependency-tracking, see https://bugs.gnu.org/25371
AS_MKDIR_P([lib])
AS_MKDIR_P([sed])
AS_MKDIR_P([doc])
AS_MKDIR_P([testsuite])

AC_CONFIG_FILES([
  Makefile
  po/Makefile.in
  gnulib-tests/Makefile
])
AC_OUTPUT

/*  Functions from hack's utils library.
    Copyright (C) 1989-2026 Free Software Foundation, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3, or (at your option)
    any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; If not, see <https://www.gnu.org/licenses/>. */

#include <config.h>

#include "utils.h"

#include <binary-io.h>
#include <fwriting.h>
#include <min-eloop-threshold.h>
#include <minmax.h>
#include <progname.h>
#include <quotearg.h>
#include <xalloc.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gettext.h>
#define _(msgid) gettext (msgid)

#ifdef SSIZE_MAX
# define SSIZE_IDX_MAX MIN (SSIZE_MAX, IDX_MAX)
#else
# define SSIZE_IDX_MAX IDX_MAX
#endif

#if O_BINARY
extern bool binary_mode;
#endif

/* Store information about files opened with ck_fopen
   so that error messages from ck_fread, ck_fwrite, etc. can print the
   name of the file that had the error */

struct open_file
  {
    FILE *fp;
    char *name;
    struct open_file *link;
  };

static struct open_file *open_files = NULL;
static void do_ck_fclose (FILE *, char const *);

/* Print an error message and exit */

void
panic (const char *str, ...)
{
  va_list ap;

  fprintf (stderr, "%s: ", program_name);
  va_start (ap, str);
  vfprintf (stderr, str, ap);
  va_end (ap);
  putc ('\n', stderr);

#ifdef PACIFY_LSAN
  while (open_files)
    {
      struct open_file *next = open_files->link;
      free (open_files->name);
      free (open_files);
      open_files = next;
    }
#endif

  exit (EXIT_PANIC);
}

/* Quote file names.  */

static char *
quotef_n (int n, char const *arg)
{
  return quotearg_n_style_colon (n, shell_escape_quoting_style, arg);
}

char *
quotef (char const *arg)
{
  return quotef_n (0, arg);
}

/* Internal routine to get a filename from open_files */
static const char * _GL_ATTRIBUTE_PURE
utils_fp_name (FILE *fp)
{
  struct open_file *p;

  for (p=open_files; p; p=p->link)
    if (p->fp == fp)
      return p->name;
  if (fp == stdin)
    return "stdin";
  else if (fp == stdout)
    return "stdout";
  else if (fp == stderr)
    return "stderr";

  return "<unknown>";
}

static void
register_open_file (FILE *fp, const char *name)
{
  struct open_file *p;
  p = xmalloc (sizeof *p);
  p->link = open_files;
  open_files = p;
  p->name = xstrdup (name);
  p->fp = fp;
}

/* Panic on failing fopen */
FILE *
ck_fopen (const char *name, const char *mode, int fail)
{
  FILE *fp;

  fp = fopen (name, mode);
  if (!fp)
    {
      if (fail)
        panic (_("couldn't open file %s: %s"), quotef (name), strerror (errno));

      return NULL;
    }

  register_open_file (fp, name);
  return fp;
}

#if defined WIN32 || defined _WIN32 || defined __CYGWIN__ || defined MSDOS
/* Panic on failing fdopen */
FILE *
ck_fdopen ( int fd, const char *name, const char *mode, int fail)
{
  FILE *fp;

  fp = fdopen (fd, mode);
  if (!fp)
    {
      if (fail)
        panic (_("couldn't attach to %s: %s"), quotef (name), strerror (errno));

      return NULL;
    }

  register_open_file (fp, name);
  return fp;
}
#endif

/* When we've created a temporary for an in-place update,
   we may have to exit before the rename.  This is the name
   of the temporary that we'll have to unlink via an atexit-
   registered cleanup function.  */
static char const *G_file_to_unlink;

void
remove_cleanup_file (void)
{
  if (G_file_to_unlink)
    unlink (G_file_to_unlink);
}

/* Note that FILE must be removed upon exit.  */
static void
register_cleanup_file (char const *file)
{
  G_file_to_unlink = file;
}

/* Clear the global file-to-unlink global.  */
void
cancel_cleanup (void)
{
  G_file_to_unlink = NULL;
}

FILE *
ck_mkstemp (char **p_filename, const char *tmpdir,
            const char *base, const char *mode)
{
  idx_t tmpdirlen = strlen (tmpdir), baselen = strlen (base);
  char *template = xmalloc (tmpdirlen + baselen + 8);
  char *basecopy = mempcpy (template, tmpdir, tmpdirlen);
  *basecopy++ = '/';
  char *suffix = mempcpy (basecopy, base, baselen);
  memset (suffix, 'X', 6);
  suffix[6] = '\0';

   /* The ownership might change, so omit some permissions at first
      so unauthorized users cannot nip in before the file is ready.
      mkstemp forces O_BINARY on cygwin, so use mkostemp instead.  */
  mode_t save_umask = umask (0077);
  int fd = mkostemp (template, 0);
  int err = errno;
  umask (save_umask);
  FILE *fp = NULL;

  if (0 <= fd)
    {
      *p_filename = template;
      register_cleanup_file (template);

#if O_BINARY
      if (binary_mode && set_binary_mode (fd, O_BINARY) == -1)
        panic (_("%s: failed to set binary mode"), quotef (template));
#endif

      fp = fdopen (fd, mode);
      err = errno;
    }

  if (!fp)
    panic (_("couldn't open temporary file %s: %s"), quotef (template),
           strerror (err));

  register_open_file (fp, template);
  return fp;
}

/* Panic on failing fwrite */
void
ck_fwrite (const void *ptr, idx_t size, idx_t nmemb, FILE *stream)
{
  clearerr (stream);
  if (size && fwrite (ptr, size, nmemb, stream) != nmemb)
    panic (ngettext ("couldn't write %td item to %s: %s",
                     "couldn't write %td items to %s: %s", nmemb),
           nmemb, quotef (utils_fp_name (stream)),
           strerror (errno));
}

/* Panic on failing fread */
idx_t
ck_fread (void *ptr, idx_t size, idx_t nmemb, FILE *stream)
{
  clearerr (stream);
  if (size && (nmemb=fread (ptr, size, nmemb, stream)) <= 0 && ferror (stream))
    panic (_("read error on %s: %s"), quotef (utils_fp_name (stream)),
           strerror (errno));

  return nmemb;
}

ssize_t
ck_getdelim (char **text, size_t *buflen, char delim, FILE *stream)
{
  if (ferror (stream))
    panic (_("read error on %s"), quotef (utils_fp_name (stream)));

  ssize_t result = getdelim (text, buflen, delim, stream);
  if (ferror (stream)) /* implies result < 0, hence errno is set */
    panic (_("read error on %s: %s"), quotef (utils_fp_name (stream)),
           strerror (errno));

  return result;
}

/* Panic on failing fflush */
void
ck_fflush (FILE *stream)
{
  if (!fwriting (stream))
    return;

  clearerr (stream);
  if (fflush (stream) == EOF && errno != EBADF)
    panic ("couldn't flush %s: %s", quotef (utils_fp_name (stream)),
           strerror (errno));
}

/* Panic on failing fclose */
void
ck_fclose (FILE *stream)
{
  struct open_file **prev = &open_files;
  struct open_file *cur;

  /* a NULL stream means to close all files */
  while ((cur = *prev))
    {
      if (!stream || stream == cur->fp)
        {
          FILE *fp = cur->fp;
          *prev = cur->link;
          do_ck_fclose (fp, cur->name);
          free (cur->name);
          free (cur);
        }
      else
        prev = &cur->link;
    }

  /* Also care about stdout, because if it is redirected the
     last output operations might fail and it is important
     to signal this as an error (perhaps to make). */
  if (!stream)
    do_ck_fclose (stdout, "stdout");
}

/* Close a single file. */
static void
do_ck_fclose (FILE *fp, char const *name)
{
  ck_fflush (fp);
  clearerr (fp);

  if (fclose (fp) == EOF)
    panic ("couldn't close %s: %s", quotef (name), strerror (errno));
}

/* Follow symlink FNAME and return the ultimate target, stored in a
   temporary buffer that the caller should not free.  Return FNAME if
   it is not a symlink.  Panic if a symlink loop is found.  */
const char *
follow_symlink (const char *fname)
{
  /* The file name, as adjusted so far by replacing symlinks with
     their contents.  Only the last file name component is replaced,
     as we need not do all the work of realpath.  */

  /* FIXME: We should get a file descriptor on the parent directory,
     to avoid resolving that directory name more than once (which can
     lead to races).  Perhaps someday the Gnulib 'supersede' module
     can get a function openat_supersede that will do this for us.  */
  char const *fn = fname;

#ifdef HAVE_READLINK
  static char *buf;
  static idx_t buf_size;

  idx_t buf_used = 0;

  for (idx_t num_links = 0; ; num_links++)
    {
      ssize_t linklen;
      idx_t newlen;
      char const *c;

      /* Put symlink contents into BUF + BUF_USED.  */
      while ((linklen = (buf_used < buf_size
                         ? readlink (fn, buf + buf_used, buf_size - buf_used)
                         : 0))
             == buf_size - buf_used)
        {
          buf = xpalloc (buf, &buf_size, 1, SSIZE_IDX_MAX, 1);
          if (num_links)
            fn = buf;
        }
      if (linklen < 0)
        {
          if (errno == EINVAL)
            break;
          panic (_("couldn't readlink %s: %s"), quotef (fn), strerror (errno));
        }
      if (MIN_ELOOP_THRESHOLD <= num_links)
        panic (_("couldn't follow symlink %s: %s"), quotef (fname),
               strerror (ELOOP));

      if ((linklen == 0 || buf[buf_used] != '/') && (c = strrchr (fn, '/')))
        {
          /* A relative symlink not from the working directory.
             Make sure BUF is big enough.  */
          idx_t dirlen = c - fn + 1;
          newlen = dirlen + linklen;
          if (buf_size <= newlen)
            {
              buf = xpalloc (buf, &buf_size, newlen + 1 - buf_size,
                             SSIZE_IDX_MAX, 1);
              if (num_links)
                fn = buf;
            }

          /* Store the new file name in BUF.  Beware overlap.  */
          memmove (buf + dirlen, buf + buf_used, linklen);
          if (fn != buf)
            memcpy (buf, fn, dirlen);
        }
      else
        {
          /* A symlink to an absolute file name, or a relative symlink
             from the working directory.  The new file name is simply
             the symlink contents.  */
          memmove (buf, buf + buf_used, linklen);
          newlen = linklen;
        }

      buf[newlen] = '\0';
      buf_used = newlen + 1;
      fn = buf;
    }
#endif

  return fn;
}

/* Panic on failing rename */
void
ck_rename (const char *from, const char *to)
{
  int rd = rename (from, to);
  if (rd != -1)
    return;

  panic (_("cannot rename %s to %s: %s"),
         quotef_n (0, from), quotef_n (1, to), strerror (errno));
}




/* Implement a variable sized buffer of 'stuff'.  We don't know what it is,
nor do we care, as long as it doesn't mind being aligned by malloc. */

struct buffer
  {
    idx_t allocated;
    idx_t length;
    char *b;
  };

struct buffer *
init_buffer (void)
{
  struct buffer *b = XNMALLOC (1, struct buffer);
  b->allocated = b->length = 0;
  b->b = xpalloc (NULL, &b->allocated, 1, -1, 1);
  return b;
}

char *
get_buffer (struct buffer const *b)
{
  return b->b;
}

idx_t
size_buffer (struct buffer const *b)
{
  return b->length;
}

void
add1_buffer (struct buffer *b, char c)
{
  /* This special case should be kept cheap;
   *  don't generalize it by using memcpy -- even "builtin"
   *  versions of memcpy(a, b, 1) can become
   *  expensive when called too often.
   */
  if (b->length == b->allocated)
    b->b = xpalloc (b->b, &b->allocated, 1, -1, 1);
  b->b[b->length++] = c;
}

void
free_buffer (struct buffer *b)
{
  if (b)
    {
      free (b->b);
      free (b);
    }
}

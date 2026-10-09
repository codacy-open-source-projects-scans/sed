/*  GNU SED, a batch stream editor.
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

/* compile.c: translate sed source into internal form */

#include "sed.h"

#include <c-ctype.h>
#include <minmax.h>
#include <progname.h>
#include <read-file.h>
#include <xalloc.h>

#include <errno.h>
#include <obstack.h>
#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>

#define obstack_chunk_alloc  xzalloc
#define obstack_chunk_free   free

/* let's not confuse text editors that have only dumb bracket-matching... */
#define OPEN_BRACKET	'['
#define CLOSE_BRACKET	']'
/* #define OPEN_BRACE	'{' */
#define CLOSE_BRACE	'}'

struct prog_info {
  /* 'prog.base' points to the first character in the string, 'prog.cur' points
     to the current character in the string, 'prog.prev' points to the
     previous character in the string, and 'prog.end' points
     to the current character in the string, and 'prog.end' points
     to the end of the string.  This allows us to compile script
     strings that contain nulls. */
  char const *base;
  char const *prev;
  char const *cur;
  char const *end;
};

/* Information used to give out useful and informative error messages. */
struct error_info {
  /* This is the name of the current script file. */
  const char *name;

  /* This is the number of the current script line that we're compiling. */
  intmax_t line;

  /* This is the index of the "-e" expressions on the command line. */
  int string_expr_count;
};


/* Label structure used to resolve GOTO's, labels, and block beginnings. */
struct sed_label {
  idx_t v_index;		/* index of vector element being referenced */
  char *name;			/* NUL-terminated name of the label */
  struct error_info err_info;	/* track where '{}' blocks start */
  struct sed_label *next;	/* linked list (stack) */
};

struct special_files {
  struct output outf;
  FILE **pfp;
};

static FILE *my_stdin, *my_stdout, *my_stderr;
static struct special_files special_files[] = {
  { { (char *) "/dev/stdin", false, NULL, NULL }, &my_stdin },
  { { (char *) "/dev/stdout", false, NULL, NULL }, &my_stdout },
  { { (char *) "/dev/stderr", false, NULL, NULL }, &my_stderr },
  { { NULL, false, NULL, NULL }, NULL }
};

/* Where we are in the processing of the input. */
static struct prog_info prog;
static struct error_info cur_input;

/* Information about labels and jumps-to-labels.  This is used to do
   the required backpatching after we have compiled all the scripts. */
static struct sed_label *jumps = NULL;
static struct sed_label *labels = NULL;

/* We wish to detect #n magic only in the first input argument;
   this flag tracks when we have consumed the first file of input. */
static bool first_script = true;

/* Allow for scripts like "sed -e 'i\' -e foo": */
static struct buffer *pending_text = NULL;
static struct text_buf *old_text_buf = NULL;

/* Information about block start positions.  This is used to backpatch
   block end positions. */
static struct sed_label *blocks = NULL;

/* Use an obstack for compilation. */
static struct obstack obs;

static struct output *file_read = NULL;
static struct output *file_write = NULL;

/* Complain about a programming error and exit.
   bad_prog translates WHY, bad_prog_notranslate does not.  */
static _Noreturn void _GL_ATTRIBUTE_FORMAT_PRINTF_STANDARD (1, 0)
vbad_prog (char const *why, va_list ap)
{
  if (cur_input.name)
    fprintf (stderr, _("%s: file %s line %jd: "), program_name,
             quotef (cur_input.name), cur_input.line);
  else
    fprintf (stderr, _("%s: -e expression #%d, char %td: "),
             program_name,
             cur_input.string_expr_count,
             prog.cur - prog.base);

  vfprintf (stderr, why, ap);
  fputc ('\n', stderr);

  exit (EXIT_BAD_USAGE);
}
void
bad_prog (char const *why, ...)
{
  va_list ap;
  va_start (ap, why);
  vbad_prog (gettext (why), ap);
}
void
bad_prog_notranslate (const char *why, ...)
{
  va_list ap;
  va_start (ap, why);
  vbad_prog (why, ap);
}

enum { INCHAR_EOF = -1 - UCHAR_MAX };

/* Return the next program character as its char32_t encoding,
   or as -B if the next input is the encoding error byte B.
   Return INCHAR_EOF if there isn't anything to read.
   Keep cur_input.line up to date, so error messages can be meaningful. */
static int
inchar (void)
{
  char const *p = prog.prev = prog.cur;
  if (prog.end <= p)
    return INCHAR_EOF;
  mcel_t g = mcel_scan (p, prog.end);
  int ch = g.err ? -g.err : g.ch;
  prog.cur = p + g.len;
  if (ch == '\n')
    ++cur_input.line;
  return ch;
}

/* Undo the previous inchar, unless it said we were at EOF.  */
static void
savchar (void)
{
  if (prog.prev == prog.cur)
    return;
  prog.cur = prog.prev;
  if (*prog.cur == '\n' && cur_input.line > 0)
    --cur_input.line;
}

/* Read the next non-blank character from the program.  */
static int
in_nonblank (void)
{
  int ch;
  do
    ch = inchar ();
  while (0 < ch && c32isblank (ch));

  return ch;
}

/* Consume script input until a valid end of command marker is found:
     comment, closing brace, newline, semicolon or EOF.
   If any other character is found, die with 'extra characters after command'
   error.
*/
static void
read_end_of_cmd (void)
{
  const int ch = in_nonblank ();
  if (ch == CLOSE_BRACE || ch == '#')
    savchar ();
  else if (ch != INCHAR_EOF && ch != '\n' && ch != ';')
    bad_prog ("extra characters after command");
}

/* Read an unsigned integer value from the program.  Return the value,
   or INTMAX_MAX on overflow.  */
static intmax_t
in_integer (int ch)
{
  intmax_t num = 0;

  while (c_isdigit (ch))
    {
      if (ckd_mul (&num, num, 10) || ckd_add (&num, num, ch - '0'))
        num = INTMAX_MAX;
      ch = inchar ();
    }
  savchar ();
  return num;
}

static void
add_prev_to_buffer (struct buffer *b)
{
  char const *plim = prog.cur;
  for (char const *p = prog.prev; p < plim; p++)
    add1_buffer (b, *p);
}

static int
add_then_next (struct buffer *b)
{
  add_prev_to_buffer (b);
  return inchar ();
}

static char *
convert_number (char *result, char *buf, const char *bufend, int base)
{
  int n = 0;
  char *p;
  char *plim = buf + MIN (bufend - buf, 2 + (base < 16));

  for (p = buf; p < plim; p++)
    {
      int d = -1;
      switch (*p)
        {
        case '0': d = 0x0; break;
        case '1': d = 0x1; break;
        case '2': d = 0x2; break;
        case '3': d = 0x3; break;
        case '4': d = 0x4; break;
        case '5': d = 0x5; break;
        case '6': d = 0x6; break;
        case '7': d = 0x7; break;
        case '8': d = 0x8; break;
        case '9': d = 0x9; break;
        case 'A': case 'a': d = 0xa; break;
        case 'B': case 'b': d = 0xb; break;
        case 'C': case 'c': d = 0xc; break;
        case 'D': case 'd': d = 0xd; break;
        case 'E': case 'e': d = 0xe; break;
        case 'F': case 'f': d = 0xf; break;
        }
      if (d < 0 || base <= d)
        break;
      n = n * base + d;
    }

  *result = n & UCHAR_MAX;
  return p;
}

/* Read in a filename for a 'r', 'w', or 's///w' command. */
static struct buffer *
read_filename (void)
{
  struct buffer *b;
  int ch;

  if (sandbox)
    bad_prog ("e/r/w commands disabled in sandbox mode");

  b = init_buffer ();
  ch = in_nonblank ();
  while (ch != INCHAR_EOF && ch != '\n')
    {
#if 0 /*XXX ZZZ 1998-09-12 kpp: added, then had second thoughts*/
      if (posixicity == POSIXLY_EXTENDED)
        if (ch == ';' || ch == '#')
          {
            savchar ();
            break;
          }
#endif
      ch = add_then_next (b);
    }
  add1_buffer (b, '\0');
  return b;
}

static struct output *
get_openfile (struct output **file_ptrs, const char *mode, int fail)
{
  struct buffer *b;
  char *file_name;
  struct output *p;

  b = read_filename ();
  file_name = get_buffer (b);
  if (strlen (file_name) == 0)
    bad_prog ("missing filename in r/R/w/W commands");

  for (p=*file_ptrs; p; p=p->link)
    if (streq (p->name, file_name))
      break;

  if (posixicity == POSIXLY_EXTENDED)
    {
      /* Check whether it is a special file (stdin, stdout or stderr) */
      struct special_files *special = special_files;

      /* std* sometimes are not constants, so they
         cannot be used in the initializer for special_files */
      my_stdin = stdin; my_stdout = stdout; my_stderr = stderr;
      for (special = special_files; special->outf.name; special++)
        if (streq (special->outf.name, file_name))
          {
            special->outf.fp = *special->pfp;
            free_buffer (b);
            return &special->outf;
          }
    }

  if (!p)
    {
      p = obstack_alloc (&obs, sizeof *p);
      p->name = xstrdup (file_name);
      p->fp = ck_fopen (p->name, mode, fail);
      p->missing_newline = false;
      p->link = *file_ptrs;
      *file_ptrs = p;
    }
  free_buffer (b);
  return p;
}

static struct sed_cmd *
next_cmd_entry (struct vector *v)
{
  struct sed_cmd *cmd;

  if (v->v_length == v->v_allocated)
    v->v = xpalloc (v->v, &v->v_allocated, 1, -1, sizeof *v->v);

  cmd = v->v + v->v_length;

  /* This sets cmd->cmd to something invalid, to catch bugs early.  */
  *cmd = (struct sed_cmd) {NULL};

  return cmd;
}

static int
snarf_char_class (struct buffer *b)
{
  int ch;
  int state = 0;
  int delim IF_LINT ( = 0) ;

  ch = inchar ();
  if (ch == '^')
    ch = add_then_next (b);
  if (ch == CLOSE_BRACKET)
    ch = add_then_next (b);

  /* States are:
        0 outside a collation element, character class or collation class
        1 after the bracket
        2 after the opening ./:/=
        3 after the closing ./:/= */

  for (;; ch = add_then_next (b))
    {
      switch (ch)
        {
        case INCHAR_EOF:
        case '\n':
          return ch;

        case '.':
        case ':':
        case '=':
          if (state == 1)
            {
              delim = ch;
              state = 2;
            }
          else if (state == 2 && ch == delim)
            state = 3;
          else
            break;

          continue;

        case OPEN_BRACKET:
          if (state == 0)
            state = 1;
          continue;

        case CLOSE_BRACKET:
          if (state == 0 || state == 1)
            return ch;
          else if (state == 3)
            state = 0;

          break;

        default:
          break;
        }

      /* Getting a character different from .=: whilst in state 1
         goes back to state 0, getting a character different from ]
         whilst in state 3 goes back to state 2.  */
      state &= ~1;
    }
}

static struct buffer *
match_slash (int slash, bool regex, bool s_command)
{
  struct buffer *b;
  int ch;

  b = init_buffer ();
  while ((ch = inchar ()) != INCHAR_EOF && ch != '\n')
    {
      if (ch == slash)
        return b;
      else if (ch == '\\')
        {
          ch = inchar ();
          if (ch == INCHAR_EOF)
            break;
          /* Preserve backslash except when escaping delimiter in regex.  */
          if (ch != '\n' && (ch != slash || (!regex && ch == '&')))
            add1_buffer (b, '\\');
          /* Special case: in regex, treat \cX as atomic escape,
             but only in GNU-extension mode (not strict POSIX).  */
          if (regex && ch == 'c' && posixicity != POSIXLY_BASIC)
            {
              add_prev_to_buffer (b);
              int next = inchar ();
              if (next == INCHAR_EOF)
                break;
              add_prev_to_buffer (b);
              /* Skip end-of-loop add_prev_to_buffer; we already did it.  */
              continue;
            }
          /* Warn for some non-portable backslash escapes if --posix is
             in use.  Note that we ignore any special characters, although
             they may be non-portable in some contexts.  */
          if (s_command && posixicity != POSIXLY_EXTENDED
              && ! (ch == slash
                    || ch == '&' || ch == '\\' || c_isdigit (ch) || ch == '\n'
                    || ch == '.' || ch == '*' || ch == '^' || ch == '$'
                    || ch == '(' || ch == ')' || ch == '{' || ch == '}'
                    || ch == OPEN_BRACKET
                    || (extended_regexp_flags & REG_EXTENDED
                        && (ch == '+' || ch == '?' || ch == '|'))))
            fprintf (stderr, _("%s: warning: using \"\\%.*s\" in the 's' "
                               "command is not portable\n"),
                     program_name, (int) {prog.cur - prog.prev}, prog.prev);
        }
      else if (ch == OPEN_BRACKET && regex)
        {
          add_prev_to_buffer (b);
          ch = snarf_char_class (b);
          if (ch != CLOSE_BRACKET)
            break;
        }

      add_prev_to_buffer (b);
    }

  if (ch == '\n')
    savchar ();	/* for proper line number in error report */
  free_buffer (b);
  return NULL;
}

static int
mark_subst_opts (struct subst *cmd)
{
  int flags = 0;
  int ch;

  cmd->global = false;
  cmd->print = false;
  cmd->eval = false;
  cmd->numb = 0;
  cmd->outf = NULL;

  for (;;)
    switch ( (ch = in_nonblank ()) )
      {
      case 'i':	/* GNU extension */
      case 'I':	/* GNU extension */
        if (posixicity == POSIXLY_BASIC)
          bad_prog ("unknown option to 's'");
        flags |= REG_ICASE;
        break;

      case 'm':	/* GNU extension */
      case 'M':	/* GNU extension */
        if (posixicity == POSIXLY_BASIC)
          bad_prog ("unknown option to 's'");
        flags |= REG_NEWLINE;
        break;

      case 'e':
        if (posixicity == POSIXLY_BASIC)
          bad_prog ("unknown option to 's'");
        cmd->eval = true;
        break;

      case 'p':
        if (cmd->print)
          bad_prog ("multiple 'p' options to 's' command");
        cmd->print |= (1 << cmd->eval); /* 1=before eval, 2=after */
        break;

      case 'g':
        if (cmd->global)
          bad_prog ("multiple 'g' options to 's' command");
        cmd->global = true;
        break;

      case 'w':
        cmd->outf = get_openfile (&file_write, write_mode, true);
        return flags;

      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
        if (cmd->numb)
          bad_prog ("multiple number options to 's' command");
        cmd->numb = in_integer (ch);
        if (!cmd->numb)
          bad_prog ("number option to 's' command may not be zero");
        break;

      case CLOSE_BRACE:
      case '#':
        savchar ();
        FALLTHROUGH;
      case INCHAR_EOF:
      case '\n':
      case ';':
        return flags;

      case '\r':
        if (inchar () == '\n')
          return flags;
        FALLTHROUGH;
      default:
        bad_prog ("unknown option to 's'");
      }
}

/* read in a label for a ':', 'b', or 't' command */
static char * _GL_ATTRIBUTE_MALLOC
read_label (void)
{
  struct buffer *b;
  int ch;
  char *ret;

  b = init_buffer ();
  ch = in_nonblank ();

  while (ch != INCHAR_EOF && ch != '\n'
         && ch != ';' && ch != CLOSE_BRACE && ch != '#'
         && !(0 <= ch && c32isblank (ch)))
    ch = add_then_next (b);

  savchar ();
  add1_buffer (b, '\0');
  ret = xstrdup (get_buffer (b));
  free_buffer (b);
  return ret;
}

/* Store a label (or label reference) created by a ':', 'b', or 't'
   command so that the jump to/from the label can be backpatched after
   compilation is complete, or a reference created by a '{' to be
   backpatched when the corresponding '}' is found.  */
static struct sed_label *
setup_label (struct sed_label *list, idx_t idx, char *name,
             const struct error_info *err_info)
{
  struct sed_label *ret = obstack_alloc (&obs, sizeof *ret);
  ret->v_index = idx;
  ret->name = name;
  if (err_info)
    ret->err_info = *err_info;
  ret->next = list;
  return ret;
}

static struct sed_label *
release_label (struct sed_label *list_head)
{
  struct sed_label *ret;

  if (!list_head)
    return NULL;
  ret = list_head->next;

  free (list_head->name);

#if 0
  /* We use obstacks */
  free (list_head);
#endif
  return ret;
}

static struct replacement *
new_replacement (char *text, idx_t length, enum replacement_types type)
{
  struct replacement *r = obstack_alloc (&obs, sizeof *r);

  r->prefix = text;
  r->prefix_length = length;
  r->subst_id = -1;
  r->repl_type = type;

  /* r-> next = NULL; */
  return r;
}

static void
setup_replacement (struct subst *sub, const char *text, idx_t length)
{
  char *base;
  char *p;
  char *text_end;
  enum replacement_types repl_type = REPL_ASIS, save_type = REPL_ASIS;
  struct replacement root;
  struct replacement *tail;

  sub->max_id = 0;
  base = xmemdup (text, length);
  length = normalize_text (base, length, TEXT_REPLACEMENT);

#ifdef PACIFY_LSAN
  sub->replacement_buffer = base;
#endif

  text_end = base + length;
  tail = &root;

  for (p=base; p<text_end; ++p)
    {
      if (*p == '\\')
        {
          /* Preceding the backslash may be some literal text: */
          tail = tail->next =
            new_replacement (base, p - base, repl_type);

          repl_type = save_type;

          /* Skip the backslash and look for a numeric back-reference,
             or a case-munging escape if not in POSIX mode: */
          ++p;
          if (p == text_end)
            ++tail->prefix_length;

          else if (posixicity == POSIXLY_BASIC && !c_isdigit (*p))
            {
              p[-1] = *p;
              ++tail->prefix_length;
            }

          else
            switch (*p)
              {
              case '0': case '1': case '2': case '3': case '4':
              case '5': case '6': case '7': case '8': case '9':
                tail->subst_id = *p - '0';
                if (sub->max_id < tail->subst_id)
                  sub->max_id = tail->subst_id;
                break;

              case 'L':
                repl_type = REPL_LOWERCASE;
                save_type = REPL_LOWERCASE;
                break;

              case 'U':
                repl_type = REPL_UPPERCASE;
                save_type = REPL_UPPERCASE;
                break;

              case 'E':
                repl_type = REPL_ASIS;
                save_type = REPL_ASIS;
                break;

              case 'l':
                save_type = repl_type;
                repl_type |= REPL_LOWERCASE_FIRST;
                break;

              case 'u':
                save_type = repl_type;
                repl_type |= REPL_UPPERCASE_FIRST;
                break;

              default:
                p[-1] = *p;
                ++tail->prefix_length;
              }

          base = p + 1;
        }
      else if (*p == '&')
        {
          /* Preceding the ampersand may be some literal text: */
          tail = tail->next =
            new_replacement (base, p - base, repl_type);

          repl_type = save_type;
          tail->subst_id = 0;
          base = p + 1;
        }
  }
  /* There may be some trailing literal text: */
  if (base < text_end)
    tail = tail->next =
      new_replacement (base, text_end - base, repl_type);

  tail->next = NULL;
  sub->replacement = root.next;
}

static void
read_text (struct text_buf *buf, int leadin_ch)
{
  int ch;

  /* Should we start afresh (as opposed to continue a partial text)? */
  if (buf)
    {
      if (pending_text)
        free_buffer (pending_text);
      pending_text = init_buffer ();
      buf->text = NULL;
      buf->text_length = 0;
      old_text_buf = buf;
    }
  /* assert(old_text_buf != NULL); */

  if (leadin_ch == INCHAR_EOF)
    return;

  if (leadin_ch != '\n')
    add_prev_to_buffer (pending_text);

  ch = inchar ();
  while (ch != INCHAR_EOF && ch != '\n')
    {
      if (ch == '\\')
        {
          ch = inchar ();
          if (ch != INCHAR_EOF)
            add1_buffer (pending_text, '\\');
        }

      if (ch == INCHAR_EOF)
        {
          add1_buffer (pending_text, '\n');
          return;
        }

      ch = add_then_next (pending_text);
    }

  add1_buffer (pending_text, '\n');
  if (!buf)
    buf = old_text_buf;
  buf->text_length = normalize_text (get_buffer (pending_text),
                                     size_buffer (pending_text), TEXT_BUFFER);
  buf->text = xmemdup (get_buffer (pending_text), buf->text_length);
  free_buffer (pending_text);
  pending_text = NULL;
}

/* Try to read an address for a sed command.  If it succeeds,
   return non-zero and store the resulting address in '*addr'.
   If the input doesn't look like an address read nothing
   and return zero.  */
static bool
compile_address (struct addr *addr, int ch)
{
  addr->addr_type = ADDR_IS_NULL;
  addr->addr_step = 0;
  addr->addr_number = -1;  /* cannot match */
  addr->addr_regex = NULL;

  if (ch == '/' || ch == '\\')
    {
      int flags = 0;
      struct buffer *b;
      addr->addr_type = ADDR_IS_REGEX;
      if (ch == '\\')
        ch = inchar ();
      if ( !(b = match_slash (ch, true, false)) )
        bad_prog ("unterminated address regex");

      for (;;)
        {
          ch = in_nonblank ();
          if (posixicity == POSIXLY_BASIC)
            goto posix_address_modifier;
          switch (ch)
            {
            case 'I':	/* GNU extension */
              flags |= REG_ICASE;
              break;

            case 'M':	/* GNU extension */
              flags |= REG_NEWLINE;
              break;

            default:
            posix_address_modifier:
              savchar ();
              addr->addr_regex = compile_regex (b, flags, 0);
              free_buffer (b);
              return true;
            }
        }
    }
  else if (c_isdigit (ch))
    {
      addr->addr_number = in_integer (ch);
      addr->addr_type = ADDR_IS_NUM;
      ch = in_nonblank ();
      if (ch != '~' || posixicity == POSIXLY_BASIC)
        {
          savchar ();
        }
      else
        {
          idx_t step = in_integer (in_nonblank ());
          if (step > 0)
            {
              addr->addr_step = step;
              addr->addr_type = ADDR_IS_NUM_MOD;
            }
        }
    }
  else if ((ch == '+' || ch == '~') && posixicity != POSIXLY_BASIC)
    {
      addr->addr_step = in_integer (in_nonblank ());
      if (addr->addr_step==0)
        ; /* default to ADDR_IS_NULL; forces matching to stop on next line */
      else if (ch == '+')
        addr->addr_type = ADDR_IS_STEP;
      else
        addr->addr_type = ADDR_IS_STEP_MOD;
    }
  else if (ch == '$')
    {
      addr->addr_type = ADDR_IS_LAST;
    }
  else
    return false;

  return true;
}

/* Read a program (or a subprogram within '{' '}' pairs) in and store
   the compiled form in '*vector'.  Return a pointer to the new vector.  */
static struct vector *
compile_program (struct vector *vector)
{
  struct sed_cmd *cur_cmd;
  struct buffer *b;
  int ch;

  if (!vector)
    {
      vector = XNMALLOC (1, struct vector);
      vector->v = NULL;
      vector->v_allocated = 0;
      vector->v_length = 0;

      obstack_init (&obs);
    }
  if (pending_text)
    read_text (NULL, '\n');

  for (;;)
    {
      struct addr a;

      while (0 <= (ch = inchar ()) && (c32isspace (ch) || ch == ';'))
        ;
      if (ch == INCHAR_EOF)
        break;

      cur_cmd = next_cmd_entry (vector);
      if (compile_address (&a, ch))
        {
          if (a.addr_type == ADDR_IS_STEP
              || a.addr_type == ADDR_IS_STEP_MOD)
            bad_prog ("invalid usage of +N or ~N as first address");

          cur_cmd->a1 = xmemdup (&a, sizeof a);
          ch = in_nonblank ();
          if (ch == ',')
            {
              if (!compile_address (&a, in_nonblank ()))
                bad_prog ("unexpected ','");

              cur_cmd->a2 = xmemdup (&a, sizeof a);
              ch = in_nonblank ();
            }

          if ((cur_cmd->a1->addr_type == ADDR_IS_NUM
               && cur_cmd->a1->addr_number == 0)
              && ((!cur_cmd->a2 && ch != 'r')
                  || (cur_cmd->a2 && cur_cmd->a2->addr_type != ADDR_IS_REGEX)
                  || posixicity == POSIXLY_BASIC))
            bad_prog ("invalid usage of line address 0");
        }
      if (ch == '!')
        {
          cur_cmd->addr_bang = true;
          ch = in_nonblank ();
          if (ch == '!')
            bad_prog ("multiple '!'s");
        }

      /* Do not accept extended commands in --posix mode.  Also,
         a few commands only accept one address in that mode.  */
      if (posixicity == POSIXLY_BASIC)
       switch (ch)
         {
           case 'e': case 'F': case 'v': case 'z':
           case 'Q': case 'T': case 'R': case 'W':
             bad_prog ("unknown command: '%c'", ch);

            case 'a': case 'i': case 'l':
            case '=': case 'r':
              if (cur_cmd->a2)
                bad_prog ("command only uses one address");
          }

      cur_cmd->cmd = ch;
      switch (ch)
        {
        case '#':
          if (cur_cmd->a1)
            bad_prog ("comments don't accept any addresses");
          ch = inchar ();
          if (ch=='n' && first_script && cur_input.line < 2)
            if (prog.cur - prog.base == 2)
              no_default_output = true;
          while (ch != INCHAR_EOF && ch != '\n')
            ch = inchar ();
          continue;	/* restart the for (;;) loop */

        case 'v':
          /* This is an extension.  Programs needing GNU sed might start
           * with a 'v' command so that other seds will stop.
           * We compare the version and ignore POSIXLY_CORRECT.
           */
          {
            char *version = read_label ();
            char const *compared_version;
            compared_version = (*version == '\0') ? "4.0" : version;
            if (strverscmp (compared_version, PACKAGE_VERSION) > 0)
              bad_prog ("expected newer version of sed");

            free (version);
            posixicity = POSIXLY_EXTENDED;
          }
          continue;

        case '{':
          blocks = setup_label (blocks, vector->v_length, NULL, &cur_input);
          cur_cmd->addr_bang = !cur_cmd->addr_bang;
          break;

        case '}':
          if (!blocks)
            bad_prog ("unexpected '}'");
          if (cur_cmd->a1)
            bad_prog ("'}' doesn't want any addresses");

          read_end_of_cmd ();

          vector->v[blocks->v_index].x.jump_index = vector->v_length;
          blocks = release_label (blocks);	/* done with this entry */
          break;

        case 'e':
          if (sandbox)
            bad_prog ("e/r/w commands disabled in sandbox mode");

          ch = in_nonblank ();
          if (ch == INCHAR_EOF || ch == '\n')
            {
              cur_cmd->x.cmd_txt.text_length = 0;
              break;
            }
          else
            goto read_text_to_slash;

        case 'a':
        case 'i':
        case 'c':
          ch = in_nonblank ();

        read_text_to_slash:
          if (ch == INCHAR_EOF)
            bad_prog ("expected \\ after 'a', 'c' or 'i'");

          if (ch == '\\')
            ch = inchar ();
          else
            {
              if (posixicity == POSIXLY_BASIC)
                bad_prog ("expected \\ after 'a', 'c' or 'i'");
              savchar ();
              ch = '\n';
            }

          read_text (&cur_cmd->x.cmd_txt, ch);
          break;

        case ':':
          if (cur_cmd->a1)
            bad_prog (": doesn't want any addresses");
          {
            char *label = read_label ();
            if (!*label)
              bad_prog ("\":\" lacks a label");
            labels = setup_label (labels, vector->v_length, label, NULL);

            if (debug)
              cur_cmd->x.label_name = strdup (label);
          }
          break;

        case 'T':
        case 'b':
        case 't':
          jumps = setup_label (jumps, vector->v_length, read_label (), NULL);
          break;

        case 'Q':
        case 'q':
          if (cur_cmd->a2)
            bad_prog ("command only uses one address");
          FALLTHROUGH;

        case 'l':
          ch = in_nonblank ();
          if (c_isdigit (ch) && posixicity != POSIXLY_BASIC)
            {
              cur_cmd->x.int_arg = in_integer (ch);
            }
          else
            {
              cur_cmd->x.int_arg = -1;
              savchar ();
            }

          read_end_of_cmd ();
          break;

       case '=':
       case 'd':
       case 'D':
       case 'F':
       case 'g':
       case 'G':
       case 'h':
        case 'H':
        case 'n':
        case 'N':
        case 'p':
        case 'P':
        case 'z':
        case 'x':
          read_end_of_cmd ();
          break;

        case 'r':
          b = read_filename ();
          if (strlen (get_buffer (b)) == 0)
            bad_prog ("missing filename in r/R/w/W commands");
          cur_cmd->x.readcmd.fname = xstrdup (get_buffer (b));

          /* Adjust '0rFILE' command to '1rFILE' in prepend mode */
          if (cur_cmd->a1
              && cur_cmd->a1->addr_type == ADDR_IS_NUM
              && cur_cmd->a1->addr_number == 0
              && !cur_cmd->a2)
            {
              cur_cmd->a1->addr_number = 1;
              cur_cmd->x.readcmd.append = false;
            }
          else
            {
              cur_cmd->x.readcmd.append = true;
            }
          free_buffer (b);
          break;

        case 'R':
          cur_cmd->x.inf = get_openfile (&file_read, read_mode, false);
          break;

        case 'W':
        case 'w':
          cur_cmd->x.outf = get_openfile (&file_write, write_mode, true);
          break;

        case 's':
          {
            struct buffer *b2;
            int flags;
            int slash;

            slash = inchar ();
            if ( !(b  = match_slash (slash, true, true)) )
              bad_prog ("unterminated 's' command");
            if ( !(b2 = match_slash (slash, false, true)) )
              bad_prog ("unterminated 's' command");

            cur_cmd->x.cmd_subst
              = obstack_alloc (&obs, sizeof *cur_cmd->x.cmd_subst);
            setup_replacement (cur_cmd->x.cmd_subst,
                               get_buffer (b2), size_buffer (b2));
            free_buffer (b2);

            flags = mark_subst_opts (cur_cmd->x.cmd_subst);
            cur_cmd->x.cmd_subst->regx =
              compile_regex (b, flags, cur_cmd->x.cmd_subst->max_id + 1);
            free_buffer (b);

            if (cur_cmd->x.cmd_subst->eval && sandbox)
              bad_prog ("e/r/w commands disabled in sandbox mode");
          }
          break;

        case 'y':
          {
            idx_t len, dest_len;
            int slash;
            struct buffer *b2;
            char *src_buf, *dest_buf;

            slash = inchar ();
            if ( !(b = match_slash (slash, false, false)) )
              bad_prog ("unterminated 'y' command");
            src_buf = get_buffer (b);
            len = normalize_text (src_buf, size_buffer (b), TEXT_BUFFER);

            if ( !(b2 = match_slash (slash, false, false)) )
              bad_prog ("unterminated 'y' command");
            dest_buf = get_buffer (b2);
            dest_len = normalize_text (dest_buf, size_buffer (b2), TEXT_BUFFER);

            /* If multibyte, count the source buffer's characters.  */
            idx_t src_char_num = 0;
            if (localeinfo.multibyte)
              for (idx_t i = 0; i < len;
                   i += mcel_scan (src_buf + i, src_buf + len).len)
                src_char_num++;
            cur_cmd->x.translate.npairs = src_char_num;

            if (src_char_num)
              {
                idx_t idx = 0;

                /* trans_pairs = {src(0), dest(0), src(1), dest(1), ... }
                     src(i) : i-th source character.
                     dest(i) : i-th destination character.  */
                struct trans_pair *trans_pair = xnmalloc (src_char_num,
                                                          sizeof *trans_pair);
                cur_cmd->x.translate.a.pair = trans_pair;
                for (idx_t i = 0; i < src_char_num; i++)
                  {
                    if (idx >= dest_len)
                      bad_prog ("'y' command strings have different lengths");

                    /* Set the i-th source character.  */
                    mcel_t s = mcel_scan (src_buf, src_buf + len);
                    trans_pair[i].from = s.err ? -s.err : s.ch;
                    src_buf += s.len; /* Forward to next character.  */
                    len -= s.len;

                    /* Fetch the i-th destination character.  */
                    mcel_t d = mcel_scan (dest_buf + idx, dest_buf + dest_len);

                    /* Set the i-th destination character.  */
                    memcpy (trans_pair[i].to, dest_buf + idx, d.len);
                    if (d.len < sizeof trans_pair[i].to)
                      trans_pair[i].to[d.len] = '\0';
                    idx += d.len; /* Forward to next character.  */
                  }
                if (idx != dest_len)
                  bad_prog ("'y' command strings have different lengths");
              }
            else
              {
                /* A single-byte locale, or a no-op empty translation y///
                   in a multibyte locale.  */
                char *translate = obstack_alloc (&obs, UCHAR_MAX + 1);
                char *ustring = src_buf;

                if (len != dest_len)
                  bad_prog ("'y' command strings have different lengths");

                /* The default translation of a character is itself.
                   Don't trap on debugging platforms if char is signed.  */
                for (idx_t i = 0; i < UCHAR_MAX + 1; i++)
                  translate[i] = i <= CHAR_MAX ? i : i - (UCHAR_MAX + 1);

                while (dest_len--)
                  translate[(unsigned char) {*ustring++}] = *dest_buf++;

                cur_cmd->x.translate.a.sb = translate;
              }

            read_end_of_cmd ();

            free_buffer (b);
            free_buffer (b2);
          }
        break;

        case INCHAR_EOF:
          bad_prog ("missing command");
          unreachable ();

        default:
          bad_prog ("unknown command: '%c'", ch);
          unreachable ();
        }

      /* this is buried down here so that "continue" statements will miss it */
      ++vector->v_length;
    }
  if (posixicity == POSIXLY_BASIC && pending_text)
    bad_prog ("incomplete command");
  return vector;
}

/* deal with \X escapes */
idx_t
normalize_text (char *buf, idx_t len, enum text_types buftype)
{
  const char *bufend = buf + len;
  char *p = buf;
  char *q = buf;

  /* This variable prevents normalizing text within bracket
     subexpressions when conforming to POSIX.  If 0, we
     are not within a bracket expression.  If -1, we are within a
     bracket expression but are not within [.FOO.], [=FOO=],
     or [:FOO:].  Otherwise, this is the '.', '=', or ':'
     respectively within these three types of subexpressions.  */
  int bracket_state = 0;

  while (p < bufend)
    {
      idx_t mbclen = mcel_scan (p, bufend).len;
      if (mbclen != 1)
        {
          memmove (q, p, mbclen);
          q += mbclen;
          p += mbclen;
          continue;
        }

      int base;
      char ch = *p;

      if (ch == '\\' && p + 1 < bufend && bracket_state == 0)
        switch (*++p)
          {
          case 'a': ch = '\a'; break;
          /* case 'b' would conflict with \b RE.  */
          case 'f': ch = '\f'; break;
          case '\n': /*fall through */
          case 'n': ch = '\n'; break;
          case 'r': ch = '\r'; break;
          case 't': ch = '\t'; break;
          case 'v': ch = '\v'; break;

          case 'd': /* decimal byte */
            base = 10;
            goto convert;

          case 'x': /* hexadecimal byte */
            base = 16;
            goto convert;

          case 'o': /* octal byte */
            base = 8;
convert:
            if (bufend - p < 2)
              goto unrecognized_escape;
            char *p1 = convert_number (&ch, p + 1, bufend, base);
            if (p1 == p + 1)
              goto unrecognized_escape;
            p = p1 - 1;

            /* Re-escape any escaped & or \ in a replacement.  */
            if (buftype == TEXT_REPLACEMENT && (ch == '&' || ch == '\\'))
              *q++ = '\\';
            break;

          case 'c':
            if (bufend - p < 2)
              goto unrecognized_escape;
            p++;

            ch = c_toupper (*p) ^ 0x40;
            if (*p == '\\')
              {
                p++;
                if (! (p < bufend && *p == '\\'))
                  bad_prog ("recursive escaping after \\c not allowed");
              }
            break;

          default:
          unrecognized_escape:
            /* we just pass the \ up one level for interpretation */
            if (buftype != TEXT_BUFFER)
              *q++ = '\\';
            ch = *p;
            break;
          }
      else if (buftype == TEXT_REGEX && posixicity != POSIXLY_EXTENDED)
        switch (*p)
          {
          case '[':
            if (!bracket_state)
              bracket_state = -1;
            break;

          case ':':
          case '.':
          case '=':
            if (bracket_state == -1 && q[-1] == '[')
              bracket_state = *p;
            break;

          case ']':
            if (bracket_state == 0)
              ;
            else if (bracket_state == -1)
              bracket_state = 0;
            else if (q[-2] != bracket_state && q[-1] == bracket_state)
              bracket_state = -1;
            break;
          }

      *q++ = ch;
      p++;
    }
    return q - buf;
}


/* 'str' is a string (from the command line) that contains a sed command.
   Compile the command, and add it to the end of 'cur_program'. */
struct vector *
compile_string (struct vector *cur_program, char *str, idx_t len)
{
  static int string_expr_count;
  struct vector *ret;

  prog.base = str;
  prog.prev = prog.cur = prog.base;
  prog.end = prog.cur + len;

  cur_input.line = 0;
  cur_input.name = NULL;
  cur_input.string_expr_count = ++string_expr_count;

  ret = compile_program (cur_program);
  prog.base = NULL;
  prog.prev = prog.cur = NULL;
  prog.end = NULL;

  first_script = false;
  return ret;
}

/* 'cmdfile' is the name of a file containing sed commands.
   Read them in and add them to the end of 'cur_program'.
 */
struct vector *
compile_file (struct vector *cur_program, const char *cmdfile)
{
  struct vector *ret;
  size_t len;
  char *str = (streq (cmdfile, "-")
               ? fread_file (stdin, 0, &len)
               : read_file (cmdfile, 0, &len));

  if (!str)
    panic (_("couldn't read file %s: %s"), quotef (cmdfile), strerror (errno));

  prog.base = str;
  prog.prev = prog.cur = prog.base;
  prog.end = prog.cur + len;

  cur_input.line = 1;
  cur_input.name = cmdfile;
  cur_input.string_expr_count = 0;

  ret = compile_program (cur_program);
  free (str);
  prog.base = NULL;
  prog.prev = prog.cur = NULL;
  prog.end = NULL;

  first_script = false;
  return ret;
}

static void
cleanup_program_filenames (void)
{
  {
    struct output *p;

    for (p = file_read; p; p = p->link)
      if (p->name)
        {
          free (p->name);
          p->name = NULL;
        }

    for (p = file_write; p; p = p->link)
      if (p->name)
        {
          free (p->name);
          p->name = NULL;
        }
  }
}

/* Make any checks which require the whole program to have been read.
   In particular: this backpatches the jump targets.
   Any cleanup which can be done after these checks is done here also.  */
void
check_final_program (struct vector *program)
{
  struct sed_label *go;
  struct sed_label *lbl;

  /* do all "{"s have a corresponding "}"? */
  if (blocks)
    {
      /* update info for error reporting: */
      cur_input = blocks->err_info;
      bad_prog ("unmatched '{'");
    }

  /* was the final command an unterminated a/c/i command? */
  if (pending_text)
    {
      old_text_buf->text_length = size_buffer (pending_text);
      if (old_text_buf->text_length)
        old_text_buf->text = xmemdup (get_buffer (pending_text),
                                      old_text_buf->text_length);
      free_buffer (pending_text);
      pending_text = NULL;
    }

  for (go = jumps; go; go = release_label (go))
    {
      for (lbl = labels; lbl; lbl = lbl->next)
        if (streq (lbl->name, go->name))
          break;
      if (lbl)
        {
          program->v[go->v_index].x.jump_index = lbl->v_index;
        }
      else
        {
          if (*go->name)
            panic (_("can't find label for jump to '%s'"), go->name);
          program->v[go->v_index].x.jump_index = program->v_length;
        }
    }
  jumps = NULL;

  for (lbl = labels; lbl; lbl = release_label (lbl))
    ;
  labels = NULL;
}


/* Rewind all resources which were allocated in this module. */
void
rewind_read_files (void)
{
  struct output *p;

  for (p=file_read; p; p=p->link)
    if (p->fp)
      rewind (p->fp);
}

/* Release all resources which were allocated in this module. */
void
finish_program (struct vector *program)
{
  cleanup_program_filenames ();

  /* close all files... */
  {
    struct output *p, *q;

    for (p=file_read; p; p=q)
      {
        if (p->fp)
          ck_fclose (p->fp);
        q = p->link;
#if 0
        /* We use obstacks. */
        free (p);
#endif
      }

    for (p=file_write; p; p=q)
      {
        if (p->fp)
          ck_fclose (p->fp);
        q = p->link;
#if 0
        /* We use obstacks. */
        free (p);
#endif
      }
    file_read = file_write = NULL;
  }

#ifdef PACIFY_LSAN
  for (idx_t i = 0; i < program->v_length; ++i)
    {
      const struct sed_cmd *sc = &program->v[i];

      if (sc->a1 && sc->a1->addr_regex)
        release_regex (sc->a1->addr_regex);
      if (sc->a2 && sc->a2->addr_regex)
        release_regex (sc->a2->addr_regex);

      switch (sc->cmd)
        {
        case 's':
          free (sc->x.cmd_subst->replacement_buffer);
          if (sc->x.cmd_subst->regx)
            release_regex (sc->x.cmd_subst->regx);
          break;
        }
    }

  obstack_free (&obs, NULL);
#else
  (void)program;
#endif

}

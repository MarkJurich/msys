/*
 * mcsource.c
 *
 * $Id: mcsource.c,v 1.4 2007-05-11 19:56:17 keithmarshall Exp $
 *
 * Copyright (C) 2006, 2007, Keith Marshall
 *
 * This file implements the message catalogue source code parser, which is
 * used internally by `gencat', to compile message dictionaries.
 *
 * Written by Keith Marshall  <keithmarshall@users.sourceforge.net>
 * Last modification: 27-Mar-2007
 *
 *
 * This is free software.  It is provided AS IS, in the hope that it may
 * be useful, but WITHOUT WARRANTY OF ANY KIND, not even an IMPLIED WARRANTY
 * of MERCHANTABILITY, nor of FITNESS FOR ANY PARTICULAR PURPOSE.
 *
 * Permission is granted to redistribute this software, either "as is" or
 * in modified form, under the terms of the GNU General Public License, as
 * published by the Free Software Foundation; either version 2, or (at your
 * option) any later version.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to the
 * Free Software Foundation, 51 Franklin St - Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */
#define WIN32_LEAN_AND_MEAN
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* Win32 hosts don't have native support for `LC_MESSAGES',
 * which is required by POSIX, but MinGW may allow us to emulate it,
 * with this define *before* we source locale.h.
 */
#define MINGW32_LC_EXTENSIONS  MINGW32_LC_ENVVARS | MINGW32_LC_MESSAGES

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <locale.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <nl_types.h>
#include <langinfo.h>

#include <gencat.h>
#include <gcmsgs.h>
#include <debug.h>

#include <platform.h>

#ifdef DEBUG_BUFSIZ
# undef  BUFSIZ
# define BUFSIZ DEBUG_BUFSIZ
#endif

/* The following define the input states of the gencat parser
*/
#define NEWLINE    0x0010   /* set at start of each logical input line  */
#define CONTINUED  0x0020   /* set if newline escaped on previous line  */
#define DIRECTIVE  0x0100   /* set while parsing a gencat directive     */
#define NUMERIC    0x0200   /* set while parsing a message/set number   */
#define MSGTEXT    0x0400   /* set while parsing message text           */
#define NEWMSG     0x0800   /* set when waiting to parse a new message  */
#define FLUSH      0x0800   /* set when message cache must be flushed   */
#define QUOTED     0x1000   /* set if the current message is quoted     */
#define ENCODED    0x2000   /* set while parsing locale encoded input   */
#define CONTEXT    0x4000   /* set when the message locale is active    */

#define CATEGORY   0x000F   /* mask to extract directive action codes   */

#define ADDSET     0x0001   /* action code set by `$set' directive      */
#define DELSET     0x0002   /* action code set by `$delset' directive   */
#define DEFQUOTE   0x0003   /* action code set by `$quote' directive    */

#define DEFCONV    0x0008   /* comment category for codeset declaration */

/* CONTINUED is a specialised case of the more general ESCAPE mechanism,
 * so let them share a common control flag.
 */
#define ESCAPE     CONTINUED

static
int mc_directive( int status, const char *keyword )
{
  /* Identify a GENCAT directive, based on a specified keyword,
   * and activate the appropriate parser attribute bits to process it.
   */

  static struct directives
  {
    /* Defines the dictionary of known directives,
     * and the parser attributes associated with each.
     */
    char *keyword;  /* the actual keyword identifying the directive */
    int   mask;     /* mask defining the attribute bits it preserves */
    int   enable;   /* additional attribute bits it turns on */
  }
  dictionary[] =
  { /*
     * keyword   mask                  enables attributes
     * -------   -------------------   --------------------
     */
    { "set",     0xfff0 & ~DIRECTIVE,  ADDSET   | NUMERIC },
    { "delset",  0xfff0 & ~DIRECTIVE,  DELSET   | NUMERIC },
    { "quote",   0xfff0 & ~DIRECTIVE,  DEFQUOTE | ENCODED },
    /*
     * An entry with a NULL keyword pointer marks
     * the end of the lookup table, and specifies the
     * default mask and flags, to be applied when
     * an unrecognised keyword is parsed.
     */
    {  NULL,     0xfff0 & ~DIRECTIVE,   0               }
  };

  struct directives *lookup = dictionary;
  while( lookup->keyword && strcmp( keyword, lookup->keyword ) )
    ++lookup;

  return (status & lookup->mask) | lookup->enable;
}

static
char *mc_default_codeset( void )
{
  /* Helper function, called when the message definition file for a
   * catalogue doesn't explicitly specify a codeset for the messages;
   * establish the default codeset for the message catalogue, using
   * the codeset of the LC_MESSAGES category in the present locale.
   */
  char *codeset;

  if( (codeset = setlocale( LC_MESSAGES, "" )) == NULL )
    codeset = setlocale( LC_MESSAGES, NULL );
  setlocale( LC_CTYPE, codeset );
  codeset = strdup( nl_langinfo( CODESET ));
  setlocale( LC_CTYPE, "C" );

  return codeset;
}

static
int errout( const char *src, long linenum, const char *fmt, ... )
{
  va_list args;
  va_start( args, fmt );
  fprintf( stderr, "%s:%ld:", src, linenum );
  vfprintf( stderr, fmt, args );
  va_end( args );
  return EXIT_FAILURE;
}

static
off_t wanted( int fd )
{
  struct stat info;
# ifndef DEBUG
  if( (fstat( fd, &info ) == 0) && (info.st_size > (off_t)(0)) )
  {
    dfprintf(( stderr, "allocate workspace: %lu bytes", (unsigned long)(info.st_size) ));
    return info.st_size;
  }
# endif
  dfprintf(( stderr, "allocate default workspace: %lu bytes", BUFSIZ ));
  return (off_t)(BUFSIZ);
}

static
size_t add_escape( iconv_t *iconv_map, char *msgbuf, wchar_t code )
{
/* A trivial helper function, for encoding an escape sequence into the
 * compiled message stream.
 */
  dfprintf(( stderr, DCODEFMT, code ));
  return iconv_wctomb( msgbuf, code );
}

static
char *update_workspace( char *buf, char *cache, unsigned int count )
{
# ifdef DEBUG
  unsigned int xcount = count;
  char *start = buf;
# endif
  while( count-- )
    *buf++ = *cache++;
# ifdef DEBUG
  *buf = '\0';
  fprintf( stderr, "cache flush: %u byte%s%s", xcount, xcount ? (xcount == 1 ? ": " : "s: ") : "s", start );
# endif
  return buf;
}

struct msgdict *mc_source( const char *input )
{
# define CODESET_DECLARED       codeset_decl_src, codeset_decl_lineno

  long accumulator;
  int fd, input_fd, count;
  char buf[BUFSIZ], keyword[64];
  char *id;

  unsigned int status = NEWLINE;
  unsigned int linenum = 0;

  unsigned int setnum = 0;
  unsigned int msgnum = 0;
  unsigned int xcount = 0;

  wchar_t quote = L'\0';

  struct msgdict *head = NULL;
  struct msgdict *tail = NULL;

  static char *codeset = NULL;
  static const char *codeset_decl_src = NULL;
  static unsigned int codeset_decl_lineno = 0;
  static iconv_t iconv_map[2] = {(iconv_t)(-1), (iconv_t)(-1)};
  char *messages; off_t msgloc, headroom;

  const char *dev_stdin = "/dev/stdin";
  if( (strcmp( input, "-") == 0) || (strcmp( input, dev_stdin ) == 0) )
  {
    input_fd = fd = STDIN_FILENO;
    input = dev_stdin;
  }

  else if( (input_fd = fd = open( input, O_RDONLY | O_BINARY )) < 0 )
    return NULL;

  dfprintf(( stderr, "\n%s:new source file\n%s:", input, input ));
  if( (messages = mc_malloc( headroom = wanted( fd ))) == NULL )
    return NULL;

  msgloc = (off_t)(0);
  while( (fd >= 0) && ((count = read( fd, buf, sizeof( buf ) )) > 0) )
  {
    char *p = buf;
    int high_water_mark = count - ( count >> 2 );
    dfprintf(( stderr, "\n%s:%u:read %u byte%s", input, linenum, count, count == 1 ? "" : "s" ));
    while( count > 0 )
    {
      wchar_t c;
      int skip = 1;
      if( status & ENCODED )
      {
        /* We are parsing context which is defined in the codeset
         * of the current message catalogue locale, so ensure that
         * we have established an appropriate codeset mapping.
         */
        if( codeset == NULL )
        {
	  /* No codeset mapping is yet in place,
	   * so default to the codeset of the system locale.
	   */
          codeset = map_codeset( iconv_map, mc_default_codeset(), "wchar_t" );
	  codeset_decl_lineno = linenum;
	  codeset_decl_src = input;
        }
          
        /* Now we may safely interpret the input according to the
         * multibyte character codeset specified for the message catalogue,
         * transforming to the wide character domain, for local processing.
         */
        p += ((skip = iconv_mbtowc( &c, p, count )) > 0) ? skip : 0;
      }

      else
      {
        /* We are parsing context which is defined in the POSIX,
         * or "C" locale, so read single byte character sequences.
         */
        c = (wchar_t)(*p++);
      }

      if( skip > 0 )
      {
        count -= skip;
        if( status & NEWLINE )
        {
          /* We just started parsing a new input line ...
           * Increment the line number, reset the parser context,
           * and clear the set/message number accumulator.
           */

          ++linenum;
          status &= ~( DIRECTIVE | NUMERIC | CATEGORY );
          accumulator = 0;

          if( (status & (NEWLINE | CONTINUED)) == NEWLINE )
          {
            /* When this new line is NOT simply a logical continuation
             * of the previous line...
             */

            status &= ~MSGTEXT;
            dfprintf(( stderr, "\n\n%s:%d:new input record", input, linenum ));
            if( c == '$' )
            {
              /* '$' as the FIRST character of the logical line
               * means that this line is either a `gencat' directive,
               * or it's a comment.
               */

              status |= DIRECTIVE;
              id = keyword;
            }

            else if( isdigit( c ) )
            {
              /* This is a message definition line,
               * with a the message identified by an explicit numeric key.
               */

              status |= NUMERIC;
              accumulator = c - L'0';
            }
          }

          else if( status & MSGTEXT )
          {
            /* When this new line IS a continuation line,
             * and it is part of a message, which is being defined,
             * then we need to include the current input character
             * as part of the message definition.
             */

            if( c == quote )
            {
              dfprintf(( stderr, "\n%s:%u:%s quoted context", input, linenum, (status & QUOTED) ? "end" : "begin" ));
              status = (status ^ QUOTED) | FLUSH;
            }

            else
            {
              xcount += skip;
              dfputc(( c, stderr ));
            }
          }

          /* Now, we dealt with the new line conditions,
           * so clear the related NEWLINE and CONTINUATION flags.
           */

          status &= ~( NEWLINE | CONTINUED );
        }

        else if( status & DIRECTIVE )
        {
          /* The input parser is in the directive identification state,
           * which persists until a space character marks the end of the
           * directive identifying keyword.
           */

          if( isspace( c ) )
          {
            /* We found the keyword delimiting space ...
            */

            if( id == keyword )
            {
              /* But, we didn't find any keyword...
               *
               * This is a comment line, but it may be the special case of
	       * a codeset declaration comment, so we can't simply ignore it;
	       * set the comment state, to parse any codeset assignment.
               */

	      status = (status & ~CATEGORY) | DEFCONV;
              dfprintf(( stderr, "\n%s:%u:record type: comment", input, linenum ));
            }

            else if( (status & CATEGORY) == DEFCONV )
	    {
	      status &= ~(DIRECTIVE | DEFCONV);
	      if( strncmp( "codeset=", keyword, 8 ) == 0 )
	      {
		*id = '\0';
		id = strdup( keyword + 8 );
		if( codeset == NULL )
		{
		  if( (codeset = map_codeset( iconv_map, id, "wchar_t" )) == NULL )
		  {
		    free( id );
		  }

		  else
		  {
		    codeset_decl_lineno = linenum;
		    codeset_decl_src = input;
		  }
		}

		else
		{
		  if( strcmp( codeset, id ) != 0 )
		  {
		    fprintf( errmsg( MSG_CODESET_CLASH ), input, linenum, id );
		    fprintf( errmsg( MSG_HAD_CODESET ), CODESET_DECLARED, codeset );
		  }
		  free( id );
		}
		dfprintf(( stderr, "; declare %s", keyword ));
	      }
	    }

	    else
            {
              /* This line has the format of a gencat directive.
	       * We have identified a possible match for a directive keyword;
               * identify it, and establish its associated parser state.
               */

              *id = '\0';
              status = mc_directive( status, keyword );
              dfprintf(( stderr, "\n%s:%u:record type: directive: %s", input, linenum, keyword ));
              dfprintf(( stderr, ": (status = %#4.4x)", status ));
            }
          }

          else
          {
            /* We are still parsing a potential directive keyword;
             * add the current character to the keyword parse buffer.
             */

            if( (id - keyword) < (sizeof( keyword ) - 1) )
              *id++ = c;
          }
        }

        else if( status & NUMERIC )
        {
          /* We are parsing a numeric value...
          */

          if( isdigit( c ) )
          {
            /* ...and the current character is part of the number,
             * so add it into the accumulator.
             */

            accumulator = accumulator * 10 + c - L'0';
          }

          else if( isspace( c ) )
          {
            /* We have reached the end of the number,
             * so hand it off as a set number, or a message number,
             * and process as appropriate.
             */

            switch( status & CATEGORY )
            {
              case ADDSET:
                /*
                 * Invoked by a "set" directive,
                 * open a new numbered message set within the catalogue ...
                 *
                 */

                dfprintf(( stderr, ": add set with id = %ld", accumulator ));
                if( accumulator > setnum )
                {
                  /* POSIX requires "set" directive entries to be presented
                   * in strictly ascending, (but not necessarily contiguous),
                   * "setnum" order, within the message source file.
                   *
                   * This entry satisfies the ascending "setnum" requirement,
                   * so we can simply create a new message set with this "setnum",
                   * and reset the "msgnum", for the start of a new set.
                   */

                  setnum = accumulator;
                  msgnum = 0;
                }

                else
                {
                  /* This "setnum" entry DOESN'T satisfy the ascending order rule,
                   * so complain, and bail out.
                   */

                  dfputc(( '\n', stderr ));
                  gencat_errno = errout( FATAL( MSG_SETNUM_NOT_INCR ), setnum, accumulator );
		  return NULL;
                }
                break;

              case DELSET:
                /*
                 * Invoked by a "delset" directive,
                 * mark a numbered message set for deletion from the catalogue.
		 *
                 */
                dfprintf(( stderr, ": delete set with id = %ld", accumulator ));
		if( (accumulator > 0) && (accumulator <= NL_SETMAX) )
		{
                  struct msgdict *this;
                  if( (this = mc_malloc( sizeof( struct msgdict ))) != NULL )
                  {
                    /* We successfully created an empty dictionary slot,
		     * so fill it in as a `delset' request entry.
		     */

		    this->src = input;
		    this->lineno = linenum;
		    this->base = NULL;
		    this->set = accumulator;
		    this->msg = 0;
                    if( head == NULL )
                    {
                      /* The catalogue currently contains no records,
                       * so simply insert this as the first one.
                       */

                      head = tail = this;
                      this->link = NULL;
                    }

                    else
                    {
                      /* We've already added some message records,
                       * so the new one must be added at the end.
                       */

                      this->link = tail->link;
                      tail->link = this;
                      tail = this;
                    }
		  }
		}
                break;

              default:
                dfprintf(( stderr, "\n%s:%u:record type: message with id = %ld", input, linenum, accumulator ));
                if( accumulator > msgnum )
                {
                  /* POSIX also requires "msgnum" to be greater than any previous
                   * message defined in the current set; this declaration satisfies
                   * this requirement, so add a new message to the catalogue.
                   */

                  struct msgdict *this;
                  if( (this = mc_malloc( sizeof( struct msgdict ))) != NULL )
                  {
                    /* We successfully created an empty dictionary slot,
                     * so we may proceed to complete the message details.
                     * The message must be assigned to a numbered set, so
                     * first check that one has been opened; if not, we
                     * simply open the default set.
                     */

                    if( setnum == 0 )
                      setnum = NL_SETD;

                    /* We may now complete the message details in the new
                     * dictionary slot, and commit the record to the catalogue.
                     */

		    this->src = input;
		    this->base = messages;
		    this->lineno = linenum;
                    this->set = setnum;
                    this->msg = msgnum = accumulator;
                    this->loc = msgloc;
                    if( head == NULL )
                    {
                      /* The catalogue currently contains no records,
                       * so simply insert this as the first one.
                       */

                      head = tail = this;
                      this->link = NULL;
                    }

                    else
                    {
                      /* We've already added some message records,
                       * so the new one must be added at the end.
                       */

                      this->link = tail->link;
                      tail->link = this;
                      tail = this;
                    }
                  }
                }

                else
                {
                  /* This doesn't satisfy the requirement for incrementing "msgnum",
                   * so complain, and bail out.
                   */

                  dfputc(( '\n', stderr ));
                  gencat_errno = errout( FATAL( MSG_MSGNUM_NOT_INCR ), msgnum, accumulator );
		  return NULL;
                }
                status |= ( MSGTEXT | ENCODED );
            }
            status &= ~( NUMERIC | CATEGORY );
          }

          else
          {
            dfprintf(( stderr, "index (abnormally terminated): %ld", accumulator ));
            status &= ~NUMERIC;
          }
        }

        else if( (status & CATEGORY) == DEFQUOTE )
        {
          /* This is the normal use of the "quote" directive,
           * followed by one delimiting space, with the next following character 
           * defining the "quote" character to be used, or "none" if no other
           * character appears before end of line.
           */

          quote = (c == L'\n') ? L'\0' : c;
          dfprintf(( stderr, quote ? ": assigned as %#4.4x" : ": none assigned", quote ));
          status &= ~( CATEGORY | ENCODED );
        }

        else if( status & MSGTEXT )
        {
          /* We are compiling a message ...
           * Continue scanning the current input line,
           * until we find the end-of-line marker.
           */

          if( c != L'\n' )
          {
            /* We haven't reached end-of-line yet...
             * Check for other characters with special significance.
             */

            if( status & ESCAPE )
            {
              /* The current input character was escaped...
               * Clear the ESCAPE flag, and interpret this case.
               */

	      size_t len = 0;
              status &= ~ESCAPE;
              switch ( c )
              {
                case L'r':      /* embed a carriage return */
		  len = add_escape( iconv_map, messages + msgloc, L'\r' );
                  break;

                case L'n':      /* embed a newline */
		  len = add_escape( iconv_map, messages + msgloc, L'\n' );
                  break;

                default:        /* not a special case; just pass it through */
                  xcount += skip;
                  dfputc(( c, stderr ));
              }
	      if( len > (size_t)(0) )
	      {
		headroom -= len;
		msgloc += len;
	      }
            }

            else if( c == L'\\' )
            {
              /* This is the escape character...
               * Set the parser flags, so that any cached message data is flushed,
               * and switch to ESCAPE mode, to interpret the next character.
               */

              status |= FLUSH | ESCAPE;
            }

            else if( c == quote )
            {
              dfprintf(( stderr, "\n%s:%u:%s quoted context", input, linenum, (status & QUOTED) ? "end" : "begin" ));
              status = (status ^ QUOTED) | FLUSH;
            }

            else
            {
              xcount += skip;
              dfputc(( c, stderr ));
            }
          }
	  if( count < ICONV_MB_LEN_MAX )
	  {
	    skip = 0;
	    status |= FLUSH;
	  }
        }

        if( c == L'\n' )
        {
          /* Mark the end of the current input line,
           * and schedule any pending message data from this line
           * for flushing to the message collection buffer.
           */

          status |= NEWLINE | FLUSH;

          /* If "QUOTED" context remains active, at the end of this line,
           * then we have an implicit continuation, so force it.
           */

          if( (status & QUOTED) == QUOTED )
            status |= CONTINUED;

          /* Clean up the context of any pending directive processing.
           */

          switch( status & CATEGORY )
          {
            case DEFQUOTE:
              /*
               * If we see end of line with a DEFQUOTE pending,
               * then there was no defining character with the "quote" directive,
               * so we must disable "quote" character recognition.
               */

              quote = L'\0';
              dfprintf(( stderr, ": none assigned" ));
              break;
          }

          if( (status & CONTINUED) == 0 )
          {
            status &= ~ENCODED;
          }
        }
      }

      if( status & FLUSH )
      {
        /* We have pending message data in the input cache,
         * which now needs to be flushed to the output queue,
         * BEFORE we proceed to the next cycle.
         */
        while( headroom < (xcount + ICONV_MB_LEN_MAX) )
        {
          headroom += BUFSIZ;
          dfprintf(( stderr, "<grow allocation to %u bytes>", (unsigned)(msgloc + headroom) ));
          if( (messages = realloc( messages, msgloc + headroom )) == NULL )
	  {
            gencat_errno = errout( FATAL( MSG_OUT_OF_MEMORY ));
	    return NULL;
	  }
        }
        headroom -= xcount;
	dfprintf(( stderr, "\n%s:%u:", input, linenum ));
        msgloc = update_workspace( messages + msgloc, p - xcount - skip, xcount ) - messages;
	dfprintf(( stderr, "; %u byte%s free\n", headroom, headroom == 1 ? "" : "s" ));
        if( (status & (MSGTEXT | NEWLINE | CONTINUED)) == (MSGTEXT | NEWLINE) )
        {
          wchar_t terminator = L'\0';
	  if( codeset == NULL )
	  {
	    /* No codeset mapping is yet in place,
	     * so default to the codeset of the system locale.
	     */
	    codeset = map_codeset( iconv_map, mc_default_codeset(), "wchar_t" );
	    codeset_decl_lineno = linenum;
	    codeset_decl_src = input;
	  }
          int xcount = iconv_wctomb( messages + msgloc, terminator );
          if( xcount >= 0 )
          {
            dfprintf(( stderr, "\n%s:%u:end of message; terminator added: %d byte(s)", input, linenum, xcount ));
            msgloc += xcount;
            headroom -= xcount;
          }
          else
          {
            dfprintf(( stderr, "\n%s:%u:end of message: add terminator failed", input, linenum ));
          }
          tail->len = msgloc - tail->loc;
        }
        status &= ~FLUSH;
        xcount = 0;

	/* Keep the input buffer filled,
	 * as we parse beyond the high water mark.
	 */
	if( (p - buf) > high_water_mark )
	{
	  int ref;
	  char *copyptr;
	  for( copyptr = buf; count; count-- )
	    *copyptr++ = *p++;
	  p = buf; ref = count = copyptr - p;
	  dfprintf(( stderr, "\n%s:%u:input count depleted: %u byte%s remaining", input, linenum, count, count == 1 ? "" : "s" ));
	  if( (fd >= 0)
	  &&  (ref == (count += read( fd, copyptr, sizeof( buf ) - count )))  )
	    fd = -1;
	  dfprintf(( stderr, "; read new input: count adjusted to %u byte%s", count, count == 1 ? "" : "s" ));
	  high_water_mark = count - ( count >> 2 );
	}
      }
    }
    dfprintf(( stderr, "\n%s:end of input; (count is now %d bytes)", input, count ));
    /*
     * We reached the end of the input stream.
     * If the final record was a message definition,
     * then the MSGTEXT parser state will still be active;
     * this state would be cancelled immediately, at the start of the next cycle,
     * but becuase there is no more input data, we will not start another cycle.
     * To avoid misidentifying this case as an incomplete final message,
     * and so displaying an erroneous warning,
     * we clear this state now.
     */
    if(  (count == 0)
    &&  ((status & (MSGTEXT | NEWLINE | CONTINUED)) == (MSGTEXT | NEWLINE))  )
      status &= ~MSGTEXT;
  }
  /*
   * At the end of the current input file...
   * Check that the parser finished in an appropriate termination state.
   */
  if( status & QUOTED )
  {
    /* Abnormal termination...
     * EOF was encountered within a quoted literal, before the closing
     * quote was found; diagnose abnormal termination state.
     */
    fprintf( errmsg( MSG_EOF_IN_QUOTES ), input, linenum );
  }

  if( (status & NEWLINE) != NEWLINE )
  {
    /* Abnormal termination...
     * The input file lacks a terminating newline; diagnose abnormal
     * termination state.
     */
    fprintf( errmsg( MSG_MISSING_NEWLINE ), input, linenum );
  }

  if( status & MSGTEXT )
  {
    /* Abnormal termination...
     * EOF was encountered while parsing a continued message definition;
     * dignose abnormal termination state, and mark incomplete message
     * for deletion.
     */
    fprintf( errmsg( MSG_TEXT_DISCARDED ), input, tail->lineno );
    tail->base = NULL;
  }

  /* After completing the construction of the message list,
   * adjust its allocated memory size to the actual size used,
   * then point all index entries to the resultant data block.
   */
  messages = mc_realloc( messages, (unsigned)(msgloc) );
  dfprintf(( stderr, "\n\nAllocation adjusted to %u bytes\n", (unsigned)(msgloc) ));
  for( tail = head; tail != NULL; tail = tail->link )
  {
    if( tail->base != NULL )
      /*
       * Update index entries *except* those with a NULL base pointer;
       * (those which are NULL based indicate entities to be deleted!!!)
       */
      tail->base = messages;
    dfprintf(( stderr, "\nindex: %#08lx; text: %s", tail->key, messages + tail->loc ));
  }
  dfputc(( L'\n', stderr ));

  /* We are done with the current input source;
   * close its file descriptor, and return the message list.
   */
  close( input_fd );
  return head;
}

/* $RCSfile: mcsource.c,v $Revision: 1.4 $: end of file */

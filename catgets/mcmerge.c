/*
 * mcmerge.c
 *
 * $Id: mcmerge.c,v 1.3 2007-05-12 22:51:10 keithmarshall Exp $
 *
 * Copyright (C) 2006, Keith Marshall
 *
 * This file implements the `mc_merge' function, which is used by
 * `gencat' to merge the compiled message dictionary derived from
 * any single source file into its current internal dictionary.
 *
 * Written by Keith Marshall  <keithmarshall@users.sourceforge.net>
 * Last modification: 12-May-2007
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <debug.h>

#include <stdio.h>
#include <stdlib.h>

#include <gencat.h>
#include <gcmsgs.h>

static
struct msgdict *mc_delset( unsigned short setnum, struct msgdict *msgset )
{
  /* Helper function, called by `mc_merge', to unlink message references
   * in any set specified in a `delset' directive.
   */
  while( msgset && (msgset->set == setnum) )
  {
    /* Walk the message list, freeing the memory allocated to message
     * entries with matching set number.
     */
    struct msgdict *ref = msgset;
    msgset = msgset->link;
    free( ref );
  }
  /* When done, return the address of the first message in the set,
   * if any, following the one we just deleted; `mc_merge' will link
   * this to succeed the predecessor, if any, of the deleted set,
   * or otherwise, replacing the first set in the catalogue,
   * thus implicitly unlinking the deleted messages.
   */
  return msgset;
}

struct msgdict *mc_merge( struct msgdict *cat, struct msgdict *input )
{
  struct msgdict *mark = cat;

# ifdef DEBUG
  if( input )
  {
    struct msgdict *curr;
    if( cat == NULL )
    {
      fprintf( stderr, "no existing messages in catalogue; loading %s\n", input->src );
    }
    else
    {
      fprintf( stderr, "merge messages from %s\n", input->src );
      for( curr = cat; curr != NULL; curr = curr->link )
	fprintf( stderr, "%s:%u: set %u message %u\n", curr->src, curr->lineno, curr->set, curr->msg );
    }
    for( curr = input; curr != NULL; curr = curr->link )
      fprintf( stderr, "%s:%u: set %u message %u\n", curr->src, curr->lineno, curr->set, curr->msg );
  }
# endif

  while( input )
  {
    /* Save a pointer to the *next* input record;
     * we will use this later, to progress to the next record,
     * after we've freed the current reference structure.
     */
    struct msgdict *next = input->link;
    
    dfprintf(( stderr, "Process entry from %s line %u\n", input->src, input->lineno ));

    if( input->set && input->msg )
    {
      /* The input reference specifies both set and message numbers.
       * Thus, the operation to be performed relates to a single message,
       * which is to be inserted, replaced or deleted at the appropriate
       * position within the current message list; before proceeding,
       * we must locate this position.
       */
      struct msgdict *curr = mark;
      while( curr && (curr->key < input->key) )
      {
	/* We loop over the existing messages in the dictionary,
	 * until we find the location at which the input message
	 * should be placed; when we find it, we leave `curr'
	 * pointing at the target location reference, while
	 * `mark' points to its immediate predecessor.
	 */
	mark = curr;
	curr = curr->link;
      }

      /* Now that we've identified *where* the current input reference
       * belongs, in the current message list, we may determine *what*
       * action is to be performed for this record.
       */
      if( input->base && (curr == NULL) )
      {
	/* The input record specifies actual message content,
	 * which is to be appended to the end of the message list...
	 */
	if( mark )
	{
	  /* ...extending an existing message list.
	  */
	  input->link = mark->link;
	  mark->link = input;
	  dfprintf(( stderr, "Append set %u message %u after set %u message %u\n",
	               input->set, input->msg, mark->set, mark->msg
	          ));
	}
	else
	{
	  /* ...there is no current message list; start one now!
	   */
	  cat = mark = input;
	  mark->link = NULL;
	  dfprintf(( stderr, "Initialise message list at set %u message %u\n", mark->set, mark->msg ));
	}
      }
      else if( curr->key == input->key )
      {
	/* The input record refers to a message which is already present
	 * in the current message list; the operation to be performed is
	 * either deletion or replacement.
	 */
	if( input->base == NULL )
	{
	  /* This input reference specifies no message content;
	   * it is a request to delete the existing message.
	   */
	  if( curr == cat )
	    /*
	     * We are deleting the first message in the current list,
	     * so we simply reassign the initial list entry.
	     */
	    cat = cat->link;

	  else
	    /* We are deleting a message from within the list, so we
	     * relink the immediately preceding reference, to chain it
	     * directly to the immediately succeeding one.
	     */
	    mark->link = curr->link;

	  /* In either case, we may release the the memory allocated to
	   * the defunct dictionary reference for the deleted message.
	   */
	  free( curr );
	}

	else if( curr->lineno > 0 )
	{
	  /* This a redefinition of a message which has previously been
	   * sourced from another input file in the current input stream;
	   * this represents a collision, which is probably unintentional,
	   * so diagnose it, and ignore this redefinition.
	   */
	  fprintf( errmsg( MSG_REDEFINED ), GENCAT_MSG_INPUT, curr->msg, curr->set );
	  fprintf( errmsg( MSG_PREVIOUS_HERE ), GENCAT_MSG_SRC( curr ));
	}

	else
	{
	  /* This is replacement of an existing message, which has
	   * been inherited from a previously existing message catalogue;
	   * such replacement is assumed to be intentional, so...
	   */
	  dfprintf(( stderr, "Replace set %u message %u\n", curr->set, curr->msg ));
	  if( curr == cat )
	  {
	    /* ...when this is the first message in the current list,
	     * we simply replace it.
	     */
	    cat = input;
	  }

	  else
	  {
	    /* ...but when there are preceding messages in the list,
	     * we must relink the predecessor to this replacement.
	     */
	    mark->link = input;
	  }

	  /* Now, to complete the replacement of the original message,
	   * link any successor to the new reference, release the memory
	   * allocated to the replaced dictionary entry, and mark the
	   * new entry as the current reference point.
	   */
	  input->link = curr->link;
	  free( curr );
	  mark = input;
	}
      }
      else
      {
	/* There is no existing reference with set and message numbers
	 * matching the current input record; thus the input record must
	 * be inserted between the existing `mark' and `curr' entries.
	 */
	dfprintf(( stderr, "Insert set %u message %u ", input->set, input->msg ));
	if( mark == curr )
	{
	  /* The `curr' entry is the first in the existing dictionary,
	   * so we insert the input record before it, as the new initial
	   * entry in the dictionary, leaving `mark' pointing to it.
	   */
	  dfprintf(( stderr, "at start of list " ));
	  cat = mark = input;
	}
	else
	{
	  /* There is already an existing message which must precede this
	   * new entry in the dictionary, so we link this new entry as its
	   * immediate successor.
	   */
	  dfprintf(( stderr, "after set %u message %u and ", mark->set, mark->msg ));
	  mark->link = input;
	}
	/*
	 * In either case, any existing `curr' entry must be linked as
	 * successor to the inserted entry.
	 */
	dfprintf(( stderr, "before set %u message %u\n", curr->set, curr->msg ));
	input->link = curr;
      }
    }

    else if( input->set && (input->base == NULL) )
    {
      /* This is a a `delset' operation; it will delete all messages
       * currently present in the catalogue, which are assigned to the
       * specified message set.
       *
       * Obviously, if the catalogue is currently empty, then this is
       * a no-op...
       */
      if( cat )
      {
	/* ...but when we do already have some content defined, then we
	 * must locate and delete the specified set, if it exists.
	 */
	if( cat->set == input->set )
	{
	  /* The set to be deleted is the first in the catalogue...
	   */
	  if( mark->set == input->set )
	    /*
	     * ...and the current insertion point also lies within
	     * this set, so *both* references must be adjusted.
	     */
	    mark = cat = mc_delset( input->set, cat );

	  else
	    /* ...the current insertion point has already advanced
	     * beyond the set to be deleted, so leave it unchanged,
	     * and simply remove the entire initial message set.
	     */
	    cat = mc_delset( input->set, cat );
	}
	else
	{
	  /* The set to be deleted is not the first in the catalogue,
	   * so we must locate the last message entry in the set which
	   * immediately precedes that to be deleted; this becomes the
	   * reference point, from which deletion commences, and to
	   * which any following message set must be relinked.
	   */
	  struct msgdict *delset, *ref = cat;
	  
	  /* Our search for this reference entry begins at the start
	   * of the catalogue; however, if the current insertion point
	   * lies in a set which precedes that to be deleted, then we
	   * may immediately jump ahead to this point.
	   */
	  if( mark->key < input->key )
	    ref = mark;

	  /* We now walk the message list, until we either find the
	   * required reference point, or our search overruns the end
	   * of the list.
	   */
	  while( ((delset = ref->link) != NULL) && (delset->key < input->key) )
	    ref = delset;

	  /* Now, we must check the current insertion point, to ensure
	   * that * it does not refer to an entry we are about to delete;
	   * if it points to a message with a set number matching that of
	   * the set to be deleted, then we must adjust it.
	   */
	  if( mark->set == input->set )
	    mark = ref;

	  /* Finally, if we found entries in the set to be deleted,
	   * then we discard them.
	   */
	  if( delset->set == input->set )
	    ref->link = mc_delset( input->set, delset );
	}
      }
      /* Whether or not we actually found and deleted the specified
       * message set, (if not, we silently ignore the `delset' request),
       * the current input record serves no further purpose.
       */
      free( input );
    }

    else
    {
      /* The input record contains an improperly formed message reference.
       * We should *never* get to here!  If we do, then the entire message
       * catalogue dictionary structure is invalid; diagnose and abort.
       */
      fprintf( errmsg( MSG_INTERNAL_ERROR ), progname, msgarg( MSG_BAD_INDEX ) );
      exit( EXIT_FAILURE );
    }

    /* Whatever operation we have just performed, if no fatal error
     * occurred we proceed to the next input record, if any, using the
     * pointer we saved earlier, since `input->link' may not now be
     * a valid reference; (`input' may have been `freed').
     */
    input = next;
  }
# ifdef DEBUG
  if( cat )
  {
    struct msgdict *curr;
    for( curr = cat; curr != NULL; curr = curr->link )
      fprintf( stderr, "%s:%u: set %u message %u\n", curr->src, curr->lineno, curr->set, curr->msg );
  }
# endif

  /* When all input records have been processed,
   * we return the resultant message catalogue index to the caller.
   */
  return cat;
}

/* $RCSfile: mcmerge.c,v $Revision: 1.3 $: end of file */

/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>	/* defines BUFSIZ */
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

/*
 * This file implements the input routines used by the parser.
 */

#include "alias.h"
#include "error.h"
#include "eval.h"
#include "input.h"
#include "main.h"
#include "memalloc.h"
#include "myhistedit.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "redir.h"
#include "shell.h"
#include "syntax.h"
#include "trap.h"

#define IBUFSIZ (BUFSIZ + PUNGETC_MAX + 1)


MKINIT struct parsefile basepf;	/* top level input file */
MKINIT char basebuf[IBUFSIZ];	/* buffer for top level input file */
MKINIT struct parsefile *toppf = &basepf;
struct parsefile *parsefile = &basepf;	/* current input file */
int whichprompt;		/* 1 == PS1, 2 == PS2 */

STATIC void pushfile(void);
static void popstring(void);
static int preadfd(void);
static void setinputfd(int fd, int push);
static int preadbuffer(void);

#ifdef mkinit
INCLUDE <stdio.h>
INCLUDE <unistd.h>
INCLUDE "input.h"
INCLUDE "error.h"
INCLUDE "syntax.h"

INIT {
	basepf.nextc = basepf.buf = basebuf;
	basepf.linno = 1;
}

RESET {
	int c;

	/* clear input buffer */
	popallfiles();

	c = PEOF;
	if (toppf->nextc - toppf->buf > toppf->unget)
		c = toppf->nextc[-toppf->unget - 1];
	while (c != '\n' && c != PEOF && !int_pending())
		c = pgetc();
}

FORKRESET {
	popallfiles();
	if (parsefile->fd > 0) {
		close(parsefile->fd);
		parsefile->fd = 0;
	}
}
#endif


static void freestrings(struct strpush *sp)
{
	INTOFF;
	do {
		struct strpush *psp;

		if (sp->ap) {
			sp->ap->flag &= ~ALIASINUSE;
			if (sp->ap->flag & ALIASDEAD) {
				unalias(sp->ap->name);
			}
		}

		psp = sp;
		sp = sp->spfree;

		if (psp != &(parsefile->basestrpush))
			ckfree(psp);
	} while (sp);

	parsefile->spfree = NULL;
	INTON;
}


static int __pgetc(void)
{
	int c;

	if (parsefile->unget) {
		long unget = -(long)(unsigned)parsefile->unget--;

		if (parsefile->nleft < 0)
			return preadbuffer();

		return parsefile->nextc[unget];
	}

	if (--parsefile->nleft >= 0)
		c = (signed char)*parsefile->nextc++;
	else
		c = preadbuffer();

	return c;
}


/*
 * Read a character from the script, returning PEOF on end of file.
 * Nul characters in the input are silently discarded.
 */

int __attribute__((noinline)) pgetc(void)
{
	struct strpush *sp = parsefile->spfree;

	if (unlikely(sp))
		freestrings(sp);

	return __pgetc();
}

int pgetc_eoa(void)
{
	return parsefile->strpush && parsefile->nleft == -1 &&
	       parsefile->strpush->ap ? PEOA : pgetc();
}

static int stdin_clear_nonblock(void)
{
	int flags = fcntl(0, F_GETFL, 0);

	if (flags >= 0) {
		flags &=~ O_NONBLOCK;
		flags = fcntl(0, F_SETFL, flags);
	}

	return flags;
}

static int
preadfd(void)
{
	char *buf = parsefile->buf;
	int unget;
	int nr;

	unget = parsefile->nextc - buf;
	if (unget > PUNGETC_MAX)
		unget = PUNGETC_MAX;

	memmove(buf, parsefile->nextc - unget, unget);
	parsefile->nextc = buf += unget;

retry:
#ifndef SMALL
	if (parsefile->fd == 0 && el) {
		static const char *rl_cp;
		static int el_len;

		if (rl_cp == NULL) {
			struct stackmark smark;
			pushstackmark(&smark, stackblocksize());
			rl_cp = el_gets(el, &el_len);
			popstackmark(&smark);
		}
		if (rl_cp == NULL)
			nr = 0;
		else {
			nr = el_len;
			if (nr > BUFSIZ)
				nr = BUFSIZ;
			memcpy(buf, rl_cp, nr);
			if (nr != el_len) {
				el_len -= nr;
				rl_cp += nr;
			} else
				rl_cp = 0;
		}

	} else
#endif
	if (parsefile->fd)
		nr = read(parsefile->fd, buf, BUFSIZ);
	else {
		unsigned len = BUFSIZ;

		nr = 0;

		do {
			int err;

			err = read(0, buf, 1);
			if (err <= 0) {
				if (nr)
					break;

				nr = err;
				if (errno != EWOULDBLOCK)
					break;
				if (stdin_clear_nonblock() < 0)
					break;

				out2str("sh: turning off NDELAY mode\n");
				goto retry;
			}

			nr++;
		} while (!IS_DEFINED_SMALL && *buf++ != '\n' && --len);
	}

	if (nr < 0) {
		if (errno == EINTR && !(basepf.prev && pending_sig))
			goto retry;
	}
	return nr;
}

/*
 * Refill the input buffer and return the next input character:
 *
 * 1) If a string was pushed back on the input, pop it;
 * 2) If we are reading from a string we can't refill the buffer, return EOF.
 * 3) If there is more stuff in this buffer, use it else call read to fill it.
 * 4) Process input up to the next newline, deleting nul characters.
 */

static int preadbuffer(void)
{
	int first = whichprompt == 1;
	int something;
	char savec;
	int more;
	char *q;

	if (unlikely(parsefile->strpush)) {
		popstring();
		return __pgetc();
	}
	if (parsefile->buf == NULL)
		return PEOF;
	flushall();

	more = input_get_lleft(parsefile);
	if (more <= 0) {
again:
		if ((more = preadfd()) <= 0) {
			input_set_lleft(parsefile, parsefile->nleft = 0);
			return PEOF;
		}
	}

	q = parsefile->nextc;

	/* delete nul characters */
	something = !first;
	for (;;) {
		int c;

		more--;
		c = *q;

		if (!c) {
			memmove(q, q + 1, more);
			goto check;
		}

		q++;

		if (IS_DEFINED_SMALL)
			goto check;

		switch (c) {
		case '\n':
			parsefile->nleft = q - parsefile->nextc - 1;
			goto done;

		default:
			something = 1;
			/* fall through */

		case '\t':
		case ' ':
			break;
		}

check:
		if (more <= 0) {
			parsefile->nleft = q - parsefile->nextc - 1;
			if (parsefile->nleft < 0)
				goto again;
			break;
		}
	}
done:
	input_set_lleft(parsefile, more);

	if (!IS_DEFINED_SMALL)
		savec = *q;
	*q = '\0';

	if (parsefile->fd == 0 && hist && something) {
		HistEvent he;
		INTOFF;
		history(hist, &he, first ? H_ENTER : H_APPEND,
			parsefile->nextc);
		INTON;
	}

	if (vflag) {
		out2str(parsefile->nextc);
#ifdef FLUSHERR
		flushout(out2);
#endif
	}

	if (!IS_DEFINED_SMALL)
		*q = savec;

	return (signed char)*parsefile->nextc++;
}

void pungetn(int n)
{
	parsefile->unget += n;
}

/*
 * Undo a call to pgetc.  Only two characters may be pushed back.
 * PEOF may be pushed back.
 */

void
pungetc(void)
{
	pungetn(1);
}

/*
 * Push a string back onto the input at this current parsefile level.
 * We handle aliases this way.
 */
void
pushstring(char *s, void *ap)
{
	struct strpush *sp;
	size_t len;

	len = strlen(s);
	INTOFF;
/*dprintf("*** calling pushstring: %s, %d\n", s, len);*/
	if ((unsigned long)parsefile->strpush |
	    (unsigned long)parsefile->spfree) {
		sp = ckmalloc(sizeof (struct strpush));
		sp->prev = parsefile->strpush;
		parsefile->strpush = sp;
	} else
		sp = parsefile->strpush = &(parsefile->basestrpush);
	sp->prevstring = parsefile->nextc;
	sp->prevnleft = parsefile->nleft;
	sp->unget = parsefile->unget;
	sp->spfree = parsefile->spfree;
	sp->ap = (struct alias *)ap;
	if (ap) {
		((struct alias *)ap)->flag |= ALIASINUSE;
		sp->string = ((struct alias *)ap)->name;
	}
	parsefile->nextc = s;
	parsefile->nleft = len;
	parsefile->unget = 0;
	parsefile->spfree = NULL;
	INTON;
}

static void popstring(void)
{
	struct strpush *sp = parsefile->strpush;

	INTOFF;
	if (sp->ap && parsefile->nextc > sp->string) {
		if (parsefile->nextc[-1] == ' ' ||
		    parsefile->nextc[-1] == '\t') {
			checkkwd |= CHKALIAS;
		}
		if (sp->string != sp->ap->name) {
			ckfree(sp->string);
		}
	}
	parsefile->nextc = sp->prevstring;
	parsefile->nleft = sp->prevnleft;
	parsefile->unget = sp->unget;
/*dprintf("*** calling popstring: restoring to '%s'\n", parsenextc);*/
	parsefile->strpush = sp->prev;
	parsefile->spfree = sp;
	INTON;
}

/*
 * Set the input to take input from a file.  If push is set, push the
 * old input onto the stack first.
 */

int
setinputfile(const char *fname, int flags)
{
	int fd;

	INTOFF;
	fd = sh_open(fname, O_RDONLY, flags & INPUT_NOFILE_OK);
	if (fd < 0)
		goto out;
	if (fd < 10)
		fd = savefd(fd, fd);
	setinputfd(fd, flags & INPUT_PUSH_FILE);
out:
	INTON;
	return fd;
}


/*
 * Like setinputfile, but takes an open file descriptor.  Call this with
 * interrupts off.
 */

static void
setinputfd(int fd, int push)
{
	pushfile();
	if (!push)
		toppf = parsefile;
	parsefile->fd = fd;
	parsefile->nextc = parsefile->buf = ckmalloc(IBUFSIZ);
	input_set_lleft(parsefile, parsefile->nleft = 0);
	plinno = 1;
}


/*
 * Like setinputfile, but takes input from a string.
 */

void
setinputstring(char *string)
{
	INTOFF;
	pushfile();
	parsefile->nextc = string;
	parsefile->nleft = strlen(string);
	parsefile->buf = NULL;
	plinno = 1;
	INTON;
}



/*
 * To handle the "." command, a stack of input files is used.  Pushfile
 * adds a new entry to the stack and popfile restores the previous level.
 */

STATIC void
pushfile(void)
{
	struct parsefile *pf;

	pf = (struct parsefile *)ckmalloc(sizeof (struct parsefile));
	pf->prev = parsefile;
	pf->fd = -1;
	pf->strpush = NULL;
	pf->spfree = NULL;
	pf->basestrpush.prev = NULL;
	pf->unget = 0;
	parsefile = pf;
}

void pushstdin(void)
{
	INTOFF;
	basepf.prev = parsefile;
	parsefile = &basepf;
	INTON;
}

void
popfile(void)
{
	struct parsefile *pf = parsefile;

	INTOFF;
	parsefile = pf->prev;
	pf->prev = NULL;
	if (pf == &basepf)
		goto out;

	if (pf->fd >= 0)
		close(pf->fd);
	if (pf->buf)
		ckfree(pf->buf);
	if (parsefile->spfree)
		freestrings(parsefile->spfree);
	while (pf->strpush) {
		popstring();
		freestrings(parsefile->spfree);
	}
	ckfree(pf);

out:
	INTON;
}


void __attribute__((noinline)) unwindfiles(struct parsefile *stop)
{
	while (basepf.prev || parsefile != stop)
		popfile();
}


/*
 * Return to top level.
 */

void
popallfiles(void)
{
	unwindfiles(toppf);
}

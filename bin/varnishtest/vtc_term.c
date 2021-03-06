/*-
 * Copyright (c) 2018 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * A trivial ANSI terminal emulation
 */

#include "config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "vtc.h"

struct term {
	unsigned		magic;
#define TERM_MAGIC		0x1c258f0f

	struct vtclog		*vl;
	unsigned		state;
#define NTERMARG		10
	int			arg[NTERMARG];
	int			*argp;
	int			nlin;
	int			ncol;
	char			**vram;
	int			col;
	int			line;
};

static void
term_clear(char * const *vram, int lin, int col)
{
	int i;

	for (i = 0; i < lin; i++) {
		memset(vram[i], ' ', col);
		vram[i][col] = '\0';
	}
}

static void
term_scroll(const struct term *tp)
{
	int i;
	char *l;

	l = tp->vram[0];
	for (i = 0; i < tp->nlin -1; i++)
		tp->vram[i] = tp->vram[i + 1];
	tp->vram[i] = l;
	memset(l, ' ', tp->ncol);
}

void
Term_Dump(const struct term *tp)
{
	int i;

	for (i = 0; i < tp->nlin; i++)
		vtc_dump(tp->vl, 3, "screen", tp->vram[i], tp->ncol);
}

static void
term_escape(struct term *tp, int c, int n)
{
	int i;

	switch (c) {
	case 'A':
		// CUU - Cursor up
		if (tp->arg[0] == -1) tp->arg[0] = 1;
		tp->line -= tp->arg[0];
		if (tp->line < 0)
			vtc_fatal(tp->vl, "ANSI A[%d] outside vram",
			    tp->arg[0]);
		break;
	case 'B':
		// CUD - Cursor down
		if (tp->arg[0] == -1) tp->arg[0] = 1;
		if (tp->arg[0] > tp->nlin)
			vtc_fatal(tp->vl, "ANSI B[%d] outside vram",
			    tp->arg[0]);
		tp->line += tp->arg[0];
		while (tp->line >= tp->nlin) {
			term_scroll(tp);
			tp->line--;
		}
		break;
	case 'C':
		// CUF - Cursor forward
		if (tp->arg[0] == -1) tp->arg[0] = 1;
		tp->col += tp->arg[0];
		if (tp->col >= tp->ncol)
			tp->col = tp->ncol - 1;
		break;
	case 'D':
		// CUB - Cursor backward
		if (tp->arg[0] == -1) tp->arg[0] = 1;
		tp->col -= tp->arg[0];
		if (tp->col < 0)
			tp->col = 0;
		break;
	case 'h':
		// SM - Set Mode (mostly ignored XXX?)
		tp->col = 0;
		tp->line = 0;
		break;
	case 'H':
		// CUP - Cursor Position
		if (tp->arg[0] == -1) tp->arg[0] = 1;
		if (tp->arg[1] == -1) tp->arg[1] = 1;
		if (tp->arg[0] > tp->nlin || tp->arg[1] > tp->ncol)
			vtc_fatal(tp->vl, "ANSI H[%d,%d] outside vram",
			    tp->arg[0], tp->arg[1]);
		tp->line = tp->arg[0] - 1;
		tp->col = tp->arg[1] - 1;
		break;
	case 'J':
		// ED - Erase in Display (0=below, 1=above, 2=all)
		switch (tp->arg[0]) {
		case 2:
			term_clear(tp->vram, tp->nlin, tp->ncol);
			break;
		default:
			vtc_fatal(tp->vl, "ANSI J[%d]", tp->arg[0]);
		}
		break;
	case 'K':
		// EL - Erase in line (0=right, 1=left, 2=full line)
		if (tp->arg[0] == -1) tp->arg[0] = 0;
		switch (tp->arg[0]) {
		case 0:
			for (i = tp->col; i < tp->ncol; i++)
				tp->vram[tp->line][i] = ' ';
			break;
		case 1:
			for (i = 0; i < tp->col; i++)
				tp->vram[tp->line][i] = ' ';
			break;
		case 2:
			for (i = 0; i < tp->ncol; i++)
				tp->vram[tp->line][i] = ' ';
			break;
		default:
			vtc_fatal(tp->vl, "ANSI K[%d]", tp->arg[0]);
		}
		break;
	case 'm':
		// SGG - Character Attributes (ignored)
		break;
	default:
		for (i = 0; i < n; i++)
			vtc_log(tp->vl, 4, "ANSI arg %d", tp->arg[i]);
		vtc_fatal(tp->vl, "ANSI unknown (%c)", c);
		break;
	}
}

static void
term_char(struct term *tp, char c)
{
	assert(tp->col < tp->ncol);
	assert(tp->line < tp->nlin);
	assert(tp->state <= 3);
	switch (c) {
	case 0x00:
		break;
	case '\b':
		if (tp->col > 0)
			tp->col--;
		break;
	case '\t':
		while (++tp->col % 8)
			continue;
		if (tp->col >= tp->ncol) {
			tp->col = 0;
			term_char(tp, '\n');
		}
		break;
	case '\n':
		if (tp->line == tp->nlin - 1)
			term_scroll(tp);
		else
			tp->line++;
		break;
	case '\r':
		tp->col = 0;
		break;
	default:
		if (c < ' ' || c > '~')
			c = '?';
		tp->vram[tp->line][tp->col++] = c;
		if (tp->col >= tp->ncol) {
			tp->col = 0;
			term_char(tp, '\n');
		}
	}
}

void
Term_Feed(struct term *tp, const char *b, const char *e)
{
	int i;

	while (b < e) {
		assert(tp->col < tp->ncol);
		assert(tp->line < tp->nlin);
		assert(tp->state <= 3);
		switch (tp->state) {
		case 0:
			if (*b == '\x1b')
				tp->state = 1;
			else if (*(const uint8_t*)b == 0x9b)
				tp->state = 2;
			else
				term_char(tp, *b);
			b++;
			break;
		case 1:
			if (*b++ != '[')
				vtc_fatal(tp->vl, "ANSI not '[' (0x%x)",
				    b[-1] & 0xff);
			tp->state = 2;
			break;
		case 2:
			tp->argp = tp->arg;
			for (i=0; i < NTERMARG; i++)
				tp->arg[i] = -1;
			tp->state = 3;
			if (*b == '?')
				b++;
			break;
		case 3:
			if (tp->argp - tp->arg >= NTERMARG)
				vtc_fatal(tp->vl, "ANSI too many args");

			if (isdigit(*b)) {
				if (*tp->argp == -1)
					*tp->argp = 0;
				*tp->argp *= 10;
				*tp->argp += *b++ - '0';
				continue;
			}
			if (*b == ';') {
				tp->argp++;
				tp->state = 3;
				b++;
				continue;
			}
			term_escape(tp, *b++, tp->argp -  tp->arg);
			tp->state = 0;
			break;
		default:
			WRONG("Wrong ansi state");
		}
	}
}

void
Term_SetSize(struct term *tp, int lin, int col)
{
	char **vram;
	int i, j;

	vram = calloc(lin, sizeof *tp->vram);
	AN(vram);
	for (i = 0; i < lin; i++) {
		vram[i] = malloc(col + 1L);
		AN(vram[i]);
	}
	term_clear(vram, lin, col);
	if (tp->vram != NULL) {
		for (i = 0; i < lin; i++) {
			if (i >= tp->nlin)
				break;
			j = col;
			if (j > tp->ncol)
				j = tp->ncol;
			memcpy(vram[i], tp->vram[i], j);
		}
		for (i = 0; i < tp->nlin; i++)
			free(tp->vram[i]);
		free(tp->vram);
	}
	tp->vram = vram;
	tp->nlin = lin;
	tp->ncol = col;
}

struct term *
Term_New(struct vtclog *vl, int lin, int col)
{
	struct term *tp;

	ALLOC_OBJ(tp, TERM_MAGIC);
	AN(tp);
	tp->vl = vl;
	Term_SetSize(tp, lin, col);
	tp->line = tp->nlin - 1;
	return (tp);
}


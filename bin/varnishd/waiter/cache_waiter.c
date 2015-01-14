/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
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
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#include "cache/cache.h"

#include "vfil.h"

#include "waiter/waiter.h"
#include "waiter/waiter_priv.h"

#define NEV 8192

const char *
WAIT_GetName(void)
{

	if (waiter != NULL)
		return (waiter->name);
	else
		return ("no_waiter");
}

struct waiter *
WAIT_Init(waiter_handle_f *func, volatile double *tmo)
{
	struct waiter *w;

	AN(waiter);
	AN(waiter->name);
	AN(waiter->init);

	w = calloc(1, sizeof (struct waiter) + waiter->size);
	AN(w);
	INIT_OBJ(w, WAITER_MAGIC);
	w->priv = (void*)(w + 1);
	w->impl = waiter;
	w->func = func;
	w->tmo = tmo;
	VTAILQ_INIT(&w->sesshead);

	waiter->init(w);
	AN(w->impl->pass || w->pfd > 0);
	return (w);
}

void
WAIT_UsePipe(struct waiter *w)
{
	CHECK_OBJ_NOTNULL(w, WAITER_MAGIC);

	AN(waiter->inject);
	AZ(pipe(w->pipes));
	AZ(VFIL_nonblocking(w->pipes[0]));
	AZ(VFIL_nonblocking(w->pipes[1]));
	w->pfd = w->pipes[1];
	ALLOC_OBJ(w->pipe_w, WAITED_MAGIC);
	w->pipe_w->fd = w->pipes[0];
	w->pipe_w->deadline = 9e99;
	VTAILQ_INSERT_HEAD(&w->sesshead, w->pipe_w, list);
	waiter->inject(w, w->pipe_w);
}

int
WAIT_Enter(const struct waiter *w, struct waited *wp)
{
	ssize_t written;

	CHECK_OBJ_NOTNULL(w, WAITER_MAGIC);
	CHECK_OBJ_NOTNULL(wp, WAITED_MAGIC);
	assert(wp->fd >= 0);

	if (w->impl->pass != NULL)
		return (w->impl->pass(w->priv, wp));
	assert(w->pfd >= 0);

	written = write(w->pfd, &wp, sizeof wp);
	if (written != sizeof wp && (errno == EAGAIN || errno == EWOULDBLOCK))
		return (-1);
	assert (written == sizeof wp);
	return (0);
}

void
WAIT_handle(struct waiter *w, struct waited *wp, enum wait_event ev, double now)
{
	struct waited *ss[NEV];
	int i, j;

	CHECK_OBJ_NOTNULL(w, WAITER_MAGIC);
	CHECK_OBJ_NOTNULL(wp, WAITED_MAGIC);

	if (wp == w->pipe_w) {
		i = read(w->pipes[0], ss, sizeof ss);
		if (i == -1 && errno == EAGAIN)
			return;
		for (j = 0; i >= sizeof ss[0]; j++, i -= sizeof ss[0]) {
			CHECK_OBJ_NOTNULL(ss[j], WAITED_MAGIC);
			assert(ss[j]->fd >= 0);
			VTAILQ_INSERT_TAIL(&w->sesshead, ss[j], list);
			w->impl->inject(w, ss[j]);
		}
		AZ(i);
		return;
	}

	VTAILQ_REMOVE(&w->sesshead, wp, list);
	w->func(wp, ev, now);
}

/*	$OpenBSD: io.c,v 1.49 2019/06/28 13:35:00 deraadt Exp $	*/

/*
 * Copyright (c) 1989, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
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

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <wctype.h>
#include <iconv.h>

#include "pathnames.h"
#include "calendar.h"


struct iovec header[] = {
	{ "From: ", 6 },
	{ NULL, 0 },
	{ " (Reminder Service)\nTo: ", 24 },
	{ NULL, 0 },
	{ "\nSubject: ", 10 },
	{ NULL, 0 },
	{ "'s Calendar\nPrecedence: bulk\n",  29 },
	{ "Auto-Submitted: auto-generated\n\n", 32 },
};

static size_t
utf8towcs (wchar_t *out, char *in, size_t n)
{
	char *ip = in, *op = (char *) out;
	size_t ibl = strlen(in), obl = n * sizeof(wchar_t);
	static iconv_t cd = (iconv_t) -1;
	int r;

	if (cd == (iconv_t) -1)
		if ((cd = iconv_open("WCHAR_T", "UTF-8")) == (iconv_t) -1) 
			err(1, NULL);

	r = iconv(cd, &ip, &ibl, &op, &obl);

	if ((r == -1) && (errno != EINVAL) && (errno != E2BIG))
		return -1;

	if (obl >= sizeof(wchar_t))
		*((wchar_t *) op) = L'\0';
	else
		*(((wchar_t *) op) - 1) = L'\0';
	return r;
}

void
cal(void)
{
	int ch, l, i, bodun = 0, bodun_maybe = 0, var, printing, nlen, utf8mode = 0;
	struct event *events, *cur_evt, *ev1, *tmp;
	wchar_t buf[2048 + 1], *prefix = NULL, *p;
	char filebuf[2048 + 1], *filep;
	struct match *m;
	FILE *fp;

	events = NULL;
	cur_evt = NULL;
	if ((fp = opencal()) == NULL)
		return;
	for (printing = 0; fgets(filebuf, sizeof(filebuf), stdin) != NULL;) {
		if ((filep = strchr(filebuf, '\n')) != NULL)
			*filep = '\0';
		else
			while ((ch = getchar()) != '\n' && ch != EOF);
		/* convert the line */
		if (utf8mode)
			i = utf8towcs(buf, filebuf, 2048);
		else
			i = mbstowcs(buf, filebuf, 2048);

		if (i == -1)
			continue;

		for (l = wcslen(buf); l > 0 && iswspace(buf[l - 1]); l--)
			;
		buf[l] = L'\0';
		if (buf[0] == L'\0')
			continue;

		if (wcsncmp(buf, L"# ", 2) == 0)
			continue;

		if (wcsncmp(buf, L"LANG=", 5) == 0) {
			if (strncasecmp(filebuf + 5, "utf-8", 5) == 0)
				utf8mode = 1;
			else {
				utf8mode = 0;
				if (setlocale(LC_ALL, filebuf + 5) == NULL)
					errx(1, "switch to locale ``%s'' cannot be honored", filebuf + 5);
			}
			setnnames();
			if (!strcmp(filebuf + 5, "ru_RU.UTF-8") ||
			    !strcmp(filebuf + 5, "uk_UA.UTF-8") ||
			    !strcmp(filebuf + 5, "by_BY.UTF-8")) {
				bodun_maybe++;
				bodun = 0;
				free(prefix);
				prefix = NULL;
			} else
				bodun_maybe = 0;
			continue;
		} else if (wcsncmp(buf, L"CALENDAR=", 9) == 0) {
			wchar_t *ep;

			if (buf[9] == L'\0')
				calendar = 0;
			else if (!wcscasecmp(buf + 9, L"julian")) {
				calendar = JULIAN;
				errno = 0;
				julian = wcstoul(buf + 14, &ep, 10);
				if (buf[0] == L'\0' || *ep != L'\0')
					julian = 13;
				if ((errno == ERANGE && julian == ULONG_MAX) ||
				    julian > 14)
					errx(1, "Julian calendar offset is too large");
			} else if (!wcscasecmp(buf + 9, L"gregorian"))
				calendar = GREGORIAN;
			else if (!wcscasecmp(buf + 9, L"lunar"))
				calendar = LUNAR;
		} else if (bodun_maybe && wcsncmp(buf, L"BODUN=", 6) == 0) {
			bodun++;
			free(prefix);
			if ((prefix = wcsdup(buf + 6)) == NULL)
				err(1, NULL);
			continue;
		}
		/* User defined names for special events */
		if ((p = wcschr(buf, L'='))) {
			for (i = 0; i < NUMEV; i++) {
				if (wcsncasecmp(buf, spev[i].name,
				    spev[i].nlen) == 0 &&
				    (p - buf == spev[i].nlen) &&
				    buf[spev[i].nlen + 1]) {
					p++;
					free(spev[i].uname);
					if ((spev[i].uname = wcsdup(p)) == NULL)
						err(1, NULL);
					spev[i].ulen = wcslen(p);
					i = NUMEV + 1;
				}
			}
			if (i > NUMEV)
				continue;
		}
		if (buf[0] != L'\t') {
			printing = (m = isnow(buf, bodun)) ? 1 : 0;
			if ((p = wcschr(buf, L'\t')) == NULL) {
				printing = 0;
				continue;
			}
			/* Need the following to catch hardwired "variable"
			 * dates */
			if (p > buf && p[-1] == L'*')
				var = 1;
			else
				var = 0;
			if (printing) {
				struct match *foo;

				ev1 = NULL;
				while (m) {
					cur_evt = malloc(sizeof(struct event));
					if (cur_evt == NULL)
						err(1, NULL);

					cur_evt->when = m->when;
					swprintf(cur_evt->print_date,
					    (sizeof(cur_evt->print_date)/sizeof(wchar_t)), L"%ls%lc",
					    m->print_date, (var + m->var) ? L'*' : L' ');
					if (ev1) {
						cur_evt->desc = ev1->desc;
						cur_evt->ldesc = NULL;
					} else {
						if (m->bodun && prefix) {
							nlen = wcslen(prefix) + wcslen(p);
							if ((cur_evt->ldesc = malloc(nlen * sizeof (wchar_t))) == NULL)
								err(1, NULL);
							swprintf(cur_evt->ldesc, nlen,
								L"\t%ls %ls", prefix,
								p + 1);
						} else if ((cur_evt->ldesc =
						    wcsdup(p)) == NULL)
							err(1, NULL);
						cur_evt->desc = &(cur_evt->ldesc);
						ev1 = cur_evt;
					}
					insert(&events, cur_evt);
					foo = m;
					m = m->next;
					free(foo);
				}
			}
		} else if (printing) {
			wchar_t *ldesc;

			nlen = wcslen(ev1->ldesc) + wcslen(buf) + 2;
			if ((ldesc = malloc(nlen * sizeof (wchar_t))) == NULL)
				err(1, NULL);
			swprintf(ldesc, nlen, L"%ls\n%ls", ev1->ldesc, buf);
			free(ev1->ldesc);
			ev1->ldesc = ldesc;
		}
	}
	tmp = events;
	while (tmp) {
		(void)fprintf(fp, "%ls%ls\n", tmp->print_date, *(tmp->desc));
		tmp = tmp->next;
	}
	tmp = events;
	while (tmp) {
		events = tmp;
		free(tmp->ldesc);
		tmp = tmp->next;
		free(events);
	}
	closecal(fp);
}

int
getfield(wchar_t *p, wchar_t **endp, int *flags)
{
	int val, var, i;
	wchar_t *start, savech;

	for (; !iswdigit(*p) && !iswalpha(*p) &&
	    *p != L'*' && *p != L'\t'; ++p)
		;
	if (*p == L'*') {			/* `*' is every month */
		*flags |= F_ISMONTH;
		*endp = p+1;
		return (-1);	/* means 'every month' */
	}
	if (iswdigit(*p)) {
		val = wcstol(p, &p, 10);	/* if 0, it's failure */
		for (; !iswdigit(*p) &&
		    !iswalpha(*p) && *p != L'*'; ++p)
			;
		*endp = p;
		return (val);
	}
	for (start = p; iswalpha(*++p);)
		;

	/* Sunday-1 */
	if (*p == L'+' || *p == L'-')
		for(; iswdigit(*++p); )
			;

	savech = *p;
	*p = '\0';

	/* Month */
	if ((val = getmonth(start)) != 0)
		*flags |= F_ISMONTH;

	/* Day */
	else if ((val = getday(start)) != 0) {
		*flags |= F_ISDAY;

		/* variable weekday */
		if ((var = getdayvar(start)) != 0) {
			if (var <= 5 && var >= -4)
				val += var * 10;
#ifdef DEBUG
			printf("var: %d\n", var);
#endif
		}
	}

	/* Try specials (Easter, Paskha, ...) */
	else {
		for (i = 0; i < NUMEV; i++) {
			if (wcsncasecmp(start, spev[i].name, spev[i].nlen) == 0) {
				start += spev[i].nlen;
				val = i + 1;
				i = NUMEV + 1;
			} else if (spev[i].uname != NULL &&
			    wcsncasecmp(start, spev[i].uname, spev[i].ulen) == 0) {
				start += spev[i].ulen;
				val = i + 1;
				i = NUMEV + 1;
			}
		}
		if (i > NUMEV) {
			const char *errstr;

			switch (*start) {
			case L'-':
			case L'+':
				var = wcstol(start + 1, (wchar_t **) NULL, 10);
				if (var > 365 || var < 0)
					return (0); /* Someone is just being silly */
				if (*start == '-')
					var = -var;
				val += (NUMEV + 1) * var;
				/* We add one to the matching event and multiply by
				 * (NUMEV + 1) so as not to return 0 if there's a match.
				 * val will overflow if there is an obscenely large
				 * number of special events. */
				break;
			}
			*flags |= F_SPECIAL;
		}
		if (!(*flags & F_SPECIAL)) {
			/* undefined rest */
			*p = savech;
			return (0);
		}
	}
	for (*p = savech; !iswdigit(*p) &&
	    !iswalpha(*p) && *p != L'*' && *p != L'\t'; ++p)
		;
	*endp = p;
	return (val);
}


FILE *
opencal(void)
{
	int pdes[2], fdin;
	struct stat st;

	/* open up calendar file as stdin */
	if ((fdin = open(calendarFile, O_RDONLY)) == -1 ||
	    fstat(fdin, &st) == -1 || !(S_ISREG(st.st_mode)||S_ISCHR(st.st_mode))) {
		if (!doall) {
			char *home = getenv("HOME");
			if (home == NULL || *home == '\0')
				errx(1, "cannot get home directory");
			if (!(chdir(home) == 0 &&
			    chdir(calendarHome) == 0 &&
			    (fdin = open(calendarFile, O_RDONLY)) != -1)) {
				/* Try the system-wide calendar file. */
				if ((fdin = open(_PATH_DEFAULT, O_RDONLY)) == -1) {
					errx(1, "no calendar file: \"%s\" or \"~/%s/%s\"",
					    calendarFile, calendarHome, calendarFile);
				}
			}
		}
	}

	if (pipe(pdes) == -1) {
		close(fdin);
		return (NULL);
	}
	switch (vfork()) {
	case -1:			/* error */
		(void)close(pdes[0]);
		(void)close(pdes[1]);
		close(fdin);
		return (NULL);
	case 0:
		dup2(fdin, STDIN_FILENO);
		/* child -- set stdout to pipe input */
		if (pdes[1] != STDOUT_FILENO) {
			(void)dup2(pdes[1], STDOUT_FILENO);
			(void)close(pdes[1]);
		}
		(void)close(pdes[0]);
		/*
		 * Set stderr to /dev/null.  Necessary so that cron does not
		 * wait for cpp to finish if it's running calendar -a.
		 */
		if (doall) {
			int fderr;
			fderr = open(_PATH_DEVNULL, O_WRONLY, 0);
			if (fderr == -1)
				_exit(0);
			(void)dup2(fderr, STDERR_FILENO);
			(void)close(fderr);
		}
		execl(_PATH_CPP, "cpp", "-traditional", "-undef", "-U__GNUC__",
		    "-P", "-I.", _PATH_EINCLUDE, _PATH_INCLUDE, (char *)NULL);
		warn(_PATH_CPP);
		_exit(1);
	}
	/* parent -- set stdin to pipe output */
	(void)dup2(pdes[0], STDIN_FILENO);
	(void)close(pdes[0]);
	(void)close(pdes[1]);

	/* not reading all calendar files, just set output to stdout */
	if (!doall)
		return (stdout);

	/* set output to a temporary file, so if no output don't send mail */
	return(tmpfile());
}

void
closecal(FILE *fp)
{
	struct stat sbuf;
	int nread, pdes[2], status;
	char buf[1024];
	pid_t pid = -1;

	if (!doall)
		return;

	(void)rewind(fp);
	if (fstat(fileno(fp), &sbuf) || !sbuf.st_size)
		goto done;
	if (pipe(pdes) == -1)
		goto done;
	switch ((pid = vfork())) {
	case -1:			/* error */
		(void)close(pdes[0]);
		(void)close(pdes[1]);
		goto done;
	case 0:
		/* child -- set stdin to pipe output */
		if (pdes[0] != STDIN_FILENO) {
			(void)dup2(pdes[0], STDIN_FILENO);
			(void)close(pdes[0]);
		}
		(void)close(pdes[1]);
		execl(_PATH_SENDMAIL, "sendmail", "-i", "-t", "-F",
		    "\"Reminder Service\"", (char *)NULL);
		warn(_PATH_SENDMAIL);
		_exit(1);
	}
	/* parent -- write to pipe input */
	(void)close(pdes[0]);

	header[1].iov_base = header[3].iov_base = pw->pw_name;
	header[1].iov_len = header[3].iov_len = strlen(pw->pw_name);
	writev(pdes[1], header, 8);
	while ((nread = read(fileno(fp), buf, sizeof(buf))) > 0)
		(void)write(pdes[1], buf, nread);
	(void)close(pdes[1]);
done:	(void)fclose(fp);
	if (pid != -1) {
		while (waitpid(pid, &status, 0) == -1) {
			if (errno != EINTR)
				break;
		}
	}
}


void
insert(struct event **head, struct event *cur_evt)
{
	struct event *tmp, *tmp2;

	if (*head) {
		/* Insert this one in order */
		tmp = *head;
		tmp2 = NULL;
		while (tmp->next &&
		    tmp->when <= cur_evt->when) {
			tmp2 = tmp;
			tmp = tmp->next;
		}
		if (tmp->when > cur_evt->when) {
			cur_evt->next = tmp;
			if (tmp2)
				tmp2->next = cur_evt;
			else
				*head = cur_evt;
		} else {
			cur_evt->next = tmp->next;
			tmp->next = cur_evt;
		}
	} else {
		*head = cur_evt;
		cur_evt->next = NULL;
	}
}

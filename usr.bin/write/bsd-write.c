/*
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Jef Poskanzer and Craig Leres of the Lawrence Berkeley Laboratory.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
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

#ifndef lint
static const char copyright[] =
"@(#) Copyright (c) 1989, 1993\n\
	The Regents of the University of California.  All rights reserved.\n";
#endif

#include <sys/param.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <ctype.h>
#include <err.h>
#include <locale.h>
#include <paths.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utmp.h>

#define DEBIAN
#ifdef DEBIAN
#  include <errno.h>
#  define HAVE_GETUTENT
/* for Hurd */
#  ifndef MAXPATHLEN
#    define MAXPATHLEN 4096
#  endif
#  ifndef MAXHOSTNAMELEN
#    define MAXHOSTNAMELEN 255
#  endif
#  ifdef __GLIBC__
#    undef __unused
#    define __unused __attribute__((__unused__))
#  endif
#endif

void done(int);
void do_write(char *, char *, uid_t);
static void usage(void);
int term_chk(char *, int *, time_t *, int);
void wr_fputs(unsigned char *s);
void search_utmp(char *, char *, char *, uid_t);
int utmp_chk(char *, char *);

int
main(int argc, char **argv)
{
	char *cp;
	time_t atime;
	uid_t myuid;
	int msgsok, myttyfd;
	char tty[MAXPATHLEN], *mytty;

	mytty = NULL;
	(void)setlocale(LC_CTYPE, "");

	while (getopt(argc, argv, "") != -1)
		usage();
	argc -= optind;
	argv += optind;

	/* check that sender has write enabled */
	if (isatty(fileno(stdin)))
		myttyfd = fileno(stdin);
	else if (isatty(fileno(stdout)))
		myttyfd = fileno(stdout);
	else if (isatty(fileno(stderr)))
		myttyfd = fileno(stderr);
	else
		mytty = "(none)";
	if (!mytty) {
		if (!(mytty = ttyname(myttyfd)))
			errx(1, "can't find your tty's name");
		if (!strncmp(mytty, "/dev/", 5))
			mytty += 5;
		if (term_chk(mytty, &msgsok, &atime, 1))
			exit(1);
	} else
		msgsok = 1;

	myuid = getuid();

	if (!msgsok) {
		warnx("write: you have write permission turned off.\n");
		if (myuid != 0)
			exit(1);
	}

	/* check args */
	switch (argc) {
	case 1:
		search_utmp(argv[0], tty, mytty, myuid);
		do_write(tty, mytty, myuid);
		break;
	case 2:
		if (!strncmp(argv[1], _PATH_DEV, strlen(_PATH_DEV)))
			argv[1] += strlen(_PATH_DEV);
		if (utmp_chk(argv[0], argv[1]))
			errx(1, "%s is not logged in on %s", argv[0], argv[1]);
		if (term_chk(argv[1], &msgsok, &atime, 1))
			exit(1);
		if (myuid && !msgsok)
			errx(1, "%s has messages disabled on %s", argv[0], argv[1]);
		do_write(argv[1], mytty, myuid);
		break;
	default:
		usage();
	}
	done(0);
	return (0);
}

static void
usage(void)
{
	(void)fprintf(stderr, "usage: write user [tty]\n");
	exit(1);
}

/*
 * utmp_chk - checks that the given user is actually logged in on
 *     the given tty
 */
int
utmp_chk(char *user, char *tty)
{
#ifdef HAVE_GETUTENT
	struct utmp *u;

	while ((u = getutent()) != (struct utmp*) 0)
		if (u->ut_type == USER_PROCESS &&
		    strncmp(user, u->ut_name, sizeof(u->ut_name)) == 0 &&
		    strncmp(tty, u->ut_line, sizeof(u->ut_line)) == 0)
			return(0);
#else
	struct utmp u;
	int ufd;

	if ((ufd = open(_PATH_UTMP, O_RDONLY)) < 0)
		return(0);	/* ignore error, shouldn't happen anyway */

	while (read(ufd, (char *) &u, sizeof(u)) == sizeof(u))
		if (strncmp(user, u.ut_name, sizeof(u.ut_name)) == 0 &&
		    u.ut_type == USER_PROCESS &&
		    strncmp(tty, u.ut_line, sizeof(u.ut_line)) == 0) {
			(void)close(ufd);
			return(0);
		}

	(void)close(ufd);
#endif
	return(1);
}

/*
 * search_utmp - search utmp for the "best" terminal to write to
 *
 * Ignores terminals with messages disabled, and of the rest, returns
 * the one with the most recent access time.  Returns as value the number
 * of the user's terminals with messages enabled, or -1 if the user is
 * not logged in at all.
 *
 * Special case for writing to yourself - ignore the terminal you're
 * writing from, unless that's the only terminal with messages enabled.
 */
void
search_utmp(char *user, char *tty, char *mytty, uid_t myuid)
{
#ifdef HAVE_GETUTENT
	struct utmp *u;
#else
	struct utmp u;
#endif
	time_t bestatime, atime;
	int ufd, nloggedttys, nttys, msgsok, user_is_me;
	char atty[UT_LINESIZE + 1];

#ifdef HAVE_GETUTENT
	setutent();
#else
	if ((ufd = open(_PATH_UTMP, O_RDONLY)) < 0)
		err(1, "utmp");
#endif

	nloggedttys = nttys = 0;
	bestatime = 0;
	user_is_me = 0;
#ifdef HAVE_GETUTENT
	while ((u = getutent()) != (struct utmp*) 0)
		if (strncmp(user, u->ut_name, sizeof(u->ut_name)) == 0
		    && u->ut_type == USER_PROCESS) {
			++nloggedttys;
			(void)strncpy(atty, u->ut_line, UT_LINESIZE);
#else
	while (read(ufd, (char *) &u, sizeof(u)) == sizeof(u))
		if (strncmp(user, u.ut_name, sizeof(u.ut_name)) == 0) {
			++nloggedttys;
			(void)strncpy(atty, u.ut_line, UT_LINESIZE);
#endif
			atty[UT_LINESIZE] = '\0';
			if (term_chk(atty, &msgsok, &atime, 0))
				continue;	/* bad term? skip */
			if (myuid && !msgsok)
				continue;	/* skip ttys with msgs off */
			if (strcmp(atty, mytty) == 0) {
				user_is_me = 1;
				continue;	/* don't write to yourself */
			}
			++nttys;
#ifdef HAVE_GETUTENT
/*
 * If this is a newly created tty (eg fresh xterm), then user will not have
 * typed at it, and the atime will be way in the past.
 * So, if the atime is < utmp creation time, use the utmp time.
 */
			if (atime < u->ut_time)
				atime = u->ut_time;
#endif
			if (atime > bestatime) {
				bestatime = atime;
				(void)strcpy(tty, atty);
			}
		}

#ifdef HAVE_GETUTENT
	endutent();
#else
	(void)close(ufd);
#endif
	if (nloggedttys == 0)
		errx(1, "%s is not logged in", user);
	if (nttys == 0) {
		if (user_is_me) {		/* ok, so write to yourself! */
			(void)strcpy(tty, mytty);
			return;
		}
		errx(1, "%s has messages disabled", user);
	} else if (nttys > 1) {
		warnx("%s is logged in more than once; writing to %s", user, tty);
	}
}

/* fullname - take relative path names as being relative to /dev/ */
void
fullname(buf, s)
char *buf, *s;
{
	strcpy(buf, _PATH_DEV);
	if (s[0] != '/')
		buf += strlen(_PATH_DEV);
	strcpy(buf, s);
}

/*
 * term_chk - check that a terminal exists, and get the message bit
 *     and the access time
 */
int
term_chk(char *tty, int *msgsokP, time_t *atimeP, int showerror)
{
	struct stat s;
	char path[MAXPATHLEN];

	fullname(path, tty);
	if (stat(path, &s) < 0) {
		if (showerror)
			warn("%s", path);
		return(1);
	}
	*msgsokP = (s.st_mode & (S_IWRITE >> 3)) != 0;	/* group write bit */
	*atimeP = s.st_atime;
	return(0);
}

/*
 * do_write - actually make the connection
 */
void
do_write(char *tty, char *mytty, uid_t myuid)
{
	const char *login;
	char *nows;
	struct passwd *pwd;
	time_t now;
	char path[MAXPATHLEN], host[MAXHOSTNAMELEN], line[512];

	/* Determine our login name before the we reopen() stdout */
	if ((login = getlogin()) == NULL) {
		if ((pwd = getpwuid(myuid)))
			login = pwd->pw_name;
		else
			errx(1, "who are you?"); /* uh-oh, no passwd entry! */
	}

	/* got the utmp entry: ensure it matches the uid */
	if ((pwd = getpwnam(login)) == NULL)
		/* claimed utmp entry isn't in the password file */
		errx(1, "you're %s, eh? Never heard of you.", login);
	if (myuid != pwd->pw_uid) {
		/* uid of logname in utmp is different from our uid,
		   eg su'ed in someone else's login */
		if (myuid != 0)
			errx(1, "you are uid %d, but your login is as uid %d",
			    myuid, pwd->pw_uid);
		else
		    warnx("warning: write will appear from %s", login);
	}

	fullname(path, tty);
	if ((freopen(path, "w", stdout)) == NULL)
		err(1, "%s", path);

	(void)signal(SIGINT, done);
	(void)signal(SIGHUP, done);

	/* print greeting */
	if (gethostname(host, sizeof(host)) < 0)
		(void)strcpy(host, "???");
	now = time((time_t *)NULL);
	nows = ctime(&now);
	nows[16] = '\0';
	(void)printf("\r\n\007\007\007Message from %s@%s on %s at %s ...\r\n",
	    login, host, mytty, nows + 11);

	while (fgets(line, sizeof(line), stdin) != NULL)
		wr_fputs(line);
}

/*
 * done - cleanup and exit
 */
void
done(int n __unused)
{
	(void)printf("EOF\r\n");
	exit(0);
}

/*
 * wr_fputs - like fputs(), but makes control characters visible and
 *     turns \n into \r\n
 */
void
wr_fputs(unsigned char *s)
{

	static int binary_chars = 0;

#define	PUTC(c)	if (putchar(c) == EOF) goto error;

	for (; *s != '\0'; ++s) {
		if (*s == '\n') {
			PUTC('\r');
			PUTC('\n');
		} else if (iscntrl(*s) && *s != '\007') {
			PUTC('^');
			PUTC(*s ^ 0x40); /* DEL to ?, others to alpha */
			if (binary_chars++ > 256)
			    errx(1, "Too many binary characters; aborting...");
		} else
		PUTC(*s);
	}
	return;
error:	if (errno == EIO)
	    errx(1, "Input/output error: maybe the remote tty disappeared");
	else
	    err(1, NULL);
#undef PUTC
}

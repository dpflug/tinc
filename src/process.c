/*
    process.c -- process management functions
    Copyright (C) 1999-2003 Ivo Timmermans <ivo@o2w.nl>,
                  2000-2003 Guus Sliepen <guus@sliepen.eu.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    $Id: process.c,v 1.1.2.56 2003/07/21 14:47:43 guus Exp $
*/

#include "system.h"

#include "conf.h"
#include "connection.h"
#include "device.h"
#include "edge.h"
#include "logger.h"
#include "node.h"
#include "pidfile.h"
#include "process.h"
#include "subnet.h"
#include "utils.h"
#include "xalloc.h"

/* If zero, don't detach from the terminal. */
int do_detach = 1;

extern char *identname;
extern char *pidfilename;
extern char **g_argv;
extern int use_logfile;

sigset_t emptysigset;

static int saved_debug_level = -1;

extern int sighup;
extern int sigalrm;
extern int do_purge;

static void memory_full(int size)
{
	logger(LOG_ERR, _("Memory exhausted (couldn't allocate %d bytes), exitting."), size);
	cp_trace();
	exit(1);
}

/* Some functions the less gifted operating systems might lack... */

#ifndef HAVE_FCLOSEALL
static int fcloseall(void)
{
	fflush(stdin);
	fflush(stdout);
	fflush(stderr);
	fclose(stdin);
	fclose(stdout);
	fclose(stderr);
	return 0;
}
#endif

/*
  Close network connections, and terminate neatly
*/
void cleanup_and_exit(int c)
{
	cp();

	close_network_connections();

	ifdebug(CONNECTIONS)
		dump_device_stats();

	logger(LOG_NOTICE, _("Terminating"));

	closelogger();
	exit(c);
}

/*
  check for an existing tinc for this net, and write pid to pidfile
*/
static int write_pidfile(void)
{
	int pid;

	cp();

	pid = check_pid(pidfilename);

	if(pid) {
		if(netname)
			fprintf(stderr, _("A tincd is already running for net `%s' with pid %d.\n"),
					netname, pid);
		else
			fprintf(stderr, _("A tincd is already running with pid %d.\n"), pid);
		return 1;
	}

	/* if it's locked, write-protected, or whatever */
	if(!write_pid(pidfilename))
		return 1;

	return 0;
}

/*
  kill older tincd for this net
*/
int kill_other(int signal)
{
	int pid;

	cp();

	pid = read_pid(pidfilename);

	if(!pid) {
		if(netname)
			fprintf(stderr, _("No other tincd is running for net `%s'.\n"),
					netname);
		else
			fprintf(stderr, _("No other tincd is running.\n"));
		return 1;
	}

	errno = 0;					/* No error, sometimes errno is only changed on error */

	/* ESRCH is returned when no process with that pid is found */
	if(kill(pid, signal) && errno == ESRCH) {
		if(netname)
			fprintf(stderr, _("The tincd for net `%s' is no longer running. "),
					netname);
		else
			fprintf(stderr, _("The tincd is no longer running. "));

		fprintf(stderr, _("Removing stale lock file.\n"));
		remove_pid(pidfilename);
	}

	return 0;
}

/*
  Detach from current terminal, write pidfile, kill parent
*/
int detach(void)
{
	cp();

	setup_signals();

	/* First check if we can open a fresh new pidfile */

	if(write_pidfile())
		return -1;

	/* If we succeeded in doing that, detach */

	closelogger();

	if(do_detach) {
		if(daemon(0, 0) < 0) {
			fprintf(stderr, _("Couldn't detach from terminal: %s"),
					strerror(errno));
			return -1;
		}

		/* Now UPDATE the pid in the pidfile, because we changed it... */

		if(!write_pid(pidfilename))
			return -1;
	}

	openlogger(identname, use_logfile?LOGMODE_FILE:(do_detach?LOGMODE_SYSLOG:LOGMODE_STDERR));

	logger(LOG_NOTICE, _("tincd %s (%s %s) starting, debug level %d"),
			   VERSION, __DATE__, __TIME__, debug_level);

	xalloc_fail_func = memory_full;

	return 0;
}

/*
  Execute the program name, with sane environment.
*/
static void _execute_script(const char *scriptname, char **envp)
	__attribute__ ((noreturn));
static void _execute_script(const char *scriptname, char **envp)
{
	int save_errno;

	cp();

	while(*envp)
		putenv(*envp++);

	chdir("/");

	closelogger();

	/* Close all file descriptors */
	fcloseall();

	execl(scriptname, NULL);
	/* No return on success */

	save_errno = errno;

	openlogger(identname, use_logfile?LOGMODE_FILE:(do_detach?LOGMODE_SYSLOG:LOGMODE_STDERR));
	logger(LOG_ERR, _("Could not execute `%s': %s"), scriptname,
		   strerror(save_errno));
	exit(save_errno);
}

/*
  Fork and execute the program pointed to by name.
*/
int execute_script(const char *name, char **envp)
{
	pid_t pid;
	int status;
	struct stat s;
	char *scriptname;

	cp();

	asprintf(&scriptname, "%s/%s", confbase, name);

	/* First check if there is a script */

	if(stat(scriptname, &s))
		return 0;

	pid = fork();

	if(pid < 0) {
		logger(LOG_ERR, _("System call `%s' failed: %s"), "fork",
			   strerror(errno));
		return -1;
	}

	if(pid) {
		ifdebug(STATUS) logger(LOG_INFO, _("Executing script %s"), name);

		free(scriptname);

		if(waitpid(pid, &status, 0) == pid) {
			if(WIFEXITED(status)) {	/* Child exited by itself */
				if(WEXITSTATUS(status)) {
					logger(LOG_ERR, _("Process %d (%s) exited with non-zero status %d"),
						   pid, name, WEXITSTATUS(status));
					return -1;
				} else
					return 0;
			} else if(WIFSIGNALED(status)) {	/* Child was killed by a signal */
				logger(LOG_ERR, _("Process %d (%s) was killed by signal %d (%s)"), pid,
					   name, WTERMSIG(status), strsignal(WTERMSIG(status)));
				return -1;
			} else {			/* Something strange happened */
				logger(LOG_ERR, _("Process %d (%s) terminated abnormally"), pid,
					   name);
				return -1;
			}
		} else if (errno != EINTR) {
			logger(LOG_ERR, _("System call `%s' failed: %s"), "waitpid",
				   strerror(errno));
			return -1;
		}

		/* Why do we get EINTR? */
		return 0;
	}

	/* Child here */

	_execute_script(scriptname, envp);
}


/*
  Signal handlers.
*/

static RETSIGTYPE sigterm_handler(int a)
{
	logger(LOG_NOTICE, _("Got TERM signal"));

	cleanup_and_exit(0);
}

static RETSIGTYPE sigquit_handler(int a)
{
	logger(LOG_NOTICE, _("Got QUIT signal"));
	cleanup_and_exit(0);
}

static RETSIGTYPE fatal_signal_square(int a)
{
	logger(LOG_ERR, _("Got another fatal signal %d (%s): not restarting."), a,
		   strsignal(a));
	cp_trace();
	exit(1);
}

static RETSIGTYPE fatal_signal_handler(int a)
{
	struct sigaction act;
	logger(LOG_ERR, _("Got fatal signal %d (%s)"), a, strsignal(a));
	cp_trace();

	if(do_detach) {
		logger(LOG_NOTICE, _("Trying to re-execute in 5 seconds..."));

		act.sa_handler = fatal_signal_square;
		act.sa_mask = emptysigset;
		act.sa_flags = 0;
		sigaction(SIGSEGV, &act, NULL);

		close_network_connections();
		sleep(5);
		remove_pid(pidfilename);
		execvp(g_argv[0], g_argv);
	} else {
		logger(LOG_NOTICE, _("Not restarting."));
		exit(1);
	}
}

static RETSIGTYPE sighup_handler(int a)
{
	logger(LOG_NOTICE, _("Got HUP signal"));
	sighup = 1;
}

static RETSIGTYPE sigint_handler(int a)
{
	if(saved_debug_level != -1) {
		logger(LOG_NOTICE, _("Reverting to old debug level (%d)"),
			saved_debug_level);
		debug_level = saved_debug_level;
		saved_debug_level = -1;
	} else {
		logger(LOG_NOTICE,
			_("Temporarily setting debug level to 5.  Kill me with SIGINT again to go back to level %d."),
			debug_level);
		saved_debug_level = debug_level;
		debug_level = 5;
	}
}

static RETSIGTYPE sigalrm_handler(int a)
{
	logger(LOG_NOTICE, _("Got ALRM signal"));
	sigalrm = 1;
}

static RETSIGTYPE sigusr1_handler(int a)
{
	dump_connections();
}

static RETSIGTYPE sigusr2_handler(int a)
{
	dump_device_stats();
	dump_nodes();
	dump_edges();
	dump_subnets();
}

static RETSIGTYPE sigwinch_handler(int a)
{
	extern int do_purge;
	do_purge = 1;
}

static RETSIGTYPE unexpected_signal_handler(int a)
{
	logger(LOG_WARNING, _("Got unexpected signal %d (%s)"), a, strsignal(a));
	cp_trace();
}

static RETSIGTYPE ignore_signal_handler(int a)
{
	ifdebug(SCARY_THINGS) logger(LOG_DEBUG, _("Ignored signal %d (%s)"), a, strsignal(a));
}

static struct {
	int signal;
	void (*handler)(int);
} sighandlers[] = {
	{
	SIGHUP, sighup_handler}, {
	SIGTERM, sigterm_handler}, {
	SIGQUIT, sigquit_handler}, {
	SIGSEGV, fatal_signal_handler}, {
	SIGBUS, fatal_signal_handler}, {
	SIGILL, fatal_signal_handler}, {
	SIGPIPE, ignore_signal_handler}, {
	SIGINT, sigint_handler}, {
	SIGUSR1, sigusr1_handler}, {
	SIGUSR2, sigusr2_handler}, {
	SIGCHLD, ignore_signal_handler}, {
	SIGALRM, sigalrm_handler}, {
	SIGWINCH, sigwinch_handler}, {
	0, NULL}
};

void setup_signals(void)
{
	int i;
	struct sigaction act;

	sigemptyset(&emptysigset);
	act.sa_handler = NULL;
	act.sa_mask = emptysigset;
	act.sa_flags = 0;

	/* Set a default signal handler for every signal, errors will be
	   ignored. */
	for(i = 0; i < NSIG; i++) {
		if(!do_detach)
			act.sa_handler = SIG_DFL;
		else
			act.sa_handler = unexpected_signal_handler;
		sigaction(i, &act, NULL);
	}

	/* If we didn't detach, allow coredumps */
	if(!do_detach)
		sighandlers[3].handler = SIG_DFL;

	/* Then, for each known signal that we want to catch, assign a
	   handler to the signal, with error checking this time. */
	for(i = 0; sighandlers[i].signal; i++) {
		act.sa_handler = sighandlers[i].handler;
		if(sigaction(sighandlers[i].signal, &act, NULL) < 0)
			fprintf(stderr, _("Installing signal handler for signal %d (%s) failed: %s\n"),
					sighandlers[i].signal, strsignal(sighandlers[i].signal),
					strerror(errno));
	}
}

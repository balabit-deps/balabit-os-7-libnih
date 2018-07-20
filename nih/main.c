/* libnih
 *
 * main.c - main loop handling and functions often called from main()
 *
 * Copyright © 2009 Scott James Remnant <scott@netsplit.com>.
 * Copyright © 2009 Canonical Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif /* HAVE_CONFIG_H */


#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>

#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/string.h>
#include <nih/list.h>
#include <nih/timer.h>
#include <nih/signal.h>
#include <nih/child.h>
#include <nih/io.h>
#include <nih/error.h>
#include <nih/logging.h>

#include "main.h"


/**
 * VAR_RUN:
 *
 * Directory to write pid files into.
 **/
#define VAR_RUN "/var/run"

/**
 * DEV_NULL:
 *
 * Device bound to stdin/out/err when daemonising.
 **/
#define DEV_NULL "/dev/null"


/**
 * program_name:
 *
 * The name of the program, taken from the argument array with the directory
 * name portion stripped.
 **/
const char *program_name = NULL;

/**
 * package_name:
 *
 * The name of the overall package, usually set to the autoconf PACKAGE_NAME
 * macro.  This should be used in preference.
 **/
const char *package_name = NULL;

/**
 * package_version:
 *
 * The version of the overall package, thus also the version of the program.
 * Usually set to the autoconf PACKAGE_VERSION macro.  This should be used
 * in preference.
 **/
const char *package_version = NULL;

/**
 * package_copyright:
 *
 * The copyright message for the package, taken from the autoconf
 * AC_COPYRIGHT macro.
 **/
const char *package_copyright = NULL;

/**
 * package_bugreport:
 *
 * The e-mail address to report bugs on the package to, taken from the
 * autoconf PACKAGE_BUGREPORT macro.
 **/
const char *package_bugreport = NULL;


/**
 * package_string:
 *
 * The human string for the program, either "program (version)" or if the
 * program and package names differ, "program (package version)".
 * Generated by nih_main_init_full().
 **/
const char *package_string = NULL;


/**
 * pid_file:
 *
 * Location of the pid file, or NULL if a reasonable default is to be
 * assumed.  Can be set using nih_main_set_pidfile(), and is then used by
 * nih_main_read_pidfile(), nih_main_write_pidfile() and
 * nih_main_unlink_pidfile().
 **/
static char *pid_file = NULL;


/**
 * interrupt_pipe:
 *
 * Pipe used for interrupting an active select() call in case a signal
 * comes in between the last time we handled the signal and the time we
 * ran the call.
 **/
static int interrupt_pipe[2] = { -1, -1 };

/**
 * exit_loop:
 *
 * Whether to exit the running main loop, set to TRUE by a call to
 * nih_main_loop_exit().
 **/
static __thread int exit_loop = 0;

/**
 * exit_status:
 *
 * Status to exit the running main loop with, set by nih_main_loop_exit().
 **/
static __thread int exit_status = 0;

/**
 * nih_main_loop_functions:
 *
 * List of functions to be called in each main loop iteration.  Each item
 * is an NihMainLoopFunc structure.
 **/
NihList *nih_main_loop_functions = NULL;


/**
 * nih_main_init_full:
 * @argv0: program name from arguments,
 * @package: package name from configure,
 * @version: package version from configure,
 * @bugreport: bug report address from configure,
 * @copyright: package copyright message from configure.
 *
 * Should be called at the beginning of main() to initialise the various
 * global variables exported from this module.  For autoconf-using packages
 * call the nih_main_init() macro instead.
 **/
void
nih_main_init_full (const char *argv0,
		    const char *package,
		    const char *version,
		    const char *bugreport,
		    const char *copyright)
{
	nih_assert (argv0 != NULL);
	nih_assert (package != NULL);
	nih_assert (version != NULL);

	/* Only take the basename of argv0, and allow it to be a login
	 * shell.
	 */
	program_name = strrchr (argv0, '/');
	if (program_name) {
		program_name++;
	} else if (argv0[0] == '-') {
		program_name = argv0 + 1;
	} else {
		program_name = argv0;
	}

	package_name = package;
	package_version = version;

	/* bugreport and copyright may be NULL/empty */
	if (bugreport && *bugreport)
		package_bugreport = bugreport;
	if (copyright && *copyright)
		package_copyright = copyright;

	if (package_string)
		nih_discard ((char *)package_string);

	if (strcmp (program_name, package_name)) {
		package_string = NIH_MUST (nih_sprintf (NULL, "%s (%s %s)",
							program_name,
							package_name,
							package_version));
	} else {
		package_string = NIH_MUST (nih_sprintf (NULL, "%s %s",
							package_name,
							package_version));
	}
}


/**
 * nih_main_suggest_help:
 *
 * Print a message suggesting --help to stderr.
 **/
void
nih_main_suggest_help (void)
{
	nih_assert (program_name != NULL);

	fprintf (stderr, _("Try `%s --help' for more information.\n"),
		 program_name);
}

/**
 * nih_main_version:
 *
 * Print the program version to stdout.
 **/
void
nih_main_version (void)
{
	nih_local char *str;

	nih_assert (program_name != NULL);

	printf ("%s\n", package_string);
	if (package_copyright)
		printf ("%s\n", package_copyright);
	printf ("\n");

	str = NIH_MUST (nih_str_screen_wrap (
			  NULL, _("This is free software; see the source for "
				  "copying conditions.  There is NO warranty; "
				  "not even for MERCHANTABILITY or FITNESS "
				  "FOR A PARTICULAR PURPOSE."), 0, 0));
	printf ("%s\n", str);
}


/**
 * nih_main_daemonise:
 *
 * Perform the necessary steps to become a daemon process, this will only
 * return in the child process if successful.  A file will be written to
 * /var/run/<program_name>.pid containing the pid of the child process.
 *
 * This is preferable to the libc daemon() function because it ensures
 * that the new process is not a session leader, so can open ttys without
 * worrying about them becoming its controlling terminal.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
nih_main_daemonise (void)
{
	pid_t pid;
	int   i, fd;

	nih_assert (program_name != NULL);

	/* Fork off child process.  This begans the detachment from our
	 * parent process; this will terminate the intermediate process.
	 */
	pid = fork ();
	if (pid < 0) {
		nih_return_system_error (-1);
	} else if (pid > 0) {
		exit (0);
	}

	/* Become session leader of a new process group, without any
	 * controlling tty.
	 */
	setsid ();

	/* When the session leader dies, SIGHUP is sent to all processes
	 * in that process group, including the child we're about to
	 * spawn.  So make damned sure it's ignored.
	 */
	nih_signal_set_ignore (SIGHUP);

	/* We now spawn off a second child (or at least attempt to),
	 * we do this so that it is guaranteed not to be a session leader,
	 * even by accident.  Therefore any open() call on a tty won't
	 * make that it's controlling terminal.
	 */
	pid = fork ();
	if (pid < 0) {
		nih_return_system_error (-1);
	} else if (pid > 0) {
		if (nih_main_write_pidfile (pid) < 0) {
			NihError *err;

			err = nih_error_get ();
			nih_warn ("%s: %s", _("Unable to write pid file"),
				  err->message);
			nih_free (err);
		}

		exit (0);
	}

	/* We're now in a daemon child process.  Change our working directory
	 * and file creation mask to be more appropriate.
	 */
	if (chdir ("/"))
		;
	umask (0);

	/* Close the stdin/stdout/stderr that we inherited */
	for (i = 0; i < 3; i++)
		close (i);

	/* And instead bind /dev/null to them */
	fd = open (DEV_NULL, O_RDWR);
	if (fd >= 0) {
		while (dup (fd) < 0)
			;
		while (dup (fd) < 0)
			;
	}

	return 0;
}


/**
 * nih_main_set_pidfile:
 * @filename: filename to be set.
 *
 * Set the location of the process's pid file or NULL to return it to the
 * default location under /var/run.  @filename must be an absolute path.
 **/
void
nih_main_set_pidfile (const char *filename)
{
	if (pid_file)
		nih_discard (pid_file);

	pid_file = NULL;

	if (filename) {
		nih_assert (filename[0] == '/');
		pid_file = NIH_MUST (nih_strdup (NULL, filename));
	}
}

/**
 * nih_main_get_pidfile:
 *
 * Returns the location of the process's pid file, which may be overridden
 * by nih_main_set_pidfile().  This is guaranteed to be an absolute path.
 *
 * Returns: internal copy of the string.
 **/
const char *
nih_main_get_pidfile (void)
{
	nih_assert (program_name != NULL);

	if (! pid_file)
		pid_file = NIH_MUST (nih_sprintf (NULL, "%s/%s.pid",
						  VAR_RUN, program_name));

	return pid_file;
}

/**
 * nih_main_read_pidfile:
 *
 * Reads the pid from the process'd pid file location, which can be set
 * with nih_main_get_pidfile().
 *
 * Returns: pid read or negative value on no available pid.
 **/
pid_t
nih_main_read_pidfile (void)
{
	FILE       *pidfile;
	const char *filename;
	pid_t       pid;

	filename = nih_main_get_pidfile ();

	pidfile = fopen (filename, "r");
	if (pidfile) {
		if (fscanf (pidfile, "%d", &pid) != 1)
			pid = -1;

		fclose (pidfile);
	} else {
		pid = -1;
	}

	return pid;
}

/**
 * nih_main_write_pidfile:
 * @pid: pid to be written.
 *
 * Writes the given @pid to the process's pid file location, which can be
 * set with nih_main_set_pidfile().
 *
 * The write is performed in such a way that at the point the file exists,
 * the pid can be read from it.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
nih_main_write_pidfile (pid_t pid)
{
	FILE           *pidfile;
	const char     *filename, *ptr;
	nih_local char *tmpname;
	mode_t          oldmask;
	int             ret = -1;

	nih_assert (pid > 0);

	/* Write to a hidden temporary file alongside the pid file.
	 * The return value is guaranteed to be an absolute path.
	 */
	filename = nih_main_get_pidfile ();
	ptr = strrchr (filename, '/');
	tmpname = NIH_MUST (nih_sprintf (NULL, "%.*s/.%s.tmp",
					 (int)(ptr - filename),
					 filename, ptr + 1));

	oldmask = umask (022);

	/* Write the pid file as atomically as we can */
	pidfile = fopen (tmpname, "w");
	if (! pidfile) {
		nih_error_raise_system ();
		goto error;
	}

	if ((fprintf (pidfile, "%d\n", pid) <= 0)
	    || (fflush (pidfile) < 0)
	    || (fsync (fileno (pidfile)) < 0)
	    || (fclose (pidfile) < 0)) {
		nih_error_raise_system ();
		fclose (pidfile);
		unlink (tmpname);
		goto error;
	}

	if (rename (tmpname, filename) < 0) {
		nih_error_raise_system ();
		unlink (tmpname);
		goto error;
	}

	ret = 0;
error:
	umask (oldmask);

	return ret;
}

/**
 * nih_main_unlink_pidfile:
 *
 * Removes the process's pid file, which can be set with
 * nih_main_set_pidfile().
 *
 * Errors are ignored, since there's not much you can do about it.
 **/
void
nih_main_unlink_pidfile (void)
{
	const char *filename;

	filename = nih_main_get_pidfile ();
	unlink (filename);
}


/**
 * nih_main_loop_init:
 *
 * Initialise the loop functions list.
 **/
void
nih_main_loop_init (void)
{
	if (! nih_main_loop_functions)
		nih_main_loop_functions = NIH_MUST (nih_list_new (NULL));

	/* Set up the interrupt pipe, we need it to be non blocking so that
	 * we don't accidentally block if there's too many signals been
	 * triggered or something
	 */
	if (interrupt_pipe[0] == -1) {
		NIH_ZERO (pipe (interrupt_pipe));

		nih_io_set_nonblock (interrupt_pipe[0]);
		nih_io_set_nonblock (interrupt_pipe[1]);

		nih_io_set_cloexec (interrupt_pipe[0]);
		nih_io_set_cloexec (interrupt_pipe[1]);
	}
}

/**
 * nih_main_loop:
 *
 * Implements a fully functional main loop for a typical process, handling
 * I/O events, signals, termination of child processes, timers, etc.
 *
 * Returns: value given to nih_main_loop_exit().
 **/
int
nih_main_loop (void)
{
	nih_main_loop_init ();

	/* Set a handler for SIGCHLD so that it can interrupt syscalls */
	nih_signal_set_handler (SIGCHLD, nih_signal_handler);

	while (! exit_loop) {
		NihTimer       *next_timer;
		struct timespec now;
		struct timeval  timeout;
		fd_set          readfds, writefds, exceptfds;
		char            buf[1];
		int             nfds, ret;

		/* Use the due time of the next timer to calculate how long
		 * to spend in select().  That way we don't sleep for any
		 * less or more time than we need to.
		 */
		next_timer = nih_timer_next_due ();
		if (next_timer) {
			nih_assert (clock_gettime (CLOCK_MONOTONIC, &now) == 0);

			timeout.tv_sec = next_timer->due - now.tv_sec;
			timeout.tv_usec = 0;
		}

		/* Start off with empty watch lists */
		FD_ZERO (&readfds);
		FD_ZERO (&writefds);
		FD_ZERO (&exceptfds);

		/* Always look for changes in the interrupt pipe */
		FD_SET (interrupt_pipe[0], &readfds);
		nfds = interrupt_pipe[0] + 1;

		/* And look for changes in anything we're watching */
		nih_io_select_fds (&nfds, &readfds, &writefds, &exceptfds);

		/* Now we hang around until either a signal comes in (and
		 * calls nih_main_loop_interrupt), a file descriptor we're
		 * watching changes in some way or it's time to run a timer.
		 */
		ret = select (nfds, &readfds, &writefds, &exceptfds,
			      (next_timer ? &timeout : NULL));

		/* Deal with events */
		if (ret > 0)
			nih_io_handle_fds (&readfds, &writefds, &exceptfds);

		/* Deal with signals.
		 *
		 * Clear the list first so that if a signal occurs while
		 * handling signals it'll ensure that the functions get
		 * a chance to decide whether to do anything next time round
		 * without having to wait.
		 */
		while (read (interrupt_pipe[0], buf, sizeof (buf)) > 0)
			;
		nih_signal_poll ();

		/* Deal with terminated children */
		nih_child_poll ();

		/* Deal with timers */
		nih_timer_poll ();

		/* Run the loop functions */
		NIH_LIST_FOREACH_SAFE (nih_main_loop_functions, iter) {
			NihMainLoopFunc *func = (NihMainLoopFunc *)iter;

			func->callback (func->data, func);
		}
	}

	exit_loop = 0;
	return exit_status;
}

/**
 * nih_main_loop_interrupt:
 *
 * Interrupts the current (or next) main loop iteration because of an
 * event that potentially needs immediate processing, or because some
 * condition of the main loop has been changed.
 **/
void
nih_main_loop_interrupt (void)
{
	nih_main_loop_init ();

	if (interrupt_pipe[1] != -1)
		while (write (interrupt_pipe[1], "", 1) < 0)
			;
}

/**
 * nih_main_loop_exit:
 * @status: exit status.
 *
 * Instructs the current (or next) main loop to exit with the given exit
 * status; if the loop is in the middle of processing, it will exit once
 * all that processing is complete.
 *
 * This may be safely called by functions called by the main loop.
 **/
void
nih_main_loop_exit (int status)
{
	exit_status = status;
	exit_loop = TRUE;

	nih_main_loop_interrupt ();
}


/**
 * nih_main_loop_add_func:
 * @parent: parent object for new callback,
 * @callback: function to call,
 * @data: pointer to pass to @callback.
 *
 * Adds @callback to the list of functions that should be called once
 * in each main loop iteration.
 *
 * The callback structure is allocated using nih_alloc() and stored in a
 * linked list. Removal of the callback can be performed by freeing it.
 *
 * If @parent is not NULL, it should be a pointer to another object which
 * will be used as a parent for the returned callback.  When all parents
 * of the returned callback are freed, the returned callback will also be
 * freed.
 *
 * Returns: the function information, or NULL if insufficient memory.
 **/
NihMainLoopFunc *
nih_main_loop_add_func (const void    *parent,
			NihMainLoopCb  callback,
			void          *data)
{
	NihMainLoopFunc *func;

	nih_assert (callback != NULL);

	nih_main_loop_init ();

	func = nih_new (parent, NihMainLoopFunc);
	if (! func)
		return NULL;

	nih_list_init (&func->entry);

	nih_alloc_set_destructor (func, nih_list_destroy);

	func->callback = callback;
	func->data = data;

	nih_list_add (nih_main_loop_functions, &func->entry);

	return func;
}


/**
 * nih_main_term_signal:
 * @data: ignored,
 * @signal: ignored.
 *
 * Signal callback that instructs the main loop to exit with a normal
 * exit status, usually registered for SIGTERM and SIGINT for non-daemons.
 **/
void
nih_main_term_signal (void      *data,
		      NihSignal *signal)
{
	nih_main_loop_exit (0);
}

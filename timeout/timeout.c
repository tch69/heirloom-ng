/* 
 * timeout.c - execute a command with a time limit 
 */
/*
 * Copyright (C) 2023, 2024: Luiz Antônio (takusuman)
 *                           Arthur Bacci (arthurbacci)(atr)
 *
 * SPDX-Licence-Identifier: Zlib
 *
 * The 'validate_duration' function contains code fro Heirloom-NG's
 * watch(1) implementation that parses a float for timespec structs.
 * This code was written by Arthur Bacci and it's covered per the
 * same terms as this program.
 *
 * The part that makes the called command, in fact, sleep was
 * throloughly based on OpenBSD's implementation of timeout.
 * As per the copyright header of OpenBSD's "usr.bin/timeout/timeout.c":
 * Copyright (c) 2021 Job Snijders <job@openbsd.org>
 * Copyright (c) 2014 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2014 Vsevolod Stakhov <vsevolod@FreeBSD.org>
 *
 * SPDX-Licence-Identifier: BSD-2-Clause
 */

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <pfmt.h>
#include <signal.h>
#include <sigtable.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <unistd.h>
#include <wait.h>
#include <stdbool.h>

static char *progname;

int main(int argc, char *argv[]);
int validate_signal(char *signal_name);
struct TClock validate_duration(char *duration);
int parse_interval(const char *ss, struct TClock *interval);
void settimeout(struct TClock *duration);
void handle_signal(int signo);
void usage(void);

struct TClock {
	time_t sec;
	long int nsec;
};

struct LSignal {
	sig_atomic_t sig_alrm;
	sig_atomic_t sig_chld;
	sig_atomic_t sig_term;
	sig_atomic_t sig_ign;
};
static struct LSignal siglist;

/*
 * Rodar o comando e bifurcar para o fundo
 * "Matar" o comando depois de X tempo com SIGTERM
 * Se ainda estiver rodando, matar em definitivo com
 * o sinal informado pelo usuário --- esperar por mais
 * tempo? Opção '-k'.
 *
 * Translation by atr:
 * Runs the command and fork to the background
 * "Kill" the command after X amount of time with
 * SIGTERM
 * If it's still running, definitely kill with the
 * signal given by the user --- want it to wait for
 * more time? Use the option '-k'.
 */

int main(int argc, char *argv[]) {
	progname = argv[0];
	register int c = 0,
	    s = 0;
	int option = 0,
	    fForeground = 0,
	    fOnemoretime = 0,
	    fPreserve_status = 0,
	    killer_sig = SIGTERM, /* Default killer signal is SIGTERM. */
			/* 
			 * -1 can be replaced by another signal
			 * if the user sets the -s flag.
			 */
	    sigsused[] = { -1,  SIGHUP, SIGINT, SIGQUIT,
		    		SIGTERM, SIGCHLD, SIGALRM },
	    execerr = 0, /* Command execution error code. */
	    ecmd = 0, /* Command exit code. */
	    eprog = 0; /* Program exit code. */
	/* atr note: it might be better to have other ints replaced by bool
	 *           since it makes it a lot easier to tell what is being
	 *           acomplished.
	 */
	bool timesout = false;
	char **commandv,
	     *fst_commandv;
	pid_t cmdpid = 0,
	      exec_pid = 0,
	      pgid = 0;
	
	struct TClock first_interval = {0},
	      second_interval = {0};
	struct sigaction sa = {0};

	while ( (option = getopt(argc, argv, "fps:k:h")) != -1 ){
		switch (option) {
			case 'f':
				/* According to GNU's timeout(1) usage()
				 * function:
				 * "when not running timeout directly from a
				 * shell prompt, allow COMMAND to read from
				 * the TTY and get TTY signals; in this mode,
				 * children of COMMAND will not be timed out.
				 */
				fForeground = 1;
				break;
			case 'p':
				/* Simplest option overall, it catches process'
				 * exit code and returns it instead of returning
				 * timeout's default "timed out" exit code (124).
				 * This is only valid for processes that did not
				 * exit as zero.
				 */
				fPreserve_status = 1;
				break;
			case 's':
				killer_sig = validate_signal(optarg);
				break;
			case 'k':
				fOnemoretime = 1;
				second_interval = validate_duration(optarg);
				break;
			case 'h':
			default:
				usage();
				break;
		}
	}
	argc -= optind;
	argv += optind;

	/* Missing operand. */
	if ( argc < 2 ){
		usage();
	}

	/* atr note: if one wants to use malloc he must be sure that the last
	 * element of commandv is set to NULL
	 */
	/* Allocate commandv[], where argv[] will be copied to. */
	if ( (commandv = calloc((size_t)argc, sizeof(char *))) == NULL ) { 
		pfmt(stderr, MM_ERROR, "%s: could not allocate an array of "
		                       "%d elements, each one being %lu "
				       "bytes large.\n", progname,
				       argc, sizeof(char *)
		);
		exit(1);
	}

	/* And then copy argv[] to commandv[]. */
	/* atr: note that the previous code was only copying the reference, I
	 * believe it's a mistake and replaced it by strdup.
	 */
	/* atr: it's shifted, the first element of commandv is the second of
	 * argv and so on.
	 */
	for (c = 1; c < argc; c++) {
		commandv[c - 1] = strdup(argv[c]);
	}

	/* atr: dummy string for funny mutations */
	fst_commandv = strdup(argv[0]);
	first_interval = validate_duration(fst_commandv);
	free(fst_commandv);


	if (! fForeground) {
		/* 
		 * setpgid(pid, pgid)
		 * If both the 'pid' and 'pgid' are equal to zero, setpgid(2)
		 * assumes that both the child process pid and pgid shall be the
		 * same as parent's, that is why it is used in this case, where
		 * we do not want to foreground the process.
		 */
		if ((pgid = setpgid(0, 0)) != 0) {
			pfmt(stderr, MM_ERROR,
			     "%s: failed to set process group via setpgid"
			     "(0, 0): %s.\n", progname, strerror(errno));
		}
	}

	/* 
	 * The list of signals masked (blocked) during the execution of the
	 * handler (which is present at sigaction.sa_handler/sa_sigaction)
	 * must be initialized per excluding the current list at sa_mask.
	 */
	sigemptyset(&sa.sa_mask);
	
	/* 
	 * Since these two signals can not be caught nor ignored (using
	 * sa_mask), its inclusion at the signal ignore array must avoided.
	 * It will not cause any error, though, just be ignored.
	 */
	if (killer_sig != SIGKILL && killer_sig != SIGSTOP) {
		sigsused[0] = killer_sig;
	}

	/*
	 * Iterate over the sigsused[] integer array and start adding
	 * each one to the sa_mask set again.
	 */
	for (s = 0; s < (int)(sizeof(sigsused)/sizeof(sigsused[0])); s++) {
		sigaddset(&sa.sa_mask, sigsused[s]);
	}

	sa.sa_handler = &handle_signal;

	/*
	 * Set SA_RESTART at sa_flags, this will make certain system calls
	 * restartable by signals. This is useful because, after handle_signal()
	 * returns, the system call is restarted instead of failing with EINTR,
	 * which means an error by interruption.
	 */
	sa.sa_flags = SA_RESTART;

	/*
	 * Iterate over the sigsused[] integer array again, but this time it
	 * will be using sigaction() for changing an action taken by the system
	 * when any of the signals of the array --- which also are present on the
	 * handle_signal() function and, as the name explicitaly say, handled by
	 * it --- are sent, so, when any of these signals are sent, it actually
	 * calls handle_signal().
	 */
	for (s = 0; s < (int)(sizeof(sigsused)/sizeof(sigsused[0])); s++) {
		if (sigsused[s] != -1 && sigsused[s] != 0
		&& sigaction(sigsused[s], &sa, NULL) != 0) {
			pfmt(stderr, MM_ERROR,
				"%s: failed to change action for %s: %s.\n",
            			progname, strsignal(sigsused[s]),
					strerror(errno));
			exit(1);
		}
	}

	/* 
	 * This is meant to ignore stop signals that could come from the
	 * terminal on the parent process.
	 */
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);

	exec_pid = fork();

	/* Get the bad news first. */
	if (exec_pid == -1) {
		pfmt(stderr, MM_ERROR, "%s: failed to fork: %s.\n",
        	    progname, strerror(errno));
	} else if (exec_pid == 0) {
		/* 
		 * On the child process, signals sent by the terminal mustn't
		 * be ignored anymore and that the default action --- without
		 * involving the handler --- have to be taken by the system.
		 */
		signal(SIGTTIN, SIG_DFL);
		signal(SIGTTOU, SIG_DFL);

		/* Here we will be executing the child process. */
		if ( (execvp(commandv[0], commandv)) == -1 ) {
			pfmt(stderr, MM_ERROR, "%s: failed to exec(): %s.\n",
					progname, strerror(errno));

		}

		/*
		 * According to GNU's timeout(1) manual page:
		 *
		 *  126    if COMMAND is found but cannot be invoked
		 *  127    if COMMAND cannot be found
		 * 
		 *  ENOENT stands for "error no entity/entry", so it
		 *  means, in this context, no such file or directory.
		 */
		execerr = (errno == ENOENT) ? 127 : 126;
		_exit(execerr);
	}
	
	/* The command string will not be necessary after exec(). */
	for (int i = 0; commandv[i]; i++)
		/* atr: frees the strduped strings */
		free(commandv[i]);
	free(commandv);

	/* 
	 * Add signals from the sa_mask set to the SIG_BLOCK set,
	 * blocking it after the execution of the commmand.
	 */
	if (sigprocmask(SIG_BLOCK, &sa.sa_mask, NULL) != 0) {
		pfmt(stderr, MM_ERROR, "%s: failed to sigprocmask(): %s.\n",
				progname, strerror(errno));
		exit(1);
	}

	settimeout(&first_interval);

	while (true) {
		/* 
		 * The sa_mask will be empty again, but we will
		 * also be using sigsuspend() to prevent the
		 * function assigned to sa_handler of
		 * interrupting this section. 
		 */
		sigemptyset(&sa.sa_mask);
		sigsuspend(&sa.sa_mask);

		if (siglist.sig_chld) {
			siglist.sig_chld = 0;

			while ((cmdpid = wait(&ecmd)) < 0 && errno == EINTR);

			if (cmdpid == exec_pid) {
				eprog = ecmd;
				break;
			}
		} else if (siglist.sig_alrm || siglist.sig_term) {
			if (siglist.sig_alrm) {
				siglist.sig_alrm = 0;
				timesout = true;
			}

			if (! fForeground) {
				killpg(pgid, killer_sig);
			} else {
				kill(exec_pid, siglist.sig_term
						? (int)siglist.sig_term
						: killer_sig);
			}

			if (! fOnemoretime) {
				break;
			} else {
				settimeout(&second_interval);
				fOnemoretime = 0;
				siglist.sig_ign = killer_sig;
				killer_sig = SIGKILL;
			}

		}

	}

	while (cmdpid != exec_pid && wait(&eprog) == -1) {
		if (errno != EINTR) {
			pfmt(stderr, MM_ERROR, "%s: failed to wait(): %s.\n",
			progname, strerror(errno));

			exit(1);
		}
	}
	
	if (timesout && !fPreserve_status)
		return 124;
	if (WEXITSTATUS(eprog))
		return WEXITSTATUS(eprog);
	if (WIFSIGNALED(eprog))
		return 128 + WTERMSIG(eprog);
	

	return eprog;
}


void settimeout(struct TClock *duration) {
	timer_t tid;
	struct itimerspec its;

	/* Since the second value, sevp on the prototype of
	 * timer_create(2), is defined as NULL, every
	 * expiration of this timer will sent an SIGALRM.
	 * Just as expected.
	 */
	if (timer_create(CLOCK_REALTIME, NULL, &tid) != 0) {
		pfmt(stderr, MM_ERROR,
			"%s: failed to create timer: %s\n",
			progname, strerror(errno));
		exit(1);
	}

	/* Timer expiration. */	
	its.it_value.tv_sec = duration->sec;
	its.it_value.tv_nsec = duration->nsec;
	/* Timer period. */
	its.it_interval.tv_sec = its.it_value.tv_sec;
	its.it_interval.tv_nsec = its.it_value.tv_nsec;

	if (timer_settime(tid, 0, &its, NULL) != 0) {
		pfmt(stderr, MM_ERROR,
			"%s: failed to set timer: %s\n",
			progname, strerror(errno));
		exit(1);
	}
}

int validate_signal(char *str) {
	int signum = 0; /* EXIT if not defined. */

	/* 
	 * Check if the first character
	 * of the input string is a
	 * letter, so it can be parsed into
	 * a signal name.
	 */
	if (isalpha(str[0])) {
		register int s = 0;
		
		/* Starts with "SIG". */
		char *ssuffix = "SIG";
		if (strncmp(str, ssuffix, 3) == 0) {
			/* 
			 * Re-define string 'str'
			 * from its first three
			 * initial characters ("SIG").
			 */
			str = &str[3];
		}

		for (; (strcmp(str, sig_strs[s].signame) != 0);) {
			s += 1;
		}
		signum = sig_strs[s].signum;
	} else {
		int i = 0;

		i = atoi(str);	
		if (i < 1 || i > SIGRTMAX) {
			pfmt(stderr, MM_ERROR,
				"%s: invalid signal %d.\n",
				progname, i);
			exit(1);
		}
		signum = i;
	}

	return signum;
}

struct TClock validate_duration(char *timestr) {
	struct TClock mitoclock;
	
	if (-1 == parse_interval(timestr, &mitoclock)) {
		pfmt(stderr, MM_ERROR, "the given interval couldn't be parsed");
		exit(1);
	}

	return mitoclock;
}

/* Adapted from suspicious-tools' watch (available at
 * https://github.com/arthurbacci/suspicious-tools) and licensed under the
 * public domain
 */
/* Returns -1 if the given string is invalid, otherwise 0 is returned and the
 * interval struct is set.
 */
int parse_interval(const char *ss, struct TClock *interval) {
	char s[32];
	char *afterpoint = NULL;
	size_t i, afterpoint_len;

	strncpy(s, ss, 32);
	s[31] = '\0';

	for (i = 0; s[i]; i++) {
		if (s[i] == '.') {
			if (afterpoint == NULL) {
				afterpoint = &s[i + 1];
				s[i] = '\0';
			} else {
				return -1;
			}
		} else if (!isdigit(s[i])) {
			return -1;
		}
	}

	interval->sec = strlen(s) ? atoi(s) : 0;
	interval->nsec = afterpoint ? atoi(afterpoint) : 0;

	if (afterpoint == NULL) return 0;

	afterpoint_len = strlen(afterpoint);
	if (afterpoint_len > 9) return -1;
	for (i = afterpoint_len; i < 9; i++)
		interval->nsec *= 10;

	return 0;
}


// This is just a function that sets variables --- which
// indicates which signals where enabled or not ---, so it
// does not return anything. I wish these could be returned
// on something like a struct, like it was done for the
// interval, but the UNIX ABI does not give this opportunity.
void handle_signal(int signo) {
	if (siglist.sig_ign != 0 && signo == siglist.sig_ign) {
		// Ignore this signal.
		siglist.sig_ign = 0;
		return;
	}

	switch (signo) {
		case SIGHUP:
		case SIGINT:
		case SIGQUIT:
		case SIGTERM:
			siglist.sig_term = signo;
			break;
		case SIGCHLD:
			siglist.sig_chld = 1;
			break;
		case SIGALRM:
			siglist.sig_alrm = 1;
			break;
	}
}

void usage(void) {
	pfmt(stderr, MM_NOSTD, "usage: %s: [-fp] [-s signal] [-k time] "
	                       "time [command [args ...]]\n", progname);
	exit(1);
}


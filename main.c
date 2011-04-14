/*
	dtach - A simple program that emulates the detach feature of screen.
	Copyright (C) 2004-2008 Ned T. Crigler

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
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include "dtach.h"

/*
** dtach is a quick hack, since I wanted the detach feature of screen without
** all the other crud. It'll work best with full-screen applications, as it
** does not keep track of the screen or anything like that.
*/

/* Make sure the binary has a copyright. */
const char copyright[] = "dtach - version " PACKAGE_VERSION "(C)Copyright 2004-2008 Ned T. Crigler";

/* argv[0] from the program */
char *progname;
/* The name of the passed in socket. */
char *sockname;
/* The value of $DTACH, if set. */
char * dtach_env;
/* The character used for detaching. Defaults to '^\' */
int detach_char = '\\' - 64;
/* 1 if we should not interpret the suspend character. */
int no_suspend;
/* The default redraw method. Initially set to unspecified. */
int redraw_method = REDRAW_UNSPEC;

/*
** The original terminal settings. Shared between the master and attach
** processes. The master uses it to initialize the pty, and the attacher uses
** it to restore the original settings.
*/
struct termios orig_term;
int dont_have_tty;

static void
usage()
{
	printf(
		"dtach - version %s, compiled on %s at %s.\n"
		"Usage: dtach -a <socket> <options>\n"
		"		dtach -A <socket> <options> <command...>\n"
		"		dtach -c <socket> <options> <command...>\n"
		"		dtach -n <socket> <options> <command...>\n"
		"Modes:\n"
		"  -a\t\tAttach to the specified socket.\n"
		"  -A\t\tAttach to the specified socket, or create it if it\n"
		"\t\t  does not exist, running the specified command.\n"
		"  -c\t\tCreate a new socket and run the specified command.\n"
		"  -n\t\tCreate a new socket and run the specified command "
		"detached.\n"
		"Options:\n"
		"  -e <char>\tSet the detach character to <char>, defaults "
		"to ^\\.\n"
		"  -E\t\tDisable the detach character.\n"
		"  -r <method>\tSet the redraw method to <method>. The "
		"valid methods are:\n"
		"\t\t	  none: Don't redraw at all.\n"
		"\t\t	ctrl_l: Send a Ctrl L character to the program.\n"
		"\t\t	 winch: Send a WINCH signal to the program.\n"
		"  -z\t\tDisable processing of the suspend key.\n"
		"\nIf the environment variable $DTACH is set, the location\n"
		"it points to will be used as the socket folder. For example:\n"
		"\tDTACH=/tmp/dtach dtach -A foo ...\n"
		"will connect the socket /tmp/dtach/foo. You can override this by\n"
		"providing an absolute path for the socket, i.e.:\n"
		"\tDTACH=/tmp/dtach dtach -A /tmp/foo ...\n"
		"will create the socket at /tmp/foo, not /tmp/dtach/tmp/foo.\n"
		"\nReport any bugs to <" PACKAGE_BUGREPORT ">.\n",
		PACKAGE_VERSION, __DATE__, __TIME__);
	exit(0);
}

int
main(int argc, char **argv)
{
	int mode = 0;

	/* Save the program name */
	progname = argv[0];
	++argv; --argc;

	/* Parse the arguments */
	if (argc >= 1 && **argv == '-')
	{
		if (strncmp(*argv, "--help", strlen(*argv)) == 0)
			usage();
		else if (strncmp(*argv, "--version", strlen(*argv)) == 0)
		{
			printf("dtach - version %s, compiled on %s at %s.\n",
				PACKAGE_VERSION, __DATE__, __TIME__);
			return 0;
		}

		mode = argv[0][1];
		if (mode == '?')
			usage();
		else if (mode != 'a' && mode != 'c' && mode != 'n' &&
			 mode != 'A')
		{
			printf("%s: Invalid mode '-%c'\n", progname, mode);
			printf("Try '%s --help' for more information.\n",
				progname);
			return 1;
		}
	}
	if (!mode)
	{
		printf("%s: No mode was specified.\n", progname);
		printf("Try '%s --help' for more information.\n",
			progname);
		return 1;
	}
	++argv; --argc;

	if (argc < 1)
	{
		printf("%s: No socket was specified.\n", progname);
		printf("Try '%s --help' for more information.\n",
			progname);
		return 1;
	}
	sockname = *argv;
	
	/*
	 * Ignore $DTACH even if set if the given socket name is:
	 * + an absolute path
	 * + an explicit path in the current working directory
	 * + a path with a directory traversal (..)
	 */
	dtach_env = getenv("DTACH");
	if (dtach_env) {
		int not_abs = strncmp(sockname, "/", 1);
		int not_cwd = strncmp(sockname, "./", 2);
		int not_traversal = !strstr(sockname, "..");
		if (not_abs && not_cwd && not_traversal) {
			// check if $DTACH exists yet
			struct stat dloc;
			if (!stat(dtach_env, &dloc)) {
				if (!S_ISDIR(dloc.st_mode)) {
					printf("%s: ", progname);
					printf("$DTACH exists but is not a directory.\n");
					printf("Either delete it or point $DTACH elsewhere.\n");
					printf("$DTACH is: %s\n", dtach_env);
					return 1;
				}
			}
			else {
				if (mkdir(dtach_env, 0755)) {
					perror(dtach_env);
					return 1;
				}
			}
			// len of new string, plus another /
			int maxlen = strlen(sockname) + strlen(dtach_env) + 1;
			char * newsock = calloc(maxlen, sizeof(char));
			strncpy(newsock, dtach_env, strlen(dtach_env));
			strncat(newsock, "/", 1);
			strncat(newsock, sockname, strlen(sockname));
			sockname = newsock;
		}
	}
	++argv; --argc;

	while (argc >= 1 && **argv == '-')
	{
		char *p;

		for (p = argv[0] + 1; *p; ++p)
		{
			if (*p == 'E')
				detach_char = -1;
			else if (*p == 'z')
				no_suspend = 1;
			else if (*p == 'e')
			{
				++argv; --argc;
				if (argc < 1)
				{
					printf("%s: No escape character "
						"specified.\n", progname);	
					printf("Try '%s --help' for more "
						"information.\n", progname);
					return 1;
				}
				if (argv[0][0] == '^' && argv[0][1])
				{
					if (argv[0][1] == '?')
						detach_char = '\177';
					else
						detach_char = argv[0][1] & 037;
				}
				else
					detach_char = argv[0][0];
				break;
			}
			else if (*p == 'r')
			{
				++argv; --argc;
				if (argc < 1)
				{
					printf("%s: No redraw method "
						"specified.\n", progname);	
					printf("Try '%s --help' for more "
						"information.\n", progname);
					return 1;
				}
				if (strcmp(argv[0], "none") == 0)
					redraw_method = REDRAW_NONE;
				else if (strcmp(argv[0], "ctrl_l") == 0)
					redraw_method = REDRAW_CTRL_L;
				else if (strcmp(argv[0], "winch") == 0)
					redraw_method = REDRAW_WINCH;
				else
				{
					printf("%s: Invalid redraw method "
						"specified.\n", progname);	
					printf("Try '%s --help' for more "
						"information.\n", progname);
					return 1;
				}
				break;
			}
			else
			{
				printf("%s: Invalid option '-%c'\n",
					progname, *p);
				printf("Try '%s --help' for more information.\n",
					progname);
				return 1;
			}
		}
		++argv; --argc;
	}

	if (mode != 'a' && argc < 1)
	{
		printf("%s: No command was specified.\n", progname);
		printf("Try '%s --help' for more information.\n",
			progname);
		return 1;
	}

	/* Save the original terminal settings. */
	if (tcgetattr(0, &orig_term) < 0)
	{
		memset(&orig_term, 0, sizeof(struct termios));
		dont_have_tty = 1;
	}

	if (dont_have_tty && mode != 'n')
	{
		printf("%s: Attaching to a session requires a terminal.\n",
			progname);
		return 1;
	}

	if (mode == 'a')
	{
		if (argc > 0)
		{
			printf("%s: Invalid number of arguments.\n",
				progname);
			printf("Try '%s --help' for more information.\n",
				progname);
			return 1;
		}
		return attach_main(0);
	}
	else if (mode == 'n')
		return master_main(argv, 0);
	else if (mode == 'c')
	{
		if (master_main(argv, 1) != 0)
			return 1;
		return attach_main(0);
	}
	else if (mode == 'A')
	{
		/* Try to attach first. If that doesn't work, create a new
		** socket. */
		if (attach_main(1) != 0)
		{
			if (errno == ECONNREFUSED || errno == ENOENT)
			{
				if (errno == ECONNREFUSED)
					unlink(sockname);
				if (master_main(argv, 1) != 0)
					return 1;
			}
			return attach_main(0);
		}
	}
	return 0;
}

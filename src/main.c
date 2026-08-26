#include "gpuwho.h"

#include <stdlib.h>
#include <string.h>

static void usage(FILE *f)
{
	fprintf(f,
"gpuwho " GPUWHO_VERSION " -- who is using the GPUs\n"
"\n"
"usage:\n"
"  gpuwho [options]            snapshot: who is on the GPUs right now\n"
"  gpuwho wait [options]       block until a GPU is free, then notify\n"
"  gpuwho report [options]     aggregate the event log over a time window\n"
"  gpuwho collect              one-shot collector tick (run from the timer)\n"
"  gpuwho setup [options]      enable accounting mode, print install guidance\n"
"\n"
"global options:\n"
"  --state-dir DIR   where state.json and the event log live\n"
"                    (default " GPUWHO_STATE_DIR ", or $GPUWHO_STATE_DIR)\n"
"  --no-color        never colorize, even on a TTY (NO_COLOR is honored too)\n"
"  -h, --help        this text, or per-command help after a subcommand\n"
"  -V, --version     print the version and exit\n"
"\n"
"Run 'gpuwho <command> --help' for the options of a single command.\n");
}

int main(int argc, char **argv)
{
	char **rest;
	int    nrest = 0;
	int    i;
	int    want_help = 0;
	const char *cmd;

	rest = gw_malloc(sizeof(char *) * (size_t)(argc + 1));
	rest[nrest++] = argv[0];

	/* Global options are accepted anywhere on the line; peel them off and
	 * hand whatever is left to the subcommand. */
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "--state-dir") == 0) {
			if (i + 1 >= argc)
				gw_die("--state-dir needs a directory");
			gw_set_state_dir(argv[++i]);
		} else if (strncmp(a, "--state-dir=", 12) == 0) {
			gw_set_state_dir(a + 12);
		} else if (strcmp(a, "--no-color") == 0) {
			gw_set_no_color(1);
		} else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
			printf("gpuwho %s\n", GPUWHO_VERSION);
			free(rest);
			return 0;
		} else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			want_help = 1;
			rest[nrest++] = argv[i];
		} else {
			rest[nrest++] = argv[i];
		}
	}
	rest[nrest] = NULL;

	cmd = (nrest > 1) ? rest[1] : NULL;

	if (cmd && strcmp(cmd, "help") == 0) {
		usage(stdout);
		free(rest);
		return 0;
	}

	if (cmd && cmd[0] != '-') {
		int rc;
		int sub_argc = nrest - 1;
		char **sub_argv = rest + 1;

		if (strcmp(cmd, "wait") == 0)
			rc = gw_cmd_wait(sub_argc, sub_argv);
		else if (strcmp(cmd, "collect") == 0)
			rc = gw_cmd_collect(sub_argc, sub_argv);
		else if (strcmp(cmd, "report") == 0)
			rc = gw_cmd_report(sub_argc, sub_argv);
		else if (strcmp(cmd, "setup") == 0)
			rc = gw_cmd_setup(sub_argc, sub_argv);
		else
			gw_die("unknown command '%s' (try 'gpuwho --help')", cmd);

		free(rest);
		return rc;
	}

	if (want_help) {
		/* Bare "gpuwho --help": the snapshot command prints its own
		 * options, but the overview belongs here. */
		usage(stdout);
		free(rest);
		return 0;
	}

	{
		int rc = gw_cmd_snapshot(nrest, rest);
		free(rest);
		return rc;
	}
}

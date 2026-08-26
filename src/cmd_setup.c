/* gpuwho setup -- turn on accounting mode and explain the rest.
 *
 * Accounting mode is the source of every per-process metric in the history
 * layer.  Enabling it needs root; querying it does not.  Both the mode and the
 * accounting buffer are cleared by a reboot or a driver reload, which is why a
 * boot-time oneshot unit re-enables it. */

#include "gpuwho.h"

#include <string.h>
#include <unistd.h>

static void setup_usage(FILE *f)
{
	fprintf(f,
"usage: gpuwho setup [options]\n"
"\n"
"Enable NVIDIA accounting mode on every GPU (requires root) and print the\n"
"systemd units that keep it enabled and run the collector.\n"
"\n"
"options:\n"
"  --check       report the current state, change nothing\n"
"  --quiet       only report failures (for use from a unit file)\n"
"  -h, --help    this text\n");
}

static void print_guidance(void)
{
	printf(
"\n"
"History needs two units and a timer.  From a checkout:\n"
"\n"
"    sudo make install install-units\n"
"    sudo systemctl daemon-reload\n"
"    sudo systemctl enable --now gpuwho-accounting.service\n"
"    sudo systemctl enable --now gpuwho-collect.timer\n"
"\n"
"Or write them by hand:\n"
"\n"
"--- /etc/systemd/system/gpuwho-accounting.service ---\n"
"[Unit]\n"
"Description=Enable NVIDIA accounting mode for gpuwho\n"
"After=nvidia-persistenced.service\n"
"\n"
"[Service]\n"
"Type=oneshot\n"
"ExecStart=/usr/bin/nvidia-smi -am 1\n"
"\n"
"[Install]\n"
"WantedBy=multi-user.target\n"
"\n"
"--- /etc/systemd/system/gpuwho-collect.service ---\n"
"[Unit]\n"
"Description=gpuwho collector tick\n"
"After=gpuwho-accounting.service\n"
"\n"
"[Service]\n"
"Type=oneshot\n"
"ExecStart=/usr/local/bin/gpuwho collect\n"
"StateDirectory=gpuwho\n"
"\n"
"--- /etc/systemd/system/gpuwho-collect.timer ---\n"
"[Unit]\n"
"Description=Run gpuwho collector every minute\n"
"\n"
"[Timer]\n"
"OnBootSec=1min\n"
"OnUnitActiveSec=1min\n"
"AccuracySec=5s\n"
"\n"
"[Install]\n"
"WantedBy=timers.target\n"
"\n"
"Run nvidia-persistenced so the driver stays resident while the GPUs are\n"
"idle; otherwise the module unloads and accounting mode is lost.\n");
}

int gw_cmd_setup(int argc, char **argv)
{
	unsigned int ndev, i;
	int          check_only = 0, quiet = 0;
	int          rc = 0, nfailed = 0;
	int          j;

	for (j = 1; j < argc; j++) {
		const char *a = argv[j];
		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			setup_usage(stdout);
			return 0;
		} else if (strcmp(a, "--check") == 0) {
			check_only = 1;
		} else if (strcmp(a, "--quiet") == 0) {
			quiet = 1;
		} else {
			gw_die("unknown option '%s' (try 'gpuwho setup --help')", a);
		}
	}

	if (gw_nvml_init() != 0) {
		gw_warn("%s", gw_nvml_err());
		return 1;
	}
	if (gw_device_count(&ndev) != 0 || ndev == 0) {
		gw_warn("no NVIDIA GPUs found");
		gw_nvml_fini();
		return 1;
	}

	for (i = 0; i < ndev; i++) {
		int          enabled = 0;
		unsigned int bufsize = 0;

		if (gw_acct_mode(i, &enabled) != 0) {
			gw_warn("GPU %u: %s", i, gw_nvml_err());
			nfailed++;
			continue;
		}

		if (!enabled && !check_only) {
			if (gw_acct_enable(i) == 0) {
				enabled = 1;
			} else {
				gw_warn("GPU %u: cannot enable accounting: %s", i,
				        gw_nvml_err());
				if (geteuid() != 0)
					gw_warn("  (enabling accounting mode "
					        "requires root)");
				nfailed++;
			}
		}

		if (gw_acct_buffer_size(i, &bufsize) != 0)
			bufsize = 0;

		if (!quiet) {
			printf("GPU %u  accounting %-8s", i,
			       enabled ? "enabled" : "disabled");
			if (bufsize)
				printf("  buffer %u processes", bufsize);
			printf("\n");
		}
	}

	if (nfailed)
		rc = 1;

	if (!quiet)
		print_guidance();

	gw_nvml_fini();
	return rc;
}

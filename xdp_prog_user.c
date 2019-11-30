/* SPDX-License-Identifier: GPL-2.0 */

static const char *__doc__ = "XDP redirect helper\n"
	" - Allows to populate/query tx_port and redirect_params maps\n";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>

#include <locale.h>
#include <unistd.h>
#include <time.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <net/if.h>
#include <linux/if_ether.h>
#include <linux/if_link.h> /* depend on kernel-headers installed */

#include "common/common_params.h"
#include "common/common_user_bpf_xdp.h"
#include "common/common_libbpf.h"

#include "common/xdp_stats_kern_user.h"

/* re2dfa library */
#include "common/re2dfa.h"

/* IDS Inspect Uit */
typedef __u8 ids_inspect_unit;

/* IDS Inspect State */
typedef __u16 ids_inspect_state;

/* Key-Value of ids_inspect_map */
struct ids_inspect_map_key {
	ids_inspect_state state;
	ids_inspect_unit unit;
	__u8 padding;
};

struct ids_inspect_map_value {
	__u16 final_state;
	ids_inspect_state state;
};

static const char *ids_inspect_map_name = "ids_inspect_map";

static const struct option_wrapper long_options[] = {

	{{"help",        no_argument,		NULL, 'h' },
	 "Show help", false},

	{{"dev",         required_argument,	NULL, 'd' },
	 "Operate on device <ifname>", "<ifname>", true},

	{{"redirect-dev",         required_argument,	NULL, 'r' },
	 "Redirect to device <ifname>", "<ifname>", true},

	{{"src-mac", required_argument, NULL, 'L' },
	 "Source MAC address of <dev>", "<mac>", true },

	{{"dest-mac", required_argument, NULL, 'R' },
	 "Destination MAC address of <redirect-dev>", "<mac>", true },

	{{"quiet",       no_argument,		NULL, 'q' },
	 "Quiet mode (no output)"},

	{{0, 0, NULL,  0 }, NULL, false}
};

static int parse_u8(char *str, unsigned char *x)
{
	unsigned long z;

	z = strtoul(str, 0, 16);
	if (z > 0xff)
		return -1;

	if (x)
		*x = z;

	return 0;
}

static int parse_mac(char *str, unsigned char mac[ETH_ALEN])
{
	/* Parse a MAC address in this function and place the
	 * result in the mac array */
	if (parse_u8(str, &mac[0]) < 0)
		return -1;
	if (parse_u8(str + 3, &mac[1]) < 0)
		return -1;
	if (parse_u8(str + 6, &mac[2]) < 0)
		return -1;
	if (parse_u8(str + 9, &mac[3]) < 0)
		return -1;
	if (parse_u8(str + 12, &mac[4]) < 0)
		return -1;
	if (parse_u8(str + 15, &mac[5]) < 0)
		return -1;

	return 0;
}

static int dfa2map(struct DFA_state *dfa, int map_fd)
{
	struct generic_list state_list;
	struct DFA_state **state, *next_state;
	struct ids_inspect_map_key map_key;
	struct ids_inspect_map_value map_value;
	int i_state, n_state;

	/* Save all state in DFA into a generic list */
	create_generic_list(struct DFA_state *, &state_list);
	generic_list_push_back(&state_list, &dfa);
	DFA_traverse(dfa, &state_list);

	/* Encode each state */
	n_state = state_list.length;
	state = (struct DFA_state **) state_list.p_dat;
	for (i_state = 0; i_state < n_state; i_state++, state++) {
		(*state)->state_id = i_state;
	}

	/* Convert dfa to map */
	state = (struct DFA_state **) state_list.p_dat;
	for (i_state = 0; i_state < n_state; i_state++, state++) {
		int i_trans, n_trans = (*state)->n_transitions;
		for (i_trans = 0; i_trans < n_trans; i_trans++) {
			next_state = (*state)->trans[i_trans].to;
			map_key.padding = 0;
			map_key.state = (*state)->state_id;
			map_key.unit = (*state)->trans[i_trans].trans_char;
			map_value.state = next_state->state_id;
			map_value.final_state = next_state->is_acceptable;
			printf("map_key size: %ld, map_value size: %ld\n", sizeof(map_key), sizeof(map_value));
			printf("map_key - padding: %d, state: %d, unit: %c\n", map_key.padding, map_key.state, map_key.unit);
			printf("map_value - state: %d, final_state: %d\n", map_value.state, map_value.final_state);
			if (bpf_map_update_elem(map_fd, &map_key, &map_value, 0) < 0) {
				fprintf(stderr,
					"WARN: Failed to update bpf map file: err(%d):%s\n",
					errno, strerror(errno));
			}
		}
	}

	return 0;
}

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

const char *pin_basedir = "/sys/fs/bpf";

int main(int argc, char **argv)
{
	int i;
	int len;
	int map_fd;
	bool router, ids;
	char pin_dir[PATH_MAX];
	unsigned char src[ETH_ALEN];
	unsigned char dest[ETH_ALEN];

	router = false;
	ids = true;

	struct config cfg = {
		.ifindex = -1,
		.redirect_ifindex = -1,
	};

	/* Cmdline options can change progsec */
	parse_cmdline_args(argc, argv, long_options, &cfg, __doc__);
	if (cfg.redirect_ifindex > 0 && cfg.ifindex == -1) {
		fprintf(stderr, "ERR: required option --dev missing\n\n");
		usage(argv[0], __doc__, long_options, (argc == 1));
		return EXIT_FAIL_OPTION;
	}

	len = snprintf(pin_dir, PATH_MAX, "%s/%s", pin_basedir, cfg.ifname);
	if (len < 0) {
		fprintf(stderr, "ERR: creating pin dirname\n");
		return EXIT_FAIL_OPTION;
	}

	if (parse_mac(cfg.src_mac, src) < 0) {
		fprintf(stderr, "ERR: can't parse mac address %s\n", cfg.src_mac);
		return EXIT_FAIL_OPTION;
	}

	if (parse_mac(cfg.dest_mac, dest) < 0) {
		fprintf(stderr, "ERR: can't parse mac address %s\n", cfg.dest_mac);
		return EXIT_FAIL_OPTION;
	}

	printf("map dir: %s\n", pin_dir);

	if (ids) {
		/* Open the ids_inspect_map corresponding to the cfg.ifname interface */
		map_fd = open_bpf_map_file(pin_dir, ids_inspect_map_name, NULL);
		if (map_fd < 0) {
			return EXIT_FAIL_BPF;
		} else {
			char *re_string = "(dog)|(cat)";
			struct DFA_state *dfa;
			dfa = re2dfa(re_string);
			if (!dfa) {
				fprintf(stderr, "ERR: can't convert the RE to DFA\n");
				return EXIT_FAIL_RE2DFA;
			} else {
				if (dfa2map(dfa, map_fd) < 0) {
					fprintf(stderr, "ERR: can't convert the DFA to Map\n");
					return EXIT_FAIL_RE2DFA;
				}
			}
		}
	} else if (router) {
		/* Open the tx_port map corresponding to the cfg.ifname interface */
		map_fd = open_bpf_map_file(pin_dir, "tx_port", NULL);
		if (map_fd < 0) {
			return EXIT_FAIL_BPF;
		}
		for (i = 1; i < 5; ++i)
			if (bpf_map_update_elem(map_fd, &i, &i, 0) < 0) {
				fprintf(stderr,
					"WARN: Failed to update bpf map file: err(%d):%s\n",
					errno, strerror(errno));
			}
	}

	return EXIT_OK;
}

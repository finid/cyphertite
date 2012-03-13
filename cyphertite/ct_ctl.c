/*
 * Copyright (c) 2011, 2012 Conformal Systems LLC <info@conformal.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef NEED_LIBCLENS
#include <clens.h>
#endif

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>

#include <assl.h>
#include <clog.h>
#include <exude.h>
#include <shrink.h>
#include <xmlsd.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <ctutil.h>

#include "ct.h"
#include "ct_crypto.h"
#include "ct_ctl.h"
#include "ct_xml.h"
#include <ct_ext.h>


void cull(struct ct_cli_cmd *, int , char **);
void cpasswd(struct ct_cli_cmd *, int , char **);
void secrets_download(struct ct_cli_cmd *, int, char **);
void secrets_upload(struct ct_cli_cmd *, int, char **);
void secrets_generate(struct ct_cli_cmd *, int, char **);
void config_generate(struct ct_cli_cmd *, int, char **);

void
cpasswd(struct ct_cli_cmd *c, int argc, char **argv)
{
	char		old_crypto_secrets[PATH_MAX];
	char		old_configfile[PATH_MAX];
	char		prompt[1024], buf[1024], *p;
	char		answer[1024], answer2[1024];
	char		*crypto_passphrase = NULL;
	struct stat	sb;
	int		rv, write_crypto_passphrase = 0;
	int		crypto_passphrase_written = 0;
	uint8_t		ad[SHA512_DIGEST_LENGTH];
	char		b64d[128];
	FILE		*fr, *fw;

	snprintf(prompt, sizeof prompt, "This operation overwrites %s "
	    "and %s, continue? [yes]: ", ct_configfile, ct_crypto_secrets);
	if (ct_get_answer(prompt, "yes", "no", "yes", answer,
	    sizeof answer, 0) != 1)
		CFATALX("operation aborted");

	if (ct_crypto_secrets == NULL)
		CFATALX("Crypto not enabled");

	if (stat(ct_crypto_secrets, &sb) == -1)
		CFATALX("secrets file does not exist");

	if (ct_unlock_secrets(ct_crypto_passphrase, ct_crypto_secrets,
	    ct_crypto_key, sizeof(ct_crypto_key), ct_iv, sizeof (ct_iv)))
		CFATALX("can't unlock secrets");

	snprintf(prompt, sizeof prompt,
	    "Save crypto passphrase to configuration file? [yes]: ");
	rv = ct_get_answer(prompt, "yes", "no", "yes", answer,
	    sizeof answer, 0);
	if (rv == 1) {
		snprintf(prompt, sizeof prompt,
		    "Automatically generate crypto passphrase? [yes]: ");
		rv = ct_get_answer(prompt, "yes", "no", "yes", answer,
		    sizeof answer, 0);

		if (rv == 1) {
			arc4random_buf(answer2, sizeof answer2);
			ct_sha512((uint8_t *)answer2, ad, sizeof answer2);
			if (ct_base64_encode(CT_B64_ENCODE, ad,
			    sizeof ad, (uint8_t *)b64d, sizeof b64d))
				CFATALX("can't base64 encode "
				    "crypto passphrase");

			crypto_passphrase = strdup(b64d);
			if (crypto_passphrase == NULL)
				CFATALX("strdup");
		}

		bzero(answer, sizeof answer);
		bzero(answer2, sizeof answer2);
		write_crypto_passphrase = 1;
	}

	/* see if we had one autogenerated */
	if (crypto_passphrase == NULL) {
		if (ct_prompt_password("crypto passphrase: ", answer,
		    sizeof answer, answer2, sizeof answer2, 1))
			CFATALX("password");

		if (strlen(answer)) {
			crypto_passphrase = strdup(answer);
			if (crypto_passphrase == NULL)
				CFATALX("strdup");
		}

		bzero(answer, sizeof answer);
		bzero(answer2, sizeof answer2);
	}

	/* rename files */
	snprintf(old_configfile, sizeof old_configfile, "%s~", ct_configfile);
	if (rename(ct_configfile, old_configfile))
		CFATAL("%s", ct_configfile);

	snprintf(old_crypto_secrets, sizeof old_crypto_secrets, "%s~",
	    ct_crypto_secrets);
	if (rename(ct_crypto_secrets, old_crypto_secrets))
		CFATAL("%s", ct_crypto_secrets);

	/* rewrite files */
	fr = fopen(old_configfile, "r");
	if (fr == NULL)
		CFATAL("%s", old_configfile);
	fw = fopen(ct_configfile, "w");
	if (fw == NULL)
		CFATAL("%s", ct_configfile);

	while (fgets(buf, sizeof(buf), fr) != NULL) {
		if ((p = strchr(buf, '\n')) == NULL)
			CFATALX("input line too long.\n");
		*p = '\0';

		/* see what to do with crypto_passphrase */
		if (!strncmp(buf, "crypto_passphrase",
		    strlen("crypto_passphrase")) ||
		    !strncmp(buf, "crypto_password",
		    strlen("crypto_password"))) {
			if (write_crypto_passphrase) {
				fprintf(fw, "crypto_passphrase\t\t= %s\n",
				    crypto_passphrase);
				crypto_passphrase_written = 1;
			}
		} else
			fprintf(fw, "%s\n", buf);
	}
	if (crypto_passphrase_written == 0 && write_crypto_passphrase)
		fprintf(fw, "crypto_passphrase\t\t= %s\n", crypto_passphrase);

	fclose(fr);
	fclose(fw);

	ct_create_secrets(crypto_passphrase, ct_crypto_secrets, ct_crypto_key,
	    ct_iv);

	bzero(crypto_passphrase, strlen(crypto_passphrase));
	free(crypto_passphrase);
}

struct ct_cli_cmd	cmd_secrets[] = {
	{ "upload", NULL, 0, "", secrets_upload },
	{ "download", NULL, 0, "", secrets_download },
	{ "passwd", NULL, 0, "", cpasswd},
	{ "generate", NULL, 0, "", secrets_generate },
	{ NULL, NULL, 0, NULL, NULL, 0}
};

struct ct_cli_cmd	cmd_config[] = {
	{ "generate", NULL, 0, "", config_generate },
	{ NULL, NULL, 0, NULL, NULL, 0}
};

struct ct_cli_cmd	cmd_list[] = {
	{ "cull", NULL, 0, "", cull },
	{ "secrets", cmd_secrets, CLI_CMD_SUBCOMMAND, "<action> ...", NULL },
	{ "config", cmd_config, CLI_CMD_SUBCOMMAND, "<action> ...", NULL },
#ifdef CT_EXT_CTCTL_CMDS
	CT_EXT_CTCTL_CMDS
#endif
	{ NULL, NULL, 0, NULL, NULL }
};

void
ctctl_usage(void)
{
	fprintf(stderr, "%s [-D debugstring] [-F configfile] <action>\n",
	    __progname);
	exit(1);
}

int
ctctl_main(int argc, char *argv[])
{
	int			c;
	struct ct_cli_cmd	*cc = NULL;
	char			*configfile = NULL;
	char			*debugstring = NULL;
	uint64_t		debug_mask = 0;

	while ((c = getopt(argc, argv, "D:F:")) != -1) {
		switch (c) {
		case 'D':
			if (debugstring != NULL)
				CFATALX("only one -D argument is valid");
			ct_debug++;
			debugstring = optarg;
			break;
		case 'F':
			configfile = optarg;
			break;
		default:
			CWARNX("must specify action");
			ctctl_usage();
			/* NOTREACHED */
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (ct_debug) {
		cflags |= CLOG_F_DBGENABLE | CLOG_F_FILE | CLOG_F_FUNC |
		    CLOG_F_LINE | CLOG_F_DTIME;
		exude_enable(CT_LOG_EXUDE);
#if CT_ENABLE_THREADS
		exude_enable_threads();
#endif
		debug_mask |= ct_get_debugmask(debugstring);
	}

	/* please don't delete this line AGAIN! --mp */
	if (clog_set_flags(cflags))
		errx(1, "illegal clog flags");
	clog_set_mask(debug_mask);

	/* We can allocate these now that we've decided if we need exude */
	if (configfile)
		ct_configfile = e_strdup(configfile);

	/* load config XXX ick... unless we're generating one. */
	if (!(argc == 2 && strcmp(argv[0], "config") == 0 && strcmp(argv[1],
	    "generate") == 0) && ct_load_config(settings))
		CFATALX("config file not found.  Use the -F option to "
		    "specify its path or run \"%s config generate\" to create "
		    "one.", __progname);

	if ((cc = ct_cli_validate(cmd_list, &argc, &argv)) == NULL)
		ct_cli_usage(cmd_list, NULL);

	ct_cli_execute(cc, &argc, &argv);

	exude_cleanup();

	return (0);
}

/* cull - sha deletion from server operation */
void
cull(struct ct_cli_cmd *c, int argc, char **argv)
{
	struct ct_global_state	*state;
	int			 need_secrets, ret;

	/* XXX */

	ct_prompt_for_login_password();

	need_secrets = 1;

	state = ct_init(1, need_secrets, 0);

	ct_cull_kick(state);
	ct_wakeup_file();

	ret = ct_event_dispatch();
	if (ret != 0)
		CWARNX("event_dispatch returned, %d %s", errno,
		    strerror(errno));

	ct_cleanup(state);
	e_check_memory();
}

void
secrets_upload(struct ct_cli_cmd *c, int argc, char **argv)
{
	ct_upload_secrets_file();
}

void
secrets_download(struct ct_cli_cmd *c, int argc, char **argv)
{
	ct_download_secrets_file();
}

void
secrets_generate(struct ct_cli_cmd *c, int argc, char **argv)
{
	struct stat	sb;

	if (stat(ct_crypto_secrets, &sb) != -1)
		CFATALX("A crypto secrets file already exists!\n"
		    "Please check if it is valid before deleting.");
	CWARNX("Generating crypto secrets file...");
	if (ct_create_secrets(ct_crypto_passphrase, ct_crypto_secrets,
	    NULL, NULL))
		CFATALX("can't create secrets");

	if (ct_secrets_upload != 0)
		ct_upload_secrets_file();
}

void
config_generate(struct ct_cli_cmd *c, int argc, char **argv)
{
	ct_create_config();
}

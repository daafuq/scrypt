/*-
 * Copyright 2009 Colin Percival
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "platform.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "getopt.h"
#include "humansize.h"
#include "insecure_memzero.h"
#include "parsenum.h"
#include "readpass.h"
#include "scryptenc.h"
#include "warnp.h"

/* How should we get the passphrase? */
enum passphrase_entry {
	PASSPHRASE_UNSET,
	PASSPHRASE_TTY_STDIN,
	PASSPHRASE_STDIN_ONCE,
	PASSPHRASE_TTY_ONCE,
	PASSPHRASE_ENV,
	PASSPHRASE_FILE,
};

static void
usage(void)
{

	fprintf(stderr,
	    "usage: scrypt {enc | dec | info} [-f] [-M maxmem]"
	    " [-m maxmemfrac] [-P]\n"
	    "              [-t maxtime] [-v] [--passphrase method:arg]"
	    " infile [outfile]\n"
	    "       scrypt --version\n");
	exit(1);
}

static int
parse_passphrase_arg(const char * arg,
    enum passphrase_entry * passphrase_entry_p, const char ** passphrase_arg_p)
{
	const char * p;

	/* Find the separator in "method:arg", or fail if there isn't one. */
	if ((p = strchr(arg, ':')) == NULL)
		goto err1;

	/* Extract the "arg" part. */
	*passphrase_arg_p = &p[1];

	/* Parse the "method". */
	if (strncmp(arg, "dev:", 4) == 0) {
		if (strcmp(*passphrase_arg_p, "tty-stdin") == 0) {
			*passphrase_entry_p = PASSPHRASE_TTY_STDIN;
			goto success;
		}
		else if (strcmp(*passphrase_arg_p, "stdin-once") == 0) {
			*passphrase_entry_p = PASSPHRASE_STDIN_ONCE;
			goto success;
		}
		else if (strcmp(*passphrase_arg_p, "tty-once") == 0) {
			*passphrase_entry_p = PASSPHRASE_TTY_ONCE;
			goto success;
		}
	}
	if (strncmp(optarg, "env:", 4) == 0) {
		*passphrase_entry_p = PASSPHRASE_ENV;
		goto success;
	}
	if (strncmp(optarg, "file:", 5) == 0) {
		*passphrase_entry_p = PASSPHRASE_FILE;
		goto success;
	}

err1:
	warn0("Invalid option: --passphrase %s", arg);

	/* Failure! */
	return (-1);

success:
	/* Success! */
	return (0);
}

int
main(int argc, char *argv[])
{
	FILE * infile;
	FILE * outfile = stdout;
	int dec = 0;
	int info = 0;
	int force_resources = 0;
	uint64_t maxmem64;
	struct scryptenc_params params = {0, 0.5, 300.0, 0, 0, 0};
	const char * ch;
	const char * infilename;
	const char * outfilename;
	char * passwd;
	int rc;
	int verbose = 0;
	struct scryptdec_file_cookie * C = NULL;
	enum passphrase_entry passphrase_entry = PASSPHRASE_UNSET;
	const char * passphrase_arg;
	const char * passwd_env;

	WARNP_INIT;

	/* We should have "enc", "dec", or "info" first. */
	if (argc < 2)
		usage();
	if (strcmp(argv[1], "enc") == 0) {
		params.maxmem = 0;
		params.maxmemfrac = 0.125;
		params.maxtime = 5.0;
	} else if (strcmp(argv[1], "dec") == 0) {
		dec = 1;
	} else if (strcmp(argv[1], "info") == 0) {
		info = 1;
	} else if (strcmp(argv[1], "--version") == 0) {
		fprintf(stdout, "scrypt %s\n", PACKAGE_VERSION);
		exit(0);
	} else {
		warn0("First argument must be 'enc', 'dec', or 'info'.");
		usage();
	}
	argc--;
	argv++;

	/* Parse arguments. */
	while ((ch = GETOPT(argc, argv)) != NULL) {
		GETOPT_SWITCH(ch) {
		GETOPT_OPT("-f"):
			force_resources = 1;
			break;
		GETOPT_OPTARG("-l"):
			if (PARSENUM(&params.logN, optarg, 10, 2048)) {
				warnp("Invalid option: %s %s", ch, optarg);
				exit(1);
			}
			break;
		GETOPT_OPTARG("-r"):
			if (PARSENUM(&params.r, optarg, 1, 128)) {
				warnp("Invalid option: %s %s", ch, optarg);
				exit(1);
			}
			break;
		GETOPT_OPTARG("-p"):
			if (PARSENUM(&params.p, optarg, 1, 128)) {
				warnp("Invalid option: %s %s", ch, optarg);
				exit(1);
			}
			break;
		GETOPT_OPTARG("-M"):
			if (humansize_parse(optarg, &maxmem64)) {
				warn0("Could not parse the parameter to -M.");
				exit(1);
			}
			if (maxmem64 > SIZE_MAX) {
				warn0("The parameter to -M is too large.");
				exit(1);
			}
			params.maxmem = (size_t)maxmem64;
			break;
		GETOPT_OPTARG("-m"):
			if (PARSENUM(&params.maxmemfrac, optarg, 0, 1)) {
				warnp("Invalid option: -m %s", optarg);
				exit(1);
			}
			break;
		GETOPT_OPTARG("--passphrase"):
			if (passphrase_entry != PASSPHRASE_UNSET) {
				warn0("You can only enter one --passphrase or"
				    " -P argument");
				exit(1);
			}

			/* Parse "method:arg" optarg. */
			if (parse_passphrase_arg(optarg, &passphrase_entry,
			    &passphrase_arg))
				exit(1);
			break;
		GETOPT_OPTARG("-t"):
			if (PARSENUM(&params.maxtime, optarg, 0, INFINITY)) {
				warnp("Invalid option: -t %s", optarg);
				exit(1);
			}
			break;
		GETOPT_OPT("-v"):
			verbose = 1;
			break;
		GETOPT_OPT("-P"):
			if (passphrase_entry != PASSPHRASE_UNSET) {
				warn0("You can only enter one --passphrase or"
				    " -P argument");
				exit(1);
			}
			passphrase_entry = PASSPHRASE_STDIN_ONCE;
			break;
		GETOPT_MISSING_ARG:
			warn0("Missing argument to %s", ch);
			usage();
		GETOPT_DEFAULT:
			warn0("illegal option -- %s", ch);
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	/* We must have one or two parameters left. */
	if ((argc < 1) || (argc > 2))
		usage();

	/* Set the input filename. */
	if (strcmp(argv[0], "-"))
		infilename = argv[0];
	else
		infilename = NULL;

	/* Set the output filename. */
	if (argc > 1)
		outfilename = argv[1];
	else
		outfilename = NULL;

	/* Set the default passphrase entry method. */
	if (passphrase_entry == PASSPHRASE_UNSET)
		passphrase_entry = PASSPHRASE_TTY_STDIN;

	/* If the input isn't stdin, open the file. */
	if (infilename != NULL) {
		if ((infile = fopen(infilename, "rb")) == NULL) {
			warnp("Cannot open input file: %s", infilename);
			goto err0;
		}
	} else {
		infile = stdin;

		/* Error if given incompatible options. */
		if (passphrase_entry == PASSPHRASE_STDIN_ONCE) {
			warn0("Cannot read both passphrase and input file"
			    " from standard input");
			goto err0;
		}
	}

	/* User selected 'info' mode. */
	if (info) {
		/* Print the encryption parameters used for the file. */
		rc = scryptdec_file_printparams(infile);

		/* Clean up. */
		if (infile != stdin)
			fclose(infile);

		/* Finished! */
		goto done;
	}

	/* Get the password. */
	switch (passphrase_entry) {
	case PASSPHRASE_TTY_STDIN:
		/* Read passphrase, prompting only once if decrypting. */
		if (readpass(&passwd, "Please enter passphrase",
		    (dec) ? NULL : "Please confirm passphrase", 1))
			goto err1;
		break;
	case PASSPHRASE_STDIN_ONCE:
		/* Read passphrase, prompting only once, from stdin only. */
		if (readpass(&passwd, "Please enter passphrase", NULL, 0))
			goto err1;
		break;
	case PASSPHRASE_TTY_ONCE:
		/* Read passphrase, prompting only once, from tty only. */
		if (readpass(&passwd, "Please enter passphrase", NULL, 2))
			goto err1;
		break;
	case PASSPHRASE_ENV:
		/* We're not allowed to modify the output of getenv(). */
		if ((passwd_env = getenv(passphrase_arg)) == NULL) {
			warn0("Failed to read from ${%s}", passphrase_arg);
			goto err1;
		}

		/* This allows us to use the same insecure_zero() logic. */
		if ((passwd = strdup(passwd_env)) == NULL) {
			warnp("Out of memory");
			goto err1;
		}
		break;
	case PASSPHRASE_FILE:
		if (readpass_file(&passwd, passphrase_arg))
			goto err1;
		break;
	case PASSPHRASE_UNSET:
		warn0("Programming error: passphrase_entry is not set");
		goto err1;
	}

	/*-
	 * If we're decrypting, open the input file and process its header;
	 * doing this here allows us to abort without creating an output
	 * file if the input file does not have a valid scrypt header or if
	 * we have the wrong passphrase.
	 *
	 * If successful, we get back a cookie containing the decryption
	 * parameters (which we'll use after we open the output file).
	 */
	if (dec) {
		if ((rc = scryptdec_file_prep(infile, (uint8_t *)passwd,
		    strlen(passwd), &params, verbose, force_resources,
		    &C)) != 0) {
			goto cleanup;
		}
	}

	/* If we have an output file, open it. */
	if (outfilename != NULL) {
		if ((outfile = fopen(outfilename, "wb")) == NULL) {
			warnp("Cannot open output file: %s", outfilename);
			goto err2;
		}
	}

	/* Encrypt or decrypt. */
	if (dec)
		rc = scryptdec_file_copy(C, outfile);
	else
		rc = scryptenc_file(infile, outfile, (uint8_t *)passwd,
		    strlen(passwd), &params, verbose);

cleanup:
	/* Free the decryption cookie, if any. */
	scryptdec_file_cookie_free(C);

	/* Zero and free the password. */
	insecure_memzero(passwd, strlen(passwd));
	free(passwd);

	/* Close any files we opened. */
	if (infile != stdin)
		fclose(infile);
	if (outfile != stdout)
		fclose(outfile);

done:
	/* If we failed, print the right error message and exit. */
	if (rc != SCRYPT_OK) {
		switch (rc) {
		case SCRYPT_ELIMIT:
			warnp("Error determining amount of available memory");
			break;
		case SCRYPT_ECLOCK:
			warnp("Error reading clocks");
			break;
		case SCRYPT_EKEY:
			warnp("Error computing derived key");
			break;
		case SCRYPT_ESALT:
			warnp("Error reading salt");
			break;
		case SCRYPT_EOPENSSL:
			warnp("OpenSSL error");
			break;
		case SCRYPT_ENOMEM:
			warnp("Error allocating memory");
			break;
		case SCRYPT_EINVAL:
			warn0("Input is not valid scrypt-encrypted block");
			break;
		case SCRYPT_EVERSION:
			warn0("Unrecognized scrypt format version");
			break;
		case SCRYPT_ETOOBIG:
			warn0("Decrypting file would require too much memory");
			break;
		case SCRYPT_ETOOSLOW:
			warn0("Decrypting file would take too much CPU time");
			break;
		case SCRYPT_EPASS:
			warn0("Passphrase is incorrect");
			break;
		case SCRYPT_EWRFILE:
			warnp("Error writing file: %s",
			    (outfilename != NULL) ? outfilename
			    : "standard output");
			break;
		case SCRYPT_ERDFILE:
			warnp("Error reading file: %s",
			    (infilename != NULL) ? infilename
			    : "standard input");
			break;
		case SCRYPT_EPARAM:
			warn0("Error in the manually specified parameters");
			break;
		}
		goto err0;
	}

	/* Success! */
	return (0);

err2:
	scryptdec_file_cookie_free(C);
	insecure_memzero(passwd, strlen(passwd));
	free(passwd);
err1:
	if (infile != stdin)
		fclose(infile);
err0:
	/* Failure! */
	exit(1);
}

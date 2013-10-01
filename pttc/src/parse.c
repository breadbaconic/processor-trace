/*
 * Copyright (c) 2013, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "errcode.h"
#include "parse.h"
#include "util.h"

#include "pt_encode.h"
#include "pt_error.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *pt_suffix = ".pt";
const char *exp_suffix = ".exp";

enum {
	pd_len = 1024
};

/* Deallocates the memory used by @p, closes all files, clears and
 * zeroes the fields.
 */
static void p_free(struct parser *p)
{
	if (!p)
		return;

	yasm_free(p->y);
	pd_free(p->pd);
	free(p->ptfilename);

	free(p);
}

/* Initializes @p with @pttfile and @conf.
 *
 * Returns 0 on success; a negative enum errcode otherwise.
 * Returns -err_internal if @p is the NULL pointer.
 */
static struct parser *p_alloc(const char *pttfile, const struct pt_config *conf)
{
	size_t n;
	struct parser *p;

	if (!conf)
		return NULL;

	if (!pttfile)
		return NULL;

	p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;

	p->y = yasm_alloc(pttfile);
	if (!p->y)
		goto error;

	n = strlen(p->y->fileroot) + 1;

	p->ptfilename = malloc(n+strlen(pt_suffix));
	if (!p->ptfilename)
		goto error;

	strcpy(p->ptfilename, p->y->fileroot);
	strcat(p->ptfilename, pt_suffix);

	p->pd = pd_alloc(pd_len);
	if (!p->pd)
		goto error;

	p->pt_labels = l_alloc();
	if (!p->pt_labels)
		goto error;

	p->conf = conf;

	return p;

error:
	p_free(p);
	return NULL;
}

/* Generates an .exp filename following the scheme:
 *	<fileroot>[-<extra>].exp
 */
static char *expfilename(struct parser *p, const char *extra)
{
	char *filename;
	size_t n;

	if (!extra)
		extra = "";

	/* the extra string is appended to fileroot with a -.  */
	n = strlen(extra);
	if (n > 0)
		n += 1;
	n += strlen(p->y->fileroot);
	n += strlen(exp_suffix);
	/* trailing null character.  */
	n += 1;

	filename = malloc(n);
	if (!filename)
		return NULL;

	strcpy(filename, p->y->fileroot);
	if (*extra != '\0') {
		strcat(filename, "-");
		strcat(filename, extra);
	}
	strcat(filename, exp_suffix);
	return filename;
}

/* Returns true if @c is part of a label; false otherwise.  */
static int islabelchar(int c)
{
	if (isalnum(c))
		return 1;

	switch (c) {
	case '_':
		return 1;
	}

	return 0;
}

/* Generates the content of the .exp file by printing all lines with
 * everything up to and including the first comment semicolon removed.
 *
 * Returns 0 on success; a negative enum errcode otherwise.
 * Returns -err_internal if @p is the NULL pointer.
 * Returns -err_file_write if the .exp file could not be fully written.
 */
static int p_gen_expfile(struct parser *p)
{
	int errcode;
	enum { slen = 1024 };
	char s[slen];
	struct pt_directive *pd;
	char *filename;
	FILE *f;

	if (bug_on(!p))
		return -err_internal;

	pd = p->pd;

	/* the directive in the current line must be the .exp directive.  */
	errcode = yasm_pd_parse(p->y, pd);
	if (bug_on(errcode < 0))
		return -err_internal;

	if (bug_on(strcmp(pd->name, ".exp") != 0))
		return -err_internal;

	filename = expfilename(p, pd->payload);
	if (!filename)
		return -err_no_mem;
	f = fopen(filename, "w");
	if (!f) {
		free(filename);
		return -err_file_open;
	}

	for (;;) {
		int i;
		char *line, *comment;

		errcode = yasm_next_line(p->y, s, slen);
		if (errcode < 0)
			break;

		errcode = yasm_pd_parse(p->y, pd);
		if (errcode < 0 && errcode != -err_no_directive)
			break;

		if (errcode == 0 && strcmp(pd->name, ".exp") == 0) {
			fclose(f);
			printf("%s\n", filename);
			free(filename);
			filename = expfilename(p, pd->payload);
			if (!filename)
				return -err_no_mem;
			f = fopen(filename, "w");
			if (!f) {
				free(filename);
				return -err_file_open;
			}
			continue;
		}

		line = strchr(s, ';');
		if (!line)
			continue;

		line += 1;

		comment = strchr(line, '#');
		if (comment)
			*comment = '\0';

		/* remove trailing spaces.  */
		for (i = (int) strlen(line)-1; i >= 0 && isspace(line[i]); i--)
			line[i] = '\0';

		for (;;) {
			char *tmp, label[256];
			uint64_t addr;
			int i, zero_padding, qmark_padding, qmark_size, status;

			zero_padding = 0;
			qmark_padding = 0;
			qmark_size = 0;

			/* find the label character in the string.
			 * if there is no label character, we just print
			 * the rest of the line and end.
			 */
			tmp = strchr(line, '%');
			if (!tmp) {
				if (fprintf(f, "%s", line) < 0) {
					errcode = -err_file_write;
					goto error;
				}
				break;
			}

			/* make the label character a null byte and
			 * print the first portion, which does not
			 * belong to the label into the file.
			 */
			*tmp = '\0';
			if (fprintf(f, "%s", line) < 0) {
				errcode = -err_file_write;
				goto error;
			}

			/* test if there is a valid label name after the %.  */
			line = tmp+1;
			if (*line == '\0' || isspace(*line)) {
				errcode = -err_no_label;
				goto error;
			}

			/* check if zero padding is requested.  */
			if (*line == '0') {
				zero_padding = 1;
				line += 1;
			}
			/* chek if ? padding is requested.  */
			else if (*line == '?') {
				qmark_padding = 1;
				zero_padding = 1;
				qmark_size = 0;
				line += 1;
			}

			/* advance i to the first non alpha-numeric
			 * character. all characters everything from
			 * line[0] to line[i-1] belongs to the label
			 * name.
			 */
			for (i = 0; islabelchar(line[i]); i++)
				;

			if (i > 255) {
				errcode = -err_label_name;
				goto error;
			}
			strncpy(label, line, i);
			label[i] = '\0';

			/* advance to next character.  */
			line = &line[i];

			/* lookup the label name and print it to the
			 * output file.
			 */
			errcode = yasm_lookup_label(p->y, &addr, label);
			if (errcode < 0) {
				errcode = l_lookup(p->pt_labels, &addr, label);
				if (errcode < 0)
					goto error;

				if (zero_padding)
					status = fprintf(f, "%016" PRIx64, addr);
				else
					status = fprintf(f, "%" PRIx64, addr);

				if (status < 0) {
					errcode = -err_file_write;
					goto error;
				}

				continue;
			}

			/* check if masking is requested.  */
			if (*line == '.') {
				char *endptr;
				long int n;

				line += 1;

				n = strtol(line, &endptr, 0);
				/* check if strtol made progress and
				 * stops on a space or null byte.
				 * otherwise the int could not be
				 * parsed.
				 */
				if (line == endptr ||
				    (*endptr != '\0' && !isspace(*endptr)
				     && !ispunct(*endptr))) {
					errcode = -err_parse_int;
					goto error;
				}
				addr &= (1 << (n << 3)) - 1;
				line = endptr;

				qmark_size = 8 - n;
			}

			if (qmark_padding) {
				int i;

				status = fprintf(f, "0x");
				if (status < 0) {
					errcode = -err_file_write;
					goto error;
				}

				for (i = 0; i < qmark_size; ++i) {
					status = fprintf(f, "??");
					if (status < 0) {
						errcode = -err_file_write;
						goto error;
					}
				}

				for (; i < 8; ++i) {
					uint8_t byte;

					byte = (uint8_t)(addr >> ((7 - i) * 8));

					status = fprintf(f, "%02" PRIx8, byte);
					if (status < 0) {
						errcode = -err_file_write;
						goto error;
					}
				}
			} else if (zero_padding)
				status = fprintf(f, "0x%016" PRIx64, addr);
			else
				status = fprintf(f, "0x%" PRIx64, addr);

			if (status < 0) {
				errcode = -err_file_write;
				goto error;
			}

		}

		if (fprintf(f, "\n") < 0) {
			errcode = -err_file_write;
			goto error;
		}
	}

error:

	fclose(f);
	if (errcode < 0 && errcode != -err_out_of_range) {
		fprintf(stderr, "fatal: %s could not be created:\n", filename);
		yasm_print_err(p->y, "", errcode);
		remove(filename);
	} else
		printf("%s\n", filename);
	free(filename);

	/* If there are no lines left, we are done.  */
	if (errcode == -err_out_of_range)
		return 0;

	return errcode;
}

static void p_close_files(struct parser *p)
{
	if (p->ptfile) {
		fclose(p->ptfile);
		p->ptfile = NULL;
	}
}

static int p_open_files(struct parser *p)
{
	p->ptfile = fopen(p->ptfilename, "w");
	if (!p->ptfile) {
		fprintf(stderr, "open %s failed\n", p->ptfilename);
		goto error;
	}
	return 0;

error:
	p_close_files(p);
	return -err_file_open;
}

/* Processes the current directive.
 * If the encoder returns an error, a message including current file and
 * line number together with the pt error string is printed on stderr.
 *
 * Returns 0 on success; a negative enum errcode otherwise.
 * Returns -err_internal if @p or @e is the NULL pointer.
 * Returns -err_parse_missing_directive if there was a pt directive marker,
 * but no directive.
 * Returns -stop_process if the .exp directive was encountered.
 * Returns -err_pt_lib if the pt encoder returned an error.
 * Returns -err_parse if a general parsing error was encountered.
 * Returns -err_parse_unknown_directive if there was an unknown pt directive.
 */
static int p_process(struct parser *p, struct pt_encoder *e)
{
	int bytes_written;
	int errcode;
	char *directive, *payload, *pt_label_name, *tmp;
	struct pt_directive *pd;

	if (bug_on(!p))
		return -err_internal;

	if (bug_on(!e))
		return -err_internal;

	pd = p->pd;
	if (!pd)
		return -err_internal;

	directive = pd->name;
	payload = pd->payload;

	pt_label_name = NULL;
	bytes_written = 0;
	errcode = 0;

	/* find a label name.  */
	tmp = strchr(directive, ':');
	if (tmp) {
		uint64_t x;

		pt_label_name = directive;
		directive = tmp+1;
		*tmp = '\0';

		/* ignore whitespace between label and directive. */
		while (isspace(*directive))
			directive += 1;

		/* if we can lookup a yasm label with the same name, the
		 * current pt directive label is invalid.  */
		errcode = yasm_lookup_label(p->y, &x, pt_label_name);
		if (errcode == 0)
			errcode = -err_label_not_unique;

		if (errcode != -err_no_label)
			return yasm_print_err(p->y, "label lookup",
					      errcode);

		/* if we can lookup a pt directive label with the same
		 * name, the current pt directive label is invalid.  */
		errcode = l_lookup(p->pt_labels, &x, pt_label_name);
		if (errcode == 0)
			errcode = -err_label_not_unique;

		if (errcode != -err_no_label)
			return yasm_print_err(p->y, "label lookup",
					      -err_label_not_unique);
	}

	/* now try to match the directive string and call the
	 * corresponding function that parses the payload and emits an
	 * according packet.
	 */
	if (strcmp(directive, "") == 0)
		return yasm_print_err(p->y, "invalid syntax",
				      -err_parse_missing_directive);
	else if (strcmp(directive, ".exp") == 0) {
		/* this is the end of processing pt directives, so we
		 * add a p_last label to the pt directive labels.
		 */
		errcode = l_append(p->pt_labels, "eos", p->pt_bytes_written);
		if (errcode < 0)
			return yasm_print_err(p->y, "append label", errcode);

		return -stop_process;
	}

	if (strcmp(directive, "psb") == 0) {
		errcode = parse_empty(payload);
		if (errcode < 0) {
			yasm_print_err(p->y, "psb: parsing failed", errcode);
			goto error;
		}
		bytes_written = pt_encode_psb(e);
	} else if (strcmp(directive, "psbend") == 0) {
		errcode = parse_empty(payload);
		if (errcode < 0) {
			yasm_print_err(p->y, "psbend: parsing failed", errcode);
			goto error;
		}
		bytes_written = pt_encode_psbend(e);
	} else if (strcmp(directive, "pad") == 0) {
		errcode = parse_empty(payload);
		if (errcode < 0) {
			yasm_print_err(p->y, "pad: parsing failed", errcode);
			goto error;
		}
		bytes_written = pt_encode_pad(e);
	} else if (strcmp(directive, "ovf") == 0) {
		errcode = parse_empty(payload);
		if (errcode < 0) {
			yasm_print_err(p->y, "ovf: parsing failed", errcode);
			goto error;
		}
		bytes_written = pt_encode_ovf(e);
	} else if (strcmp(directive, "tnt") == 0) {
		uint64_t tnt;
		size_t size;

		errcode = parse_tnt(&tnt, &size, payload);
		if (errcode < 0) {
			yasm_print_err(p->y, "tnt: parsing failed", errcode);
			goto error;
		}
		bytes_written = pt_encode_tnt_8(e, (uint8_t)tnt, (int)size);
	} else if (strcmp(directive, "tnt64") == 0) {
		uint64_t tnt;
		size_t size;

		errcode = parse_tnt(&tnt, &size, payload);
		if (errcode < 0) {
			yasm_print_err(p->y, "tnt64: parsing failed", errcode);
			goto error;
		}
		bytes_written = pt_encode_tnt_64(e, tnt, (int)size);
	} else if (strcmp(directive, "tip") == 0) {
		uint64_t ip;
		uint8_t ipc;

		errcode = parse_ip(p, &ip, &ipc, payload);
		if (errcode < 0) {
			yasm_print_err(p->y, "tip: parsing failed", errcode);
			goto error;
		}
		bytes_written = pt_encode_tip(e, ip, ipc);
	} else if (strcmp(directive, "tip.pge") == 0) {
		uint64_t ip;
		uint8_t ipc;

		errcode = parse_ip(p, &ip, &ipc, payload);
		if (errcode < 0) {
			yasm_print_err(p->y, "tip.pge: parsing failed",
				       errcode);
			goto error;
		}
		bytes_written = pt_encode_tip_pge(e, ip, ipc);
	} else if (strcmp(directive, "tip.pgd") == 0) {
		uint64_t ip;
		uint8_t ipc;

		errcode = parse_ip(p, &ip, &ipc, payload);
		if (errcode < 0) {
			yasm_print_err(p->y, "tip.pgd: parsing failed",
				       errcode);
			goto error;
		}
		bytes_written = pt_encode_tip_pgd(e, ip, ipc);
	} else if (strcmp(directive, "fup") == 0) {
		uint64_t ip;
		uint8_t ipc;

		errcode = parse_ip(p, &ip, &ipc, payload);
		if (errcode < 0) {
			yasm_print_err(p->y, "fup: parsing failed", errcode);
			goto error;
		}
		bytes_written = pt_encode_fup(e, ip, ipc);
	} else if (strcmp(directive, "mode.exec") == 0) {
		enum pt_exec_mode em;

		if (strcmp(payload, "16bit") == 0)
			em = ptem_16bit;
		else if (strcmp(payload, "64bit") == 0)
			em = ptem_64bit;
		else if (strcmp(payload, "32bit") == 0)
			em = ptem_32bit;
		else {
			errcode = yasm_print_err(p->y,
						 "mode.exec: argument must be one of \"16bit\", \"64bit\" or \"32bit\"",
						 -err_parse);
			goto error;
		}
		bytes_written = pt_encode_mode_exec(e, em);
	} else if (strcmp(directive, "mode.tsx") == 0) {
		uint8_t tm;

		if (strcmp(payload, "begin") == 0)
			tm = 1;
		else if (strcmp(payload, "abort") == 0)
			tm = 2;
		else if (strcmp(payload, "commit") == 0)
			tm = 0;
		else {
			errcode = yasm_print_err(p->y,
						 "mode.tsx: argument must be one of \"begin\", \"abort\" or \"commit\"",
						 -err_parse);
			goto error;
		}
		bytes_written = pt_encode_mode_tsx(e, tm);
	} else if (strcmp(directive, "pip") == 0) {
		uint64_t cr3;

		errcode = parse_uint64(&cr3, payload);
		if (errcode < 0) {
			yasm_print_err(p->y, "pip: parsing failed", errcode);
			goto error;
		}
		bytes_written = pt_encode_pip(e, cr3);
	} else if (strcmp(directive, "tsc") == 0) {
		uint64_t tsc;

		errcode = parse_uint64(&tsc, payload);
		if (errcode < 0) {
			yasm_print_err(p->y, "tsc: parsing failed", errcode);
			goto error;
		}
		bytes_written = pt_encode_tsc(e, tsc);
	} else if (strcmp(directive, "cbr") == 0) {
		uint8_t cbr;

		errcode = parse_uint8(&cbr, payload);
		if (errcode < 0) {
			yasm_print_err(p->y, "cbr: parsing cbr failed",
				       errcode);
			goto error;
		}
		bytes_written = pt_encode_cbr(e, cbr);
	} else {
		errcode = yasm_print_err(p->y, "invalid syntax",
					 -err_parse_unknown_directive);
		goto error;
	}

	if (bytes_written < 0) {
		const char *errstr, *format;
		char *msg;
		size_t n;

		errstr = pt_errstr(pt_errcode(bytes_written));
		format = "encoder error in directive %s (status %s)";
		/* the length of format includes the "%s" (-2)
		 * characters, we add errstr (+-0) and then we need
		 * space for a terminating null-byte (+1).
		 */
		n = strlen(format)-4 + strlen(directive) + strlen(errstr) + 1;

		msg = malloc(n);
		if (!msg)
			errcode = yasm_print_err(p->y,
				       "encoder error not enough memory to show error code",
				       -err_pt_lib);
		else {
			sprintf(msg, format, directive, errstr);
			errcode = yasm_print_err(p->y, msg, -err_pt_lib);
			free(msg);
		}
	} else {
		if (pt_label_name) {
			errcode = l_append(p->pt_labels, pt_label_name,
					   p->pt_bytes_written);
			if (errcode < 0)
				goto error;
		}
		p->pt_bytes_written += bytes_written;
	}

error:
	if (errcode < 0)
		bytes_written = errcode;
	return bytes_written;
}

/* Starts the parsing process.
 *
 * Returns 0 on success; a negative enum errcode otherwise.
 * Returns -err_pt_lib if the pt encoder could not be initialized.
 * Returns -err_file_write if the .pt or .exp file could not be fully
 * written.
 */
int p_start(struct parser *p)
{
	int errcode;

	if (bug_on(!p))
		return -err_internal;

	errcode = yasm_parse(p->y);
	if (errcode < 0)
		return errcode;

	for (;;) {
		int bytes_written;
		struct pt_encoder e;

		errcode = yasm_next_pt_directive(p->y, p->pd);
		if (errcode < 0)
			break;

		errcode = pt_init_encoder(&e, p->conf);
		if (errcode < 0) {
			fprintf(stderr, "pt_init_encoder failed with %d: %s\n",
				errcode, pt_errstr(pt_errcode(errcode)));
			errcode = -err_pt_lib;
			break;
		}

		bytes_written = p_process(p, &e);
		if (bytes_written == -stop_process) {
			errcode = p_gen_expfile(p);
			break;
		}
		if (bytes_written < 0) {
			errcode = bytes_written;
			break;
		}
		if (fwrite(p->conf->begin, 1, bytes_written, p->ptfile)
		    != (size_t)bytes_written) {
			fprintf(stderr, "write %s failed", p->ptfilename);
			errcode = -err_file_write;
			break;
		}
	}

	/* If there is no directive left, there's nothing more to do.  */
	if (errcode == -err_no_directive)
		return 0;

	return errcode;
}

int parse(const char *pttfile, const struct pt_config *conf)
{
	int errcode;
	struct parser *p;

	p = p_alloc(pttfile, conf);
	if (!p)
		return -err_no_mem;

	errcode = p_open_files(p);
	if (errcode < 0)
		goto error;

	errcode = p_start(p);
	p_close_files(p);

error:
	p_free(p);
	return errcode;
}

int parse_empty(char *payload)
{
	if (!payload)
		return 0;

	strtok(payload, " ");
	if (!payload || *payload == '\0')
		return 0;

	return -err_parse_trailing_tokens;
}

int parse_tnt(uint64_t *tnt, size_t *size, char *payload)
{
	char c;

	if (bug_on(!size))
		return -err_internal;

	if (bug_on(!tnt))
		return -err_internal;

	*size = *tnt = 0;

	if (!payload)
		return 0;

	while (*payload != '\0') {
		c = *payload;
		payload++;
		if (isspace(c) || c == '.')
			continue;
		*size += 1;
		*tnt <<= 1;
		switch (c) {
		case 'n':
			break;
		case 't':
			*tnt |= 1;
			break;
		default:
			return -err_parse_unknown_char;
		}
	}

	return 0;
}

int parse_ip(struct parser *p, uint64_t *ip, uint8_t *ipc, char *payload)
{
	char *endptr;

	if (bug_on(!ip))
		return -err_internal;

	if (bug_on(!ipc))
		return -err_internal;

	*ipc = 0;
	*ip = 0;

	payload = strtok(payload, " :");
	if (!payload || *payload == '\0')
		return -err_parse_no_args;

	*ipc = (uint8_t) strtol(payload, &endptr, 0);
	if (payload == endptr || *endptr != '\0')
		return -err_parse_int;

	payload = strtok(NULL, " :");
	if (!payload)
		return -err_parse_ip_missing;

	/* can be resolved to a label?  */
	if (*payload == '%') {
		int errcode;

		if (!p)
			return -err_internal;

		errcode = yasm_lookup_label(p->y, ip, payload + 1);
		if (errcode < 0)
			return errcode;
	} else {
		/* can be parsed as address?  */
		*ip = strtoll(payload, &endptr, 0);

		if ((payload == endptr) || (*endptr != '\0'))
			return -err_parse_int;
	}

	/* no more tokens left.  */
	payload = strtok(NULL, " ");
	if (payload)
		return -err_parse_trailing_tokens;

	return 0;
}

int parse_uint64(uint64_t *x, char *payload)
{
	char *endptr;

	if (bug_on(!x))
		return -err_internal;

	payload = strtok(payload, " ,");
	if (!payload)
		return -err_parse_no_args;

	*x = strtoll(payload, &endptr, 0);
	if (payload == endptr || *endptr != '\0')
		return -err_parse_int;

	return 0;
}

int parse_uint8(uint8_t *x, char *payload)
{
	char *endptr;
	long int i;

	if (bug_on(!x))
		return -err_internal;

	payload = strtok(payload, " ,");
	if (!payload)
		return -err_parse_no_args;

	i = strtol(payload, &endptr, 0);
	if (payload == endptr || *endptr != '\0')
		return -err_parse_int;

	if (i > 0xff)
		return -err_parse_int_too_big;

	*x = (uint8_t)i;

	return 0;
}
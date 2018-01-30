/*
 * asc2log.c - convert ASC logfile to compact CAN frame logfile
 *
 * Copyright (c) 2002-2007 Volkswagen Group Electronic Research
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
 * 3. Neither the name of Volkswagen nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * Alternatively, provided that this notice is retained in full, this
 * software may be distributed under the terms of the GNU General
 * Public License ("GPL") version 2, in which case the provisions of the
 * GPL apply INSTEAD OF those given above.
 *
 * The provided data structures and external interfaces from this code
 * are not restricted to be used by modules with a GPL compatible license.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Send feedback to <linux-can@vger.kernel.org>
 *
 */

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <libgen.h>
#include <unistd.h>
#include <stdlib.h>
#include <locale.h>

#include <net/if.h>
#include <linux/can.h>
#include <linux/can/error.h>

#include "lib.h"

#define BUFFER_SZ 256
#define MSG_NAME_MAX 64

extern int optind, opterr, optopt;

typedef struct _msg_t
{
	char name[MSG_NAME_MAX];
	unsigned int id;
} msg_t;

typedef struct _msg_list_t
{
	unsigned int count;
	msg_t *data;
} msg_list_t;

static int verbose = 0;
static int raw_time = 0;

void print_usage(char *prg)
{
	fprintf(stderr, "Usage: %s\n", prg);
	fprintf(stderr, "Options: -I <infile>  (default stdin)\n");
	fprintf(stderr, "         -O <outfile> (default stdout)\n");
	fprintf(stderr, "         -D <dbcfile> (optional database)\n");
	fprintf(stderr, "         -r           (raw timestamps)\n");
	fprintf(stderr, "         -v           (verbose mode)\n");
	fprintf(stderr, "         -?           (help)\n");
}

void prframe(FILE *file, struct timeval *tv, int dev, struct can_frame *cf) {

	fprintf(file, "(%ld.%06ld) ", tv->tv_sec, tv->tv_usec);

	if (dev > 0)
		fprintf(file, "can%d ", dev-1);
	else
		fprintf(file, "canX ");

	/* no CAN FD support so far */
	fprint_canframe(file, (struct canfd_frame *)cf, "\n", 0, CAN_MAX_DLEN);
}

void get_can_id(struct can_frame *cf, char *idstring, int base, msg_list_t *msg_list) 
{
	int i;
	/* Check if idstring is in the MID_list and assign MID to can_id field if YES */
	for (i = 0; i < msg_list->count; i++)
	{
		if (strcmp(idstring, msg_list->data[i].name) == 0)
		{
			cf->can_id = msg_list->data[i].id;
			return;
		}
	}

	if (idstring[strlen(idstring)-1] == 'x') {
		cf->can_id = CAN_EFF_FLAG;
		idstring[strlen(idstring)-1] = 0;
	} else
		cf->can_id = 0;
    
	cf->can_id |= strtoul(idstring, NULL, base);
}

void calc_tv(struct timeval *tv, struct timeval *read_tv,
	     struct timeval *date_tv, char timestamps, int dplace) {

	if (dplace == 4) /* shift values having only 4 decimal places */
		read_tv->tv_usec *= 100;                /* and need for 6 */

	if (dplace == 5) /* shift values having only 5 decimal places */
		read_tv->tv_usec *= 10;                /* and need for 6 */

	if (timestamps == 'a') { /* absolute */

		tv->tv_sec  = date_tv->tv_sec  + read_tv->tv_sec;
		tv->tv_usec = date_tv->tv_usec + read_tv->tv_usec;

	} else { /* relative */

		if (((!tv->tv_sec) && (!tv->tv_usec)) && 
		    (date_tv->tv_sec || date_tv->tv_usec)) {
			tv->tv_sec  = date_tv->tv_sec; /* initial date/time */
			tv->tv_usec = date_tv->tv_usec;
		}

		tv->tv_sec  += read_tv->tv_sec;
		tv->tv_usec += read_tv->tv_usec;
	}

	if (tv->tv_usec > 1000000) {
		tv->tv_usec -= 1000000;
		tv->tv_sec++;
	}
}

int get_date(struct timeval *tv, char *date) {

	char ctmp[10];
	int  itmp;

	struct tm tms;

	if (sscanf(date, "%9s %d %9s %9s %d", ctmp, &itmp, ctmp, ctmp, &itmp) == 5) {
		/* assume EN/US date due to existing am/pm field */

		if (!setlocale(LC_TIME, "en_US")) {
			if (verbose)
			{
				fprintf(stderr, "Setting locale to 'en_US' failed!\n");
			}
			return 1;
		}

		if (!strptime(date, "%B %d %r %Y", &tms))
			return 1;

	} else {
		/* assume DE date due to non existing am/pm field */

		if (sscanf(date, "%9s %d %9s %d", ctmp, &itmp, ctmp, &itmp) != 4)
			return 1;

		if (!setlocale(LC_TIME, "de_DE")) {
			if (verbose)
			{
				fprintf(stderr, "Setting locale to 'de_DE' failed!\n");
			}
			return 1;
		}

		if (!strptime(date, "%B %d %T %Y", &tms))
			return 1;
	}
    
	// printf("h %d m %d s %d d %d m %d y %d  daylight:%d\n",
	// tms.tm_hour, tms.tm_min, tms.tm_sec,
	// tms.tm_mday, tms.tm_mon+1, tms.tm_year+1900, tms.tm_isdst);

	tms.tm_isdst = 0;
	tv->tv_sec = mktime(&tms);
	printf("%ld\n", tv->tv_sec);

	if (tv->tv_sec < 0)
		return 1;

	return 0;
}

msg_list_t process_dbc(FILE *dbcfile)
{
	msg_list_t msg_list = {0, NULL};
	char buf[BUFFER_SZ];

	unsigned int index;
	unsigned int m_id;
	char m_name[MSG_NAME_MAX];

	if (dbcfile)
	{
		while (fgets(buf, BUFFER_SZ - 1, dbcfile))
		{
			if (sscanf(buf, "BO_ %d %s %*d %*s", &m_id, m_name) == 2)
			{
				m_name[strlen(m_name) - 1] = '\0';
				msg_list.count++;
				msg_list.data = (msg_t *) realloc(msg_list.data, msg_list.count * sizeof(msg_t));

				index = msg_list.count - 1;
				msg_list.data[index].id = m_id;
				strcpy(msg_list.data[index].name, m_name);
			}
		}
		fclose(dbcfile);
	}

	return msg_list;
}

int main(int argc, char **argv)
{
	char buf[100], tmp1[100], tmp2[100];
	msg_list_t msg_list;

	FILE *infile = stdin;
	FILE *outfile = stdout;
	FILE *dbcfile = NULL;

	struct can_frame cf;
	static struct timeval tv; /* current frame timestamp */
	static struct timeval read_tv; /* frame timestamp from ASC file */
	static struct timeval date_tv; /* date of the ASC file */
	static int dplace; /* decimal place 4, 5 or 6 or uninitialized */
	static char base; /* 'd'ec or 'h'ex */
	static char timestamps; /* 'a'bsolute or 'r'elative */

	int interface;
	char rtr;
	int dlc = 0;
	int data[8];
	int i, found, opt;

	/* Process command line arguments */
	while ((opt = getopt(argc, argv, "I:O:D:rv?")) != -1) {
		switch (opt) {
		case 'I':
			infile = fopen(optarg, "r");
			if (!infile) {
				perror("infile");
				return 1;
			}
			break;

		case 'O':
			outfile = fopen(optarg, "w");
			if (!outfile) {
				perror("outfile");
				return 1;
			}
			break;

		case 'D':
			dbcfile = fopen(optarg, "r");
			if (!dbcfile) {
				perror("dbcfile");
				return 1;
			}
			break;

		case 'r':
			raw_time = 1;
			break;

		case 'v':
			verbose = 1;
			break;

		case '?':
			print_usage(basename(argv[0]));
			return 0;
			break;

		default:
			fprintf(stderr, "Unknown option %c\n", opt);
			print_usage(basename(argv[0]));
			return 1;
			break;
		}
	}

	/* Read dbc file */
	msg_list = process_dbc(dbcfile);
	if (verbose)
	{
		printf("Reading messages from .dbc file:\n");
		for (i = 0; i < msg_list.count; i++)
		{
			printf("[%d] %s\n", msg_list.data[i].id, msg_list.data[i].name);
		}
	}

	/* Read trace file */
	while (fgets(buf, 99, infile)) {

		if (!dplace) { /* the representation of a valid CAN frame not known */

			/* check for base and timestamp entries in the header */
			if ((!base) &&
			    (sscanf(buf, "base %s timestamps %s", tmp1, tmp2) == 2)) {
				base = tmp1[0];
				timestamps = tmp2[0];
				if (verbose)
					printf("base %c timestamps %c\n", base, timestamps);
				if ((base != 'h') && (base != 'd')) {
					printf("invalid base %s (must be 'hex' or 'dez')!\n",
					       tmp1);
					return 1;
				}
				if ((timestamps != 'a') && (timestamps != 'r')) {
					printf("invalid timestamps %s (must be 'absolute'"
					       " or 'relative')!\n", tmp2);
					return 1;
				}
				continue;
			}

			/* check for the original logging date in the header */ 
			if ((!date_tv.tv_sec) &&
			    (!strncmp(buf, "date", 4))) {

				if (get_date(&date_tv, &buf[9])) { /* skip 'date day ' */
					if (verbose)
					{
						fprintf(stderr, "Not able to determine original log "
							"file date. Using current time.\n");
					}

					/* use current date as default */
					gettimeofday(&date_tv, NULL);
				}
				if (verbose)
					printf("date %ld => %s", date_tv.tv_sec, ctime(&date_tv.tv_sec));
				continue;
			}

			/* check for decimal places length in valid CAN frames */
			if (sscanf(buf, "%ld.%s %d ", &tv.tv_sec, tmp2, &i) == 3){
				dplace = strlen(tmp2);
				if (verbose)
					printf("decimal place %d, e.g. '%s'\n", dplace, tmp2);
				if (dplace < 4 || dplace > 6) {
					printf("invalid dplace %d (must be 4, 5 or 6)!\n", dplace);
					return 1;
				}
			} else
				continue; /* dplace remains zero until first found CAN frame */
		} 

		/* the representation of a valid CAN frame is known here */
		/* so try to get CAN frames and ErrorFrames and convert them */

		/* 0.002367 1 390x Rx d 8 17 00 14 00 C0 00 08 00 */

		found = 0; /* found valid CAN frame ? */

		if (base == 'h') { /* check for CAN frames with hexadecimal values */

			if (sscanf(buf, "%ld.%ld %d %s %*s %c %d %x %x %x %x %x %x %x %x",
				   &read_tv.tv_sec, &read_tv.tv_usec, &interface,
				   tmp1, &rtr, &dlc,
				   &data[0], &data[1], &data[2], &data[3],
				   &data[4], &data[5], &data[6], &data[7]
				    ) == dlc + 6 ) {

				found = 1;
				get_can_id(&cf, tmp1, 16, &msg_list);
			}

		} else { /* check for CAN frames with decimal values */

			if (sscanf(buf, "%ld.%ld %d %s %*s %c %d %d %d %d %d %d %d %d %d",
				   &read_tv.tv_sec, &read_tv.tv_usec, &interface,
				   tmp1, &rtr, &dlc,
				   &data[0], &data[1], &data[2], &data[3],
				   &data[4], &data[5], &data[6], &data[7]
				    ) == dlc + 6 ) {

				found = 1;
				get_can_id(&cf, tmp1, 10, &msg_list);
			}
		}

		if (found) {
			if (rtr == 'r')
				cf.can_id |= CAN_RTR_FLAG;
 
			cf.can_dlc = dlc & 0x0FU;
			for (i=0; i<dlc; i++)
				cf.data[i] = data[i] & 0xFFU;

			if (raw_time)
			{
				prframe(outfile, &read_tv, interface, &cf);
			}
			else
			{
				calc_tv(&tv, &read_tv, &date_tv, timestamps, dplace);
				prframe(outfile, &tv, interface, &cf);
			}
			fflush(outfile);
			continue;
		}

		/* check for ErrorFrames */
		if (sscanf(buf, "%ld.%ld %d %s",
			   &read_tv.tv_sec, &read_tv.tv_usec,
			   &interface, tmp1) == 4) {
		
			if (!strncmp(tmp1, "ErrorFrame", strlen("ErrorFrame"))) {

				memset(&cf, 0, sizeof(cf));
				/* do not know more than 'Error' */
				cf.can_id  = (CAN_ERR_FLAG | CAN_ERR_BUSERROR);
				cf.can_dlc =  CAN_ERR_DLC;
		    
				if (raw_time)
				{
					prframe(outfile, &read_tv, interface, &cf);
				}
				else
				{
					calc_tv(&tv, &read_tv, &date_tv, timestamps, dplace);
					prframe(outfile, &tv, interface, &cf);
				}
				fflush(outfile);
			}
		}
	}

	if (msg_list.data)
	{
		free(msg_list.data);
	}

	fclose(outfile);
	fclose(infile);

	return 0;
}

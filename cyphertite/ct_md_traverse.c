/*
 * Copyright (c) 2011 Conformal Systems LLC <info@conformal.com>
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


#include <sys/types.h>
#include <sys/stat.h>

#include <inttypes.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <libgen.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <regex.h>
#include <vis.h>
#include <errno.h>

#include <assl.h>
#include <clog.h>
#include <exude.h>
#include <xmlsd.h>
#include <fts.h>

#include <ctutil.h>
#include "ct_xml.h"

#include "ct.h"
#include "ct_crypto.h"

struct md_list_file {
	union {
		RB_ENTRY(md_list_file)		nxt;
		SLIST_ENTRY(md_list_file)	lnk;
	}					mlf_entries;
#define mlf_next	mlf_entries.nxt
#define mlf_link	mlf_entries.lnk
	char					mlf_name[CT_MD_TAG_MAXLEN];
	off_t					mlf_size;
	time_t					mlf_mtime;
};

RB_HEAD(md_list_tree, md_list_file);
RB_PROTOTYPE(md_list_tree, md_list_file, next, ct_cmp_md);

void	ct_md_list_complete(int, char **, char **, struct md_list_tree *);

/* Taken from OpenBSD ls */
static void
printtime(time_t ftime)
{
	int i;
	char *longstring;

	longstring = ctime(&ftime);
	for (i = 4; i < 11; ++i)
		(void)putchar(longstring[i]);

#define DAYSPERNYEAR	365
#define SECSPERDAY	(60*60*24)
#define	SIXMONTHS	((DAYSPERNYEAR / 2) * SECSPERDAY)
	if (ftime + SIXMONTHS > time(NULL))
		for (i = 11; i < 16; ++i)
			(void)putchar(longstring[i]);
	else {
		(void)putchar(' ');
		for (i = 20; i < 24; ++i)
			(void)putchar(longstring[i]);
	}
	(void)putchar(' ');
}


void
ct_md_list_print(struct ct_op *op)
{
	struct md_list_tree	 results;
	struct md_list_file	*file;
	int64_t			maxsz = 8;
	ssize_t			ret;
	char			unvised[CT_MD_TAG_MAXLEN];
	int			numlen;

	RB_INIT(&results);
	ct_md_list_complete(op->op_matchmode, op->op_filelist,
	    op->op_excludelist, &results);
	RB_FOREACH(file, md_list_tree, &results) {
		if (maxsz < (int64_t)file->mlf_size)
			maxsz  = (int64_t)file->mlf_size;
	}
	numlen = snprintf(NULL, 0, "%" PRId64, maxsz);

	while ((file = RB_MIN(md_list_tree, &results)) != NULL) {
		RB_REMOVE(md_list_tree, &results, file);
		/* XXX only the extras if verbose? */
		ret = strnunvis(unvised, file->mlf_name, sizeof(unvised));
		if (ret >= sizeof(unvised) || ret == -1) {
			CWARNX("can't unvis filename %s", file->mlf_name);
			e_free(&file);
			continue;
		}

		printf("%*llu ", numlen, (unsigned long long)file->mlf_size);
		printtime(file->mlf_mtime);
		printf("\t");
		printf("%s\n", unvised);
		e_free(&file);
	}
}


/*
 * make fts_* return entities in mtime order, oldest first
 */
/* XXX: Need to clean this up with more portable code.  Using ifdefs for now
 * to make it compile.
 */
#ifdef __FreeBSD__
static int
datecompare(const FTSENT * const *a, const FTSENT * const *b)
{
	return (timespeccmp(&(*a)->fts_statp->st_mtimespec,
	    &(*b)->fts_statp->st_mtimespec, <));
}
#else
static int
datecompare(const FTSENT **a, const FTSENT **b)
{
	return (timespeccmp(&(*a)->fts_statp->st_mtim,
	    &(*b)->fts_statp->st_mtim, <));
}
#endif

/*
 * Trim down the metadata cachedir to be smaller than ``max_size''.
 *
 * We only look at files in the directory (and lower, since we use fts(3),
 * since cyphertite will only ever create files, not symlinkts or directories.
 * We delete files in date order, oldest first, until the size constraint has
 * been met.
 */
void
ct_mdcache_trim(const char *cachedir, long long max_size)
{
	char		*paths[2];
	FTS		*ftsp;
	FTSENT		*fe;
	long long	 dirsize = 0;

	paths[0] = (char *)cachedir;
	paths[1] = NULL;

	if ((ftsp = fts_open(paths, FTS_XDEV | FTS_PHYSICAL | FTS_NOCHDIR,
	   NULL)) == NULL)
		CFATAL("can't open metadata cache to scan");

	while ((fe = fts_read(ftsp)) != NULL) {
		switch(fe->fts_info) {
		case FTS_F:
			/*
			 * XXX no OFF_T_MAX in posix, on openbsd it is always a
			 * long long
			 */
			if (LLONG_MAX - dirsize < fe->fts_statp->st_size)
				CWARNX("dirsize overflowed");
			dirsize += fe->fts_statp->st_size;
			break;
		case FTS_ERR:
		case FTS_DNR:
		case FTS_NS:
			errno = fe->fts_errno;
			CFATAL("can't read directory entry");
		case FTS_DC:
			CWARNX("file system cycle found");
			/* FALLTHROUGH */
		default:
			/* valid but we don't care */
			continue;
		}
	}

	if (fts_close(ftsp))
		CFATAL("close directory failed");

	if (dirsize <= max_size)
		return;
	CDBG("cleaning up md cachedir, %llu > %llu",
	    (long long)dirsize, (long long)max_size);

	if ((ftsp = fts_open(paths, FTS_XDEV | FTS_PHYSICAL | FTS_NOCHDIR,
	    datecompare)) == NULL)
		CFATAL("can't open metadata cache to trim");

	while ((fe = fts_read(ftsp)) != NULL) {
		switch(fe->fts_info) {
		case FTS_F:
			CDBG("%s %llu", fe->fts_path,
			    (long long)fe->fts_statp->st_size);
			if (unlink(fe->fts_path) != 0) {
				CWARN("couldn't delete md file %s",
				    fe->fts_path);
				continue;
			}
			dirsize -= fe->fts_statp->st_size;
			break;
		case FTS_ERR:
		case FTS_DNR:
		case FTS_NS:
			errno = fe->fts_errno;
			CFATAL("can't read directory entry");
		case FTS_DC:
			CWARNX("file system cycle found");
			/* FALLTHROUGH */
		default:
			/* valid but we don't care */
			continue;
		}
		CDBG("size now %llu", (long long)dirsize);

		if (dirsize < max_size)
			break;
	}

	if (fts_close(ftsp))
		CFATAL("close directory failed");
}

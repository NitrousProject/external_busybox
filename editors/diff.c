/* vi: set sw=4 ts=4: */
/*
 * Mini diff implementation for busybox, adapted from OpenBSD diff.
 *
 * Copyright (C) 2010 by Matheus Izvekov <mizvekov@gmail.com>
 * Copyright (C) 2006 by Robert Sullivan <cogito.ergo.cogito@hotmail.com>
 * Copyright (c) 2003 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 *
 * Licensed under GPLv2 or later, see file LICENSE in this tarball for details.
 */

/*
 * The following code uses an algorithm due to Harold Stone,
 * which finds a pair of longest identical subsequences in
 * the two files.
 *
 * The major goal is to generate the match vector J.
 * J[i] is the index of the line in file1 corresponding
 * to line i in file0. J[i] = 0 if there is no
 * such line in file1.
 *
 * Lines are hashed so as to work in core. All potential
 * matches are located by sorting the lines of each file
 * on the hash (called "value"). In particular, this
 * collects the equivalence classes in file1 together.
 * Subroutine equiv replaces the value of each line in
 * file0 by the index of the first element of its
 * matching equivalence in (the reordered) file1.
 * To save space equiv squeezes file1 into a single
 * array member in which the equivalence classes
 * are simply concatenated, except that their first
 * members are flagged by changing sign.
 *
 * Next the indices that point into member are unsorted into
 * array class according to the original order of file0.
 *
 * The cleverness lies in routine stone. This marches
 * through the lines of file0, developing a vector klist
 * of "k-candidates". At step i a k-candidate is a matched
 * pair of lines x,y (x in file0, y in file1) such that
 * there is a common subsequence of length k
 * between the first i lines of file0 and the first y
 * lines of file1, but there is no such subsequence for
 * any smaller y. x is the earliest possible mate to y
 * that occurs in such a subsequence.
 *
 * Whenever any of the members of the equivalence class of
 * lines in file1 matable to a line in file0 has serial number
 * less than the y of some k-candidate, that k-candidate
 * with the smallest such y is replaced. The new
 * k-candidate is chained (via pred) to the current
 * k-1 candidate so that the actual subsequence can
 * be recovered. When a member has serial number greater
 * that the y of all k-candidates, the klist is extended.
 * At the end, the longest subsequence is pulled out
 * and placed in the array J by unravel
 *
 * With J in hand, the matches there recorded are
 * checked against reality to assure that no spurious
 * matches have crept in due to hashing. If they have,
 * they are broken, and "jackpot" is recorded--a harmless
 * matter except that a true match for a spuriously
 * mated line may now be unnecessarily reported as a change.
 *
 * Much of the complexity of the program comes simply
 * from trying to minimize core utilization and
 * maximize the range of doable problems by dynamically
 * allocating what is needed and reusing what is not.
 * The core requirements for problems larger than somewhat
 * are (in words) 2*length(file0) + length(file1) +
 * 3*(number of k-candidates installed), typically about
 * 6n words for files of length n.
 */

#include "libbb.h"

#if 0
//#define dbg_error_msg(...) bb_error_msg(__VA_ARGS__)
#else
#define dbg_error_msg(...) ((void)0)
#endif

enum {                   /* print_status() and diffreg() return values */
	STATUS_SAME,     /* files are the same */
	STATUS_DIFFER,   /* files differ */
	STATUS_BINARY,   /* binary files differ */
};

enum {                   /* Commandline flags */
	FLAG_a,
	FLAG_b,
	FLAG_d,
	FLAG_i, /* unused */
	FLAG_L, /* unused */
	FLAG_N,
	FLAG_q,
	FLAG_r,
	FLAG_s,
	FLAG_S, /* unused */
	FLAG_t,
	FLAG_T,
	FLAG_U, /* unused */
	FLAG_w,
};
#define FLAG(x) (1 << FLAG_##x)

/* We cache file position to avoid excessive seeking */
typedef struct FILE_and_pos_t {
	FILE *ft_fp;
	off_t ft_pos;
} FILE_and_pos_t;

struct globals {
	smallint exit_status;
	int opt_U_context;
	char *label[2];
	struct stat stb[2];
};
#define G (*ptr_to_globals)
#define exit_status        (G.exit_status       )
#define opt_U_context      (G.opt_U_context     )
#define label              (G.label             )
#define stb                (G.stb               )
#define INIT_G() do { \
	SET_PTR_TO_GLOBALS(xzalloc(sizeof(G))); \
	opt_U_context = 3; \
} while (0)

typedef int token_t;

enum {
	/* Public */
	TOK_EMPTY = 1 << 9,  /* Line fully processed, you can proceed to the next */
	TOK_EOF   = 1 << 10, /* File ended */
	/* Private (Only to be used by read_token() */
	TOK_EOL   = 1 << 11, /* we saw EOL (sticky) */
	TOK_SPACE = 1 << 12, /* used -b code, means we are skipping spaces */
	SHIFT_EOF = (sizeof(token_t)*8 - 8) - 1,
	CHAR_MASK = 0x1ff,   /* 8th bit is used to distinguish EOF from 0xff */
};

/* Restores full EOF from one 8th bit: */
//#define TOK2CHAR(t) (((t) << SHIFT_EOF) >> SHIFT_EOF)
/* We don't really need the above, we only need to have EOF != any_real_char: */
#define TOK2CHAR(t) ((t) & CHAR_MASK)

static void seek_ft(FILE_and_pos_t *ft, off_t pos)
{
	if (ft->ft_pos != pos) {
		ft->ft_pos = pos;
		fseeko(ft->ft_fp, pos, SEEK_SET);
	}
}

/* Reads tokens from given fp, handling -b and -w flags
 * The user must reset tok every line start
 */
static int read_token(FILE_and_pos_t *ft, token_t tok)
{
	tok |= TOK_EMPTY;
	while (!(tok & TOK_EOL)) {
		bool is_space;
		int t;

		t = fgetc(ft->ft_fp);
		if (t != EOF)
			ft->ft_pos++;
		is_space = (t == EOF || isspace(t));

		/* If t == EOF (-1), set both TOK_EOF and TOK_EOL */
		tok |= (t & (TOK_EOF + TOK_EOL));
		/* Only EOL? */
		if (t == '\n')
			tok |= TOK_EOL;

		if ((option_mask32 & FLAG(w)) && is_space)
			continue;

		/* Trim char value to low 9 bits */
		t &= CHAR_MASK;

		if (option_mask32 & FLAG(b)) {
			/* Was prev char whitespace? */
			if (tok & TOK_SPACE) { /* yes */
				if (is_space) /* this one too, ignore it */
					continue;
				tok &= ~TOK_SPACE;
			} else if (is_space) {
				/* 1st whitespace char.
				 * Set TOK_SPACE and replace char by ' ' */
				t = TOK_SPACE + ' ';
			}
		}
		/* Clear EMPTY */
		tok &= ~(TOK_EMPTY + CHAR_MASK);
		/* Assign char value (low 9 bits) and maybe set TOK_SPACE */
		tok |= t;
		break;
	}
#if 0
	bb_error_msg("fp:%p tok:%x '%c'%s%s%s%s", fp, tok, tok & 0xff
		, tok & TOK_EOF ? " EOF" : ""
		, tok & TOK_EOL ? " EOL" : ""
		, tok & TOK_EMPTY ? " EMPTY" : ""
		, tok & TOK_SPACE ? " SPACE" : ""
	);
#endif
	return tok;
}

struct cand {
	int x;
	int y;
	int pred;
};

static int search(const int *c, int k, int y, const struct cand *list)
{
	if (list[c[k]].y < y)	/* quick look for typical case */
		return k + 1;

	for (int i = 0, j = k + 1;;) {
		const int l = (i + j) >> 1;
		if (l > i) {
			const int t = list[c[l]].y;
			if (t > y)
				j = l;
			else if (t < y)
				i = l;
			else
				return l;
		} else
			return l + 1;
	}
}

static unsigned isqrt(unsigned n)
{
	unsigned x = 1;
	while (1) {
		const unsigned y = x;
		x = ((n / x) + x) >> 1;
		if (x <= (y + 1) && x >= (y - 1))
			return x;
	}
}

static void stone(const int *a, int n, const int *b, int *J, int pref)
{
	const unsigned isq = isqrt(n);
	const unsigned bound =
		(option_mask32 & FLAG(d)) ? UINT_MAX : MAX(256, isq);
	int clen = 1;
	int clistlen = 100;
	int k = 0;
	struct cand *clist = xzalloc(clistlen * sizeof(clist[0]));
	int *klist = xzalloc((n + 2) * sizeof(klist[0]));
	/*clist[0] = (struct cand){0}; - xzalloc did it */
	/*klist[0] = 0; */

	for (struct cand cand = {1}; cand.x <= n; cand.x++) {
		int j = a[cand.x], oldl = 0;
		unsigned numtries = 0;
		if (j == 0)
			continue;
		cand.y = -b[j];
		cand.pred = klist[0];
		do {
			int l, tc;
			if (cand.y <= clist[cand.pred].y)
				continue;
			l = search(klist, k, cand.y, clist);
			if (l != oldl + 1)
				cand.pred = klist[l - 1];
			if (l <= k && clist[klist[l]].y <= cand.y)
				continue;
			if (clen == clistlen) {
				clistlen = clistlen * 11 / 10;
				clist = xrealloc(clist, clistlen * sizeof(clist[0]));
			}
			clist[clen] = cand;
			tc = klist[l];
			klist[l] = clen++;
			if (l <= k) {
				cand.pred = tc;
				oldl = l;
				numtries++;
			} else {
				k++;
				break;
			}
		} while ((cand.y = b[++j]) > 0 && numtries < bound);
	}
	/* Unravel */
	for (struct cand *q = clist + klist[k]; q->y; q = clist + q->pred)
		J[q->x + pref] = q->y + pref;
	free(klist);
	free(clist);
}

struct line {
	/* 'serial' is not used in the begining, so we reuse it
	 * to store line offsets, thus reducing memory pressure
	 */
	union {
		unsigned serial;
		off_t offset;
	};
	unsigned value;
};

static void equiv(struct line *a, int n, struct line *b, int m, int *c)
{
	int i = 1, j = 1;

	while (i <= n && j <= m) {
		if (a[i].value < b[j].value)
			a[i++].value = 0;
		else if (a[i].value == b[j].value)
			a[i++].value = j;
		else
			j++;
	}
	while (i <= n)
		a[i++].value = 0;
	b[m + 1].value = 0;
	j = 0;
	while (++j <= m) {
		c[j] = -b[j].serial;
		while (b[j + 1].value == b[j].value) {
			j++;
			c[j] = b[j].serial;
		}
	}
	c[j] = -1;
}

static void unsort(const struct line *f, int l, int *b)
{
	int *a = xmalloc((l + 1) * sizeof(a[0]));
	for (int i = 1; i <= l; i++)
		a[f[i].serial] = f[i].value;
	for (int i = 1; i <= l; i++)
		b[i] = a[i];
	free(a);
}

static int line_compar(const void *a, const void *b)
{
#define l0 ((const struct line*)a)
#define l1 ((const struct line*)b)
	int r = l0->value - l1->value;
	if (r)
		return r;
	return l0->serial - l1->serial;
#undef l0
#undef l1
}

static void uni_range(int a, int b)
{
	if (a < b)
		printf("%d,%d", a, b - a + 1);
	else if (a == b)
		printf("%d", b);
	else
		printf("%d,0", b);
}

static void fetch(FILE_and_pos_t *ft, const off_t *ix, int a, int b, int ch)
{
	for (int i = a; i <= b; i++) {
		seek_ft(ft, ix[i - 1]);
		putchar(ch);
		if (option_mask32 & FLAG(T))
			putchar('\t');
		for (int j = 0, col = 0; j < ix[i] - ix[i - 1]; j++) {
			int c = fgetc(ft->ft_fp);
			if (c == EOF) {
				printf("\n\\ No newline at end of file\n");
				return;
			}
			ft->ft_pos++;
			if (c == '\t' && (option_mask32 & FLAG(t)))
				do putchar(' '); while (++col & 7);
			else {
				putchar(c);
				col++;
			}
		}
	}
}

/* Creates the match vector J, where J[i] is the index
 * of the line in the new file corresponding to the line i
 * in the old file. Lines start at 1 instead of 0, that value
 * being used instead to denote no corresponding line.
 * This vector is dynamically allocated and must be freed by the caller.
 *
 * * fp is an input parameter, where fp[0] and fp[1] are the open
 *   old file and new file respectively.
 * * nlen is an output variable, where nlen[0] and nlen[1]
 *   gets the number of lines in the old and new file respectively.
 * * ix is an output variable, where ix[0] and ix[1] gets
 *   assigned dynamically allocated vectors of the offsets of the lines
 *   of the old and new file respectively. These must be freed by the caller.
 */
static int *create_J(FILE_and_pos_t ft[2], int nlen[2], off_t *ix[2])
{
	int *J, slen[2], *class, *member;
	struct line *nfile[2], *sfile[2];
	int pref = 0, suff = 0;

	/* Lines of both files are hashed, and in the process
	 * their offsets are stored in the array ix[fileno]
	 * where fileno == 0 points to the old file, and
	 * fileno == 1 points to the new one.
	 */
	for (int i = 0; i < 2; i++) {
		unsigned hash;
		token_t tok;
		size_t sz = 100;
		nfile[i] = xmalloc((sz + 3) * sizeof(nfile[i][0]));
		seek_ft(&ft[i], 0);

		nlen[i] = 0;
		/* We could zalloc nfile, but then zalloc starts showing in gprof at ~1% */
		nfile[i][0].offset = 0;
		goto start; /* saves code */
		while (1) {
			tok = read_token(&ft[i], tok);
			if (!(tok & TOK_EMPTY)) {
				/* Hash algorithm taken from Robert Sedgewick, Algorithms in C, 3d ed., p 578. */
				/*hash = hash * 128 - hash + TOK2CHAR(tok);
				 * gcc insists on optimizing above to "hash * 127 + ...", thus... */
				unsigned o = hash - TOK2CHAR(tok);
				hash = hash * 128 - o; /* we want SPEED here */
				continue;
			}
			if (nlen[i]++ == sz) {
				sz = sz * 3 / 2;
				nfile[i] = xrealloc(nfile[i], (sz + 3) * sizeof(nfile[i][0]));
			}
			/* line_compar needs hashes fit into positive int */
			nfile[i][nlen[i]].value = hash & INT_MAX;
			/* like ftello(ft[i].ft_fp) but faster (avoids lseek syscall) */
			nfile[i][nlen[i]].offset = ft[i].ft_pos;
			if (tok & TOK_EOF) {
				/* EOF counts as a token, so we have to adjust it here */
				nfile[i][nlen[i]].offset++;
				break;
			}
start:
			hash = tok = 0;
		}
		/* Exclude lone EOF line from the end of the file, to make fetch()'s job easier */
		if (nfile[i][nlen[i]].offset - nfile[i][nlen[i] - 1].offset == 1)
			nlen[i]--;
		/* Now we copy the line offsets into ix */
		ix[i] = xmalloc((nlen[i] + 2) * sizeof(ix[i][0]));
		for (int j = 0; j < nlen[i] + 1; j++)
			ix[i][j] = nfile[i][j].offset;
	}

	/* lenght of prefix and suffix is calculated */
	for (; pref < nlen[0] && pref < nlen[1] &&
	       nfile[0][pref + 1].value == nfile[1][pref + 1].value;
	       pref++);
	for (; suff < nlen[0] - pref && suff < nlen[1] - pref &&
	       nfile[0][nlen[0] - suff].value == nfile[1][nlen[1] - suff].value;
	       suff++);
	/* Arrays are pruned by the suffix and prefix lenght,
	 * the result being sorted and stored in sfile[fileno],
	 * and their sizes are stored in slen[fileno]
	 */
	for (int j = 0; j < 2; j++) {
		sfile[j] = nfile[j] + pref;
		slen[j] = nlen[j] - pref - suff;
		for (int i = 0; i <= slen[j]; i++)
			sfile[j][i].serial = i;
		qsort(sfile[j] + 1, slen[j], sizeof(*sfile[j]), line_compar);
	}
	/* nfile arrays are reused to reduce memory pressure
	 * The #if zeroed out section performs the same task as the
	 * one in the #else section.
	 * Peak memory usage is higher, but one array copy is avoided
	 * by not using unsort()
	 */
#if 0
	member = xmalloc((slen[1] + 2) * sizeof(member[0]));
	equiv(sfile[0], slen[0], sfile[1], slen[1], member);
	free(nfile[1]);

	class = xmalloc((slen[0] + 1) * sizeof(class[0]));
	for (int i = 1; i <= slen[0]; i++) /* Unsorting */
		class[sfile[0][i].serial] = sfile[0][i].value;
	free(nfile[0]);
#else
	member = (int *)nfile[1];
	equiv(sfile[0], slen[0], sfile[1], slen[1], member);
	member = xrealloc(member, (slen[1] + 2) * sizeof(member[0]));

	class = (int *)nfile[0];
	unsort(sfile[0], slen[0], (int *)nfile[0]);
	class = xrealloc(class, (slen[0] + 2) * sizeof(class[0]));
#endif
	J = xmalloc((nlen[0] + 2) * sizeof(J[0]));
	/* The elements of J which fall inside the prefix and suffix regions
	 * are marked as unchanged, while the ones which fall outside
	 * are initialized with 0 (no matches), so that function stone can
	 * then assign them their right values
	 */
	for (int i = 0, delta = nlen[1] - nlen[0]; i <= nlen[0]; i++)
		J[i] = i <= pref            ?  i :
		       i > (nlen[0] - suff) ? (i + delta) : 0;
	/* Here the magic is performed */
	stone(class, slen[0], member, J, pref);
	J[nlen[0] + 1] = nlen[1] + 1;

	free(class);
	free(member);

	/* Both files are rescanned, in an effort to find any lines
	 * which, due to limitations intrinsic to any hashing algorithm,
	 * are different but ended up confounded as the same
	 */
	for (int i = 1; i <= nlen[0]; i++) {
		if (!J[i])
			continue;

		seek_ft(&ft[0], ix[0][i - 1]);
		seek_ft(&ft[1], ix[1][J[i] - 1]);

		for (int j = J[i]; i <= nlen[0] && J[i] == j; i++, j++) {
			token_t tok0 = 0, tok1 = 0;
			do {
				tok0 = read_token(&ft[0], tok0);
				tok1 = read_token(&ft[1], tok1);

				if (((tok0 ^ tok1) & TOK_EMPTY) != 0 /* one is empty (not both) */
				 || (!(tok0 & TOK_EMPTY) && TOK2CHAR(tok0) != TOK2CHAR(tok1)))
					J[i] = 0; /* Break the correspondence */
			} while (!(tok0 & tok1 & TOK_EMPTY));
		}
	}

	return J;
}

/*
 * The following struct is used to record change information
 * doing a "context" or "unified" diff.
 */
struct context_vec {
	int a;          /* start line in old file */
	int b;          /* end line in old file */
	int c;          /* start line in new file */
	int d;          /* end line in new file */
};

static bool diff(FILE_and_pos_t ft[2], char *file[2])
{
	int nlen[2];
	off_t *ix[2];
	int *J = create_J(ft, nlen, ix);

	bool anychange = false;
	struct context_vec *vec = NULL;
	int idx = -1, i = 1;

	do {
		while (1) {
			struct context_vec v;

			for (v.a = i; v.a <= nlen[0] && J[v.a] == J[v.a - 1] + 1; v.a++)
				continue;
			v.c = J[v.a - 1] + 1;

			for (v.b = v.a - 1; v.b < nlen[0] && !J[v.b + 1]; v.b++)
				continue;
			v.d = J[v.b + 1] - 1;
			/*
			 * Indicate that there is a difference between lines a and b of the 'from' file
			 * to get to lines c to d of the 'to' file. If a is greater than b then there
			 * are no lines in the 'from' file involved and this means that there were
			 * lines appended (beginning at b).  If c is greater than d then there are
			 * lines missing from the 'to' file.
			 */
			if (v.a <= v.b || v.c <= v.d) {
				/*
				 * If this change is more than 'context' lines from the
				 * previous change, dump the record and reset it.
				 */
				if (idx >= 0
				 && v.a > vec[idx].b + (2 * opt_U_context) + 1
				 && v.c > vec[idx].d + (2 * opt_U_context) + 1
				) {
					break;
				}
				vec = xrealloc_vector(vec, 6, ++idx);
				vec[idx] = v;
			}

			i = v.b + 1;
			if (i > nlen[0])
				break;
			J[v.b] = v.d;
		}
		if (idx < 0)
			continue;
		if (!(option_mask32 & FLAG(q))) {
			struct context_vec *cvp = vec;
			int lowa = MAX(1, cvp->a - opt_U_context);
			int upb  = MIN(nlen[0], vec[idx].b + opt_U_context);
			int lowc = MAX(1, cvp->c - opt_U_context);
			int upd  = MIN(nlen[1], vec[idx].d + opt_U_context);

			if (!anychange) {
				/* Print the context/unidiff header first time through */
				printf("--- %s\n", label[0] ?: file[0]);
				printf("+++ %s\n", label[1] ?: file[1]);
			}

			printf("@@ -");
			uni_range(lowa, upb);
			printf(" +");
			uni_range(lowc, upd);
			printf(" @@\n");

			/*
			 * Output changes in "unified" diff format--the old and new lines
			 * are printed together.
			 */
			while (1) {
				bool end = cvp > &vec[idx];
				fetch(&ft[0], ix[0], lowa, end ? upb : cvp->a - 1, ' ');
				if (end)
					break;
				fetch(&ft[0], ix[0], cvp->a, cvp->b, '-');
				fetch(&ft[1], ix[1], cvp->c, cvp->d, '+');
				lowa = cvp++->b + 1;
			}
		}
		idx = -1;
		anychange = true;
	} while (i <= nlen[0]);

	free(vec);
	free(ix[0]);
	free(ix[1]);
	free(J);
	return anychange;
}

static int diffreg(char *file[2])
{
	FILE_and_pos_t ft[2];
	bool binary = false, differ = false;
	int status = STATUS_SAME;

	for (int i = 0; i < 2; i++) {
		int fd = open_or_warn_stdin(file[i]);
		if (fd == -1)
			xfunc_die();
		/* Our diff implementation is using seek.
		 * When we meet non-seekable file, we must make a temp copy.
		 */
		ft[i].ft_pos = 0;
		if (lseek(fd, 0, SEEK_SET) == -1 && errno == ESPIPE) {
			char name[] = "/tmp/difXXXXXX";
			int fd_tmp = mkstemp(name);
			if (fd_tmp < 0)
				bb_perror_msg_and_die("mkstemp");
			unlink(name);
			ft[i].ft_pos = bb_copyfd_eof(fd, fd_tmp);
			/* error message is printed by bb_copyfd_eof */
			if (ft[i].ft_pos < 0)
				xfunc_die();
			fstat(fd, &stb[i]);
			if (fd) /* Prevents closing of stdin */
				close(fd);
			fd = fd_tmp;
		}
		ft[i].ft_fp = fdopen(fd, "r");
	}

	while (1) {
		const size_t sz = COMMON_BUFSIZE / 2;
		char *const buf0 = bb_common_bufsiz1;
		char *const buf1 = buf0 + sz;
		int i, j;
		i = fread(buf0, 1, sz, ft[0].ft_fp);
		ft[0].ft_pos += i;
		j = fread(buf1, 1, sz, ft[1].ft_fp);
		ft[1].ft_pos += j;
		if (i != j) {
			differ = true;
			i = MIN(i, j);
		}
		if (i == 0)
			break;
		for (int k = 0; k < i; k++) {
			if (!buf0[k] || !buf1[k])
				binary = true;
			if (buf0[k] != buf1[k])
				differ = true;
		}
	}
	if (differ) {
		if (binary && !(option_mask32 & FLAG(a)))
			status = STATUS_BINARY;
		else if (diff(ft, file))
			status = STATUS_DIFFER;
	}
	if (status != STATUS_SAME)
		exit_status |= 1;

	fclose_if_not_stdin(ft[0].ft_fp);
	fclose_if_not_stdin(ft[1].ft_fp);

	return status;
}

static void print_status(int status, char *path[2])
{
	switch (status) {
	case STATUS_BINARY:
	case STATUS_DIFFER:
		if ((option_mask32 & FLAG(q)) || status == STATUS_BINARY)
			printf("Files %s and %s differ\n", path[0], path[1]);
		break;
	case STATUS_SAME:
		if (option_mask32 & FLAG(s))
			printf("Files %s and %s are identical\n", path[0], path[1]);
		break;
	}
}

#if ENABLE_FEATURE_DIFF_DIR
struct dlist {
	size_t len;
	int s, e;
	char **dl;
};

/* This function adds a filename to dl, the directory listing. */
static int FAST_FUNC add_to_dirlist(const char *filename,
		struct stat *sb UNUSED_PARAM,
		void *userdata, int depth UNUSED_PARAM)
{
	struct dlist *const l = userdata;
	l->dl = xrealloc_vector(l->dl, 6, l->e);
	/* + 1 skips "/" after dirname */
	l->dl[l->e] = xstrdup(filename + l->len + 1);
	l->e++;
	return TRUE;
}

/* If recursion is not set, this function adds the directory
 * to the list and prevents recursive_action from recursing into it.
 */
static int FAST_FUNC skip_dir(const char *filename,
		struct stat *sb, void *userdata,
		int depth)
{
	if (!(option_mask32 & FLAG(r)) && depth) {
		add_to_dirlist(filename, sb, userdata, depth);
		return SKIP;
	}
	return TRUE;
}

static void diffdir(char *p[2], const char *s_start)
{
	struct dlist list[2];

	memset(&list, 0, sizeof(list));
	for (int i = 0; i < 2; i++) {
		/*list[i].s = list[i].e = 0; - memset did it */
		/*list[i].dl = NULL; */

		/* We need to trim root directory prefix.
		 * Using list.len to specify its length,
		 * add_to_dirlist will remove it. */
		list[i].len = strlen(p[i]);
		recursive_action(p[i], ACTION_RECURSE | ACTION_FOLLOWLINKS,
		                 add_to_dirlist, skip_dir, &list[i], 0);
		/* Sort dl alphabetically.
		 * GNU diff does this ignoring any number of trailing dots.
		 * We don't, so for us dotted files almost always are
		 * first on the list.
		 */
		qsort_string_vector(list[i].dl, list[i].e);
		/* If -S was set, find the starting point. */
		if (!s_start)
			continue;
		while (list[i].s < list[i].e && strcmp(list[i].dl[list[i].s], s_start) < 0)
			list[i].s++;
	}
	/* Now that both dirlist1 and dirlist2 contain sorted directory
	 * listings, we can start to go through dirlist1. If both listings
	 * contain the same file, then do a normal diff. Otherwise, behaviour
	 * is determined by whether the -N flag is set. */
	while (1) {
		char *dp[2];
		int pos;
		int k;

		dp[0] = list[0].s < list[0].e ? list[0].dl[list[0].s] : NULL;
		dp[1] = list[1].s < list[1].e ? list[1].dl[list[1].s] : NULL;
		if (!dp[0] && !dp[1])
			break;
		pos = !dp[0] ? 1 : (!dp[1] ? -1 : strcmp(dp[0], dp[1]));
		k = pos > 0;
		if (pos && !(option_mask32 & FLAG(N)))
			printf("Only in %s: %s\n", p[k], dp[k]);
		else {
			char *fullpath[2], *path[2]; /* if -N */

			for (int i = 0; i < 2; i++) {
				if (pos == 0 || i == k) {
					path[i] = fullpath[i] = concat_path_file(p[i], dp[i]);
					stat(fullpath[i], &stb[i]);
				} else {
					fullpath[i] = concat_path_file(p[i], dp[1 - i]);
					path[i] = (char *)bb_dev_null;
				}
			}
			if (pos)
				stat(fullpath[k], &stb[1 - k]);

			if (S_ISDIR(stb[0].st_mode) && S_ISDIR(stb[1].st_mode))
				printf("Common subdirectories: %s and %s\n", fullpath[0], fullpath[1]);
			else if (!S_ISREG(stb[0].st_mode) && !S_ISDIR(stb[0].st_mode))
				printf("File %s is not a regular file or directory and was skipped\n", fullpath[0]);
			else if (!S_ISREG(stb[1].st_mode) && !S_ISDIR(stb[1].st_mode))
				printf("File %s is not a regular file or directory and was skipped\n", fullpath[1]);
			else if (S_ISDIR(stb[0].st_mode) != S_ISDIR(stb[1].st_mode)) {
				if (S_ISDIR(stb[0].st_mode))
					printf("File %s is a %s while file %s is a %s\n", fullpath[0], "directory", fullpath[1], "regular file");
				else
					printf("File %s is a %s while file %s is a %s\n", fullpath[0], "regular file", fullpath[1], "directory");
			} else
				print_status(diffreg(path), fullpath);

			free(fullpath[0]);
			free(fullpath[1]);
		}
		free(dp[k]);
		list[k].s++;
		if (pos == 0) {
			free(dp[1 - k]);
			list[1 - k].s++;
		}
	}
	if (ENABLE_FEATURE_CLEAN_UP) {
		free(list[0].dl);
		free(list[1].dl);
	}
}
#endif

int diff_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int diff_main(int argc UNUSED_PARAM, char **argv)
{
	int gotstdin = 0;
	char *file[2], *s_start = NULL;
	llist_t *L_arg = NULL;

	INIT_G();

	/* exactly 2 params; collect multiple -L <label>; -U N */
	opt_complementary = "=2:L::U+";
	getopt32(argv, "abdiL:NqrsS:tTU:wu"
			"p" /* ignored (for compatibility) */,
			&L_arg, &s_start, &opt_U_context);
	argv += optind;
	while (L_arg) {
		if (label[0] && label[1])
			bb_show_usage();
		if (label[0]) /* then label[1] is NULL */
			label[1] = label[0];
		label[0] = llist_pop(&L_arg);
	}
	xfunc_error_retval = 2;
	for (int i = 0; i < 2; i++) {
		file[i] = argv[i];
		/* Compat: "diff file name_which_doesnt_exist" exits with 2 */
		if (LONE_DASH(file[i])) {
			fstat(STDIN_FILENO, &stb[i]);
			gotstdin++;
		} else
			xstat(file[i], &stb[i]);
	}
	xfunc_error_retval = 1;
	if (gotstdin && (S_ISDIR(stb[0].st_mode) || S_ISDIR(stb[1].st_mode)))
		bb_error_msg_and_die("can't compare stdin to a directory");

	if (S_ISDIR(stb[0].st_mode) && S_ISDIR(stb[1].st_mode)) {
#if ENABLE_FEATURE_DIFF_DIR
		diffdir(file, s_start);
#else
		bb_error_msg_and_die("no support for directory comparison");
#endif
	} else {
		bool dirfile = S_ISDIR(stb[0].st_mode) || S_ISDIR(stb[1].st_mode);
		bool dir = S_ISDIR(stb[1].st_mode);
		if (dirfile) {
			const char *slash = strrchr(file[!dir], '/');
			file[dir] = concat_path_file(file[dir], slash ? slash + 1 : file[!dir]);
			xstat(file[dir], &stb[dir]);
		}
		/* diffreg can get non-regular files here */
		print_status(gotstdin > 1 ? STATUS_SAME : diffreg(file), file);

		if (dirfile)
			free(file[dir]);
	}

	return exit_status;
}

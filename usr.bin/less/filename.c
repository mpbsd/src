/*
 * Copyright (C) 1984-2012  Mark Nudelman
 * Modified for use with illumos by Garrett D'Amore.
 * Copyright 2014 Garrett D'Amore <garrett@damore.org>
 *
 * You may distribute under the terms of either the GNU General Public
 * License or the Less License, as specified in the README file.
 *
 * For more information, see the README file.
 */

/*
 * Routines to mess around with filenames (and files).
 * Much of this is very OS dependent.
 *
 * Modified for illumos/POSIX -- it uses native glob(3C) rather than
 * popen to a shell to perform the expansion.
 */

#include <sys/stat.h>

#include <glob.h>
#include <stdarg.h>

#include "less.h"

extern int force_open;
extern int secure;
extern int ctldisp;
extern int utf_mode;
extern IFILE curr_ifile;
extern IFILE old_ifile;
extern char openquote;
extern char closequote;

/*
 * Remove quotes around a filename.
 */
char *
shell_unquote(char *str)
{
	char *name;
	char *p;

	name = p = ecalloc(strlen(str)+1, sizeof (char));
	if (*str == openquote) {
		str++;
		while (*str != '\0') {
			if (*str == closequote) {
				if (str[1] != closequote)
					break;
				str++;
			}
			*p++ = *str++;
		}
	} else {
		char *esc = get_meta_escape();
		int esclen = strlen(esc);
		while (*str != '\0') {
			if (esclen > 0 && strncmp(str, esc, esclen) == 0)
				str += esclen;
			*p++ = *str++;
		}
	}
	*p = '\0';
	return (name);
}

/*
 * Get the shell's escape character.
 */
char *
get_meta_escape(void)
{
	char *s;

	s = lgetenv("LESSMETAESCAPE");
	if (s == NULL)
		s = "\\";
	return (s);
}

/*
 * Get the characters which the shell considers to be "metacharacters".
 */
static char *
metachars(void)
{
	static char *mchars = NULL;

	if (mchars == NULL) {
		mchars = lgetenv("LESSMETACHARS");
		if (mchars == NULL)
			mchars = DEF_METACHARS;
	}
	return (mchars);
}

/*
 * Is this a shell metacharacter?
 */
static int
metachar(char c)
{
	return (strchr(metachars(), c) != NULL);
}

/*
 * Must use quotes rather than escape characters for this meta character.
 */
static int
must_quote(char c)
{
	return (c == '\n');
}

/*
 * Insert a backslash before each metacharacter in a string.
 */
char *
shell_quote(const char *s)
{
	const char *p;
	char *r;
	char *newstr;
	int len;
	char *esc = get_meta_escape();
	int esclen = strlen(esc);
	int use_quotes = 0;
	int have_quotes = 0;

	/*
	 * Determine how big a string we need to allocate.
	 */
	len = 1; /* Trailing null byte */
	for (p = s; *p != '\0'; p++) {
		len++;
		if (*p == openquote || *p == closequote)
			have_quotes = 1;
		if (metachar(*p)) {
			if (esclen == 0) {
				/*
				 * We've got a metachar, but this shell
				 * doesn't support escape chars.  Use quotes.
				 */
				use_quotes = 1;
			} else if (must_quote(*p)) {
				/* Opening quote + character + closing quote. */
				len += 3;
			} else {
				/*
				 * Allow space for the escape char.
				 */
				len += esclen;
			}
		}
	}
	/*
	 * Allocate and construct the new string.
	 */
	if (use_quotes) {
		/* We can't quote a string that contains quotes. */
		if (have_quotes)
			return (NULL);
		newstr  = easprintf("%c%s%c", openquote, s, closequote);
	} else {
		newstr = r = ecalloc(len, sizeof (char));
		while (*s != '\0') {
			if (!metachar(*s)) {
				*r++ = *s++;
			} else if (must_quote(*s)) {
				/* Surround the character with quotes. */
				*r++ = openquote;
				*r++ = *s++;
				*r++ = closequote;
			} else {
				/* Escape the character. */
				(void) strlcpy(r, esc, newstr + len - p);
				r += esclen;
				*r++ = *s++;
			}
		}
		*r = '\0';
	}
	return (newstr);
}

/*
 * Return a pathname that points to a specified file in a specified directory.
 * Return NULL if the file does not exist in the directory.
 */
static char *
dirfile(const char *dirname, const char *filename)
{
	char *pathname;
	char *qpathname;
	int f;

	if (dirname == NULL || *dirname == '\0')
		return (NULL);
	/*
	 * Construct the full pathname.
	 */
	pathname = easprintf("%s/%s", dirname, filename);
	/*
	 * Make sure the file exists.
	 */
	qpathname = shell_unquote(pathname);
	f = open(qpathname, O_RDONLY);
	if (f == -1) {
		free(pathname);
		pathname = NULL;
	} else {
		(void) close(f);
	}
	free(qpathname);
	return (pathname);
}

/*
 * Return the full pathname of the given file in the "home directory".
 */
char *
homefile(char *filename)
{
	return (dirfile(lgetenv("HOME"), filename));
}

/*
 * Expand a string, substituting any "%" with the current filename,
 * and any "#" with the previous filename.
 * But a string of N "%"s is just replaced with N-1 "%"s.
 * Likewise for a string of N "#"s.
 * {{ This is a lot of work just to support % and #. }}
 */
char *
fexpand(char *s)
{
	char *fr, *to;
	int n;
	char *e;
	IFILE ifile;

#define	fchar_ifile(c) \
	((c) == '%' ? curr_ifile : (c) == '#' ? old_ifile : NULL)

	/*
	 * Make one pass to see how big a buffer we
	 * need to allocate for the expanded string.
	 */
	n = 0;
	for (fr = s; *fr != '\0'; fr++) {
		switch (*fr) {
		case '%':
		case '#':
			if (fr > s && fr[-1] == *fr) {
				/*
				 * Second (or later) char in a string
				 * of identical chars.  Treat as normal.
				 */
				n++;
			} else if (fr[1] != *fr) {
				/*
				 * Single char (not repeated).  Treat specially.
				 */
				ifile = fchar_ifile(*fr);
				if (ifile == NULL)
					n++;
				else
					n += strlen(get_filename(ifile));
			}
			/*
			 * Else it is the first char in a string of
			 * identical chars.  Just discard it.
			 */
			break;
		default:
			n++;
			break;
		}
	}

	e = ecalloc(n+1, sizeof (char));

	/*
	 * Now copy the string, expanding any "%" or "#".
	 */
	to = e;
	for (fr = s; *fr != '\0'; fr++) {
		switch (*fr) {
		case '%':
		case '#':
			if (fr > s && fr[-1] == *fr) {
				*to++ = *fr;
			} else if (fr[1] != *fr) {
				ifile = fchar_ifile(*fr);
				if (ifile == NULL) {
					*to++ = *fr;
				} else {
					(void) strlcpy(to, get_filename(ifile),
					    e + n + 1 - to);
					to += strlen(to);
				}
			}
			break;
		default:
			*to++ = *fr;
			break;
		}
	}
	*to = '\0';
	return (e);
}

/*
 * Return a blank-separated list of filenames which "complete"
 * the given string.
 */
char *
fcomplete(char *s)
{
	char *fpat;
	char *qs;

	if (secure)
		return (NULL);
	/*
	 * Complete the filename "s" by globbing "s*".
	 */
	fpat =  easprintf("%s*", s);

	qs = lglob(fpat);
	s = shell_unquote(qs);
	if (strcmp(s, fpat) == 0) {
		/*
		 * The filename didn't expand.
		 */
		free(qs);
		qs = NULL;
	}
	free(s);
	free(fpat);
	return (qs);
}

/*
 * Try to determine if a file is "binary".
 * This is just a guess, and we need not try too hard to make it accurate.
 */
int
bin_file(int f)
{
	char data[256];
	ssize_t i, n;
	wchar_t ch;
	int bin_count, len;

	if (!seekable(f))
		return (0);
	if (lseek(f, (off_t)0, SEEK_SET) == (off_t)-1)
		return (0);
	n = read(f, data, sizeof (data));
	bin_count = 0;
	for (i = 0; i < n; i += len) {
		len = mbtowc(&ch, data + i, n - i);
		if (len <= 0) {
			bin_count++;
			len = 1;
		} else if (iswprint(ch) == 0 && iswspace(ch) == 0 &&
		    data[i] != '\b' &&
		    (ctldisp != OPT_ONPLUS || data[i] != ESC))
			bin_count++;
	}
	/*
	 * Call it a binary file if there are more than 5 binary characters
	 * in the first 256 bytes of the file.
	 */
	return (bin_count > 5);
}

/*
 * Expand a filename, doing any system-specific metacharacter substitutions.
 */
char *
lglob(char *filename)
{
	char *gfilename;
	char *ofilename;
	glob_t list;
	int i;
	int length;
	char *p;
	char *qfilename;

	ofilename = fexpand(filename);
	if (secure)
		return (ofilename);
	filename = shell_unquote(ofilename);

	/*
	 * The globbing function returns a list of names.
	 */

#ifndef	GLOB_TILDE
#define	GLOB_TILDE	0
#endif
#ifndef	GLOB_LIMIT
#define	GLOB_LIMIT	0
#endif
	if (glob(filename, GLOB_TILDE | GLOB_LIMIT, NULL, &list) != 0) {
		free(filename);
		return (ofilename);
	}
	length = 1; /* Room for trailing null byte */
	for (i = 0; i < list.gl_pathc; i++) {
		p = list.gl_pathv[i];
		qfilename = shell_quote(p);
		if (qfilename != NULL) {
			length += strlen(qfilename) + 1;
			free(qfilename);
		}
	}
	gfilename = ecalloc(length, sizeof (char));
	for (i = 0; i < list.gl_pathc; i++) {
		p = list.gl_pathv[i];
		qfilename = shell_quote(p);
		if (qfilename != NULL) {
			if (i != 0) {
				(void) strlcat(gfilename, " ", length);
			}
			(void) strlcat(gfilename, qfilename, length);
			free(qfilename);
		}
	}
	globfree(&list);
	free(filename);
	free(ofilename);
	return (gfilename);
}

/*
 * Is the specified file a directory?
 */
int
is_dir(char *filename)
{
	int isdir = 0;
	int r;
	struct stat statbuf;

	filename = shell_unquote(filename);

	r = stat(filename, &statbuf);
	isdir = (r >= 0 && S_ISDIR(statbuf.st_mode));
	free(filename);
	return (isdir);
}

/*
 * Returns NULL if the file can be opened and
 * is an ordinary file, otherwise an error message
 * (if it cannot be opened or is a directory, etc.)
 */
char *
bad_file(char *filename)
{
	char *m = NULL;

	filename = shell_unquote(filename);
	if (!force_open && is_dir(filename)) {
		m = easprintf("%s is a directory", filename);
	} else {
		int r;
		struct stat statbuf;

		r = stat(filename, &statbuf);
		if (r == -1) {
			m = errno_message(filename);
		} else if (force_open) {
			m = NULL;
		} else if (!S_ISREG(statbuf.st_mode)) {
			m = easprintf("%s is not a regular file (use -f to "
			    "see it)", filename);
		}
	}
	free(filename);
	return (m);
}

/*
 * Return the size of a file, as cheaply as possible.
 */
off_t
filesize(int f)
{
	struct stat statbuf;

	if (fstat(f, &statbuf) >= 0)
		return (statbuf.st_size);
	return (-1);
}

/*
 * Return last component of a pathname.
 */
char *
last_component(char *name)
{
	char *slash;

	for (slash = name + strlen(name); slash > name; ) {
		--slash;
		if (*slash == '/')
			return (slash + 1);
	}
	return (name);
}

/*
 *  lexical analyzer
 */

#include <ctype.h>         /* isdigit, isxdigit, isprint */
#include <errno.h>         /* errno, ERANGE */
#include <float.h>         /* LDBL_MAX (when !defined(HUGE_VALL)) */
#include <limits.h>        /* ULONG_MAX, LONG_MAX, CHAR_BIT */
#include <math.h>          /* HUGE_VALL */
#include <stddef.h>        /* size_t, NULL */
#include <stdio.h>         /* sprintf */
#include <stdlib.h>        /* strtold */
#include <string.h>        /* strlen, strchr, memcpy, memset */
#include <cbl/arena.h>     /* ARENA_ALLOC */
#include <cbl/assert.h>    /* assert */
#include <cbl/memory.h>    /* MEM_RESIZE */
#include <cdsl/hash.h>     /* hash_new */
#ifdef HAVE_ICONV
#include <iconv.h>         /* iconv_t */
#endif    /* HAVE_ICONV */

#include "common.h"
#include "err.h"
#include "in.h"
#include "ir.h"
#include "main.h"
#include "strg.h"
#include "sym.h"
#include "ty.h"
#include "lex.h"

/* ASSUMPTION: HUGE_VALL is greater than LDBL_MAX */
#ifdef HUGE_VALL
#define OVF(ld) ((ld) == HUGE_VALL)
#else    /* !HUGE_VALL */
#define OVF(ld) ((ld) > LDBL_MAX)
#endif    /* HUGE_VALL */


/* declares strtold() when no declaration is visible */
#ifndef HUGE_VALL
long double strtold(const char *, char **);
#endif    /* !HUGE_VALL */


int lex_tc;                      /* token code for current token */
const char *lex_tok;             /* string representation of current token */
sym_t *lex_sym;                  /* symbol table entry for current token */
lex_pos_t lex_cpos;              /* locus of current token */
lex_pos_t lex_ppos;              /* locus of previous token */
lex_buf_t lex_buf = {            /* buffer set for recognizing strings */
    NULL, TL_STR,
    NULL, TL_STR,
    NULL, 10
};
lex_buf_t *lex_bp = &lex_buf;    /* pointer to current buffer set */

/* token names */
const char * const lex_name[] = {
#define xx(a, b, c, d, e, f, g, h) g,
#define yy(a, b, c, d, e, f, g, h) g,
#include "xtoken.h"
};

/* token kinds */
const char lex_kind[] = {
#define xx(a, b, c, d, e, f, g, h) f,
#define yy(a, b, c, d, e, f, g, h) f,
#include "xtoken.h"
};


static int endian = 1;    /* for LITTLE from common.h */
static sym_t tval;        /* symbol value for current token */


/*
 *  converts a locus to a string;
 *  should not be invoked more than once in the same call;
 *  see also ty_outtype()
 */
const char *(lex_outpos)(const lex_pos_t *src)
{
    static char buf[80];

    size_t len;
    char *pbuf = buf;

    assert(src);
    assert(src->f);

    len = strlen(src->f) + 2 + BUFN*2 + 1;    /* file:y:x */
    if (sizeof(buf) < len)
        pbuf = ARENA_ALLOC(strg_stmt, len);
    sprintf(pbuf, "%s:%lu:%lu", src->f, src->y, src->x);

    return pbuf;
}


/*
 *  makes in_cp point to the next non-space character
 */
int (lex_getchr)(void)
{
    assert(in_cp);
    assert(in_limit);

    while (1) {
        IN_SKIPSP(in_cp);
        if (*in_cp != '\n')
            return *in_cp;
        in_cp++;
        in_nextline();
        if (in_cp == in_limit)
            return LEX_EOI;
    }
}


/*
 *  checks if a pp-number is wholly consumed
 */
static void ppnumber(int tok)
{
    const unsigned char *rcp = in_cp;

    assert(rcp);
    assert(in_limit);
    assert(tok == LEX_ICON || tok == LEX_FCON);

    while (ISCH_APN(*in_cp)) {
        if (*in_cp == '\n')
            IN_FILLBREAK(in_cp) /* ; */
        if (in_limit - in_cp < 2)
            in_fillbuf();
        if ((in_cp[0] == 'e' || in_cp[0] == 'E') && (in_cp[1] == '+' || in_cp[1] == '-'))
            in_cp++;
        assert(in_cp < in_limit);
        in_cp++;
    }

    if (in_cp > rcp)
        err_issuex(0, ERR_CONST_PPNUMBER, tok);
}


#define N 0    /* no suffix */
#define U 1    /* suffix: U */
#define L 2    /* suffix: L */
#define X 3    /* suffix: UL or LU */
#define D 0    /* radix: decimal */
#define H 1    /* radix: octal or hexadecimal */

/*
 *  recognizes integer constants
 */
static sym_t *icon(unsigned long n, int ovf, int radix)
{
    int suffix;
    struct {
        unsigned long limit;
        ty_t *type;
    } tab[X+1][H+1][5], *p;

    assert(ty_inttype);    /* ensures types initialized */
    assert(ULONG_MAX >= TG_ULONG_MAX);
    assert(LONG_MAX >= TG_LONG_MAX);

    /* no suffix, decimal */
    tab[N][D][0].limit = TG_INT_MAX;
    tab[N][D][0].type  = ty_inttype;
    tab[N][D][1].limit = TG_LONG_MAX;
    tab[N][D][1].type  = ty_longtype;
    tab[N][D][2].limit = TG_ULONG_MAX;
    tab[N][D][2].type  = ty_ulongtype;
    tab[N][D][3].limit = ULONG_MAX;
    tab[N][D][3].type  = ty_inttype;

    /* no suffix, octal or hex */
    tab[N][H][0].limit = TG_INT_MAX;
    tab[N][H][0].type  = ty_inttype;
    tab[N][H][1].limit = TG_UINT_MAX;
    tab[N][H][1].type  = ty_unsignedtype;
    tab[N][H][2].limit = TG_LONG_MAX;
    tab[N][H][2].type  = ty_longtype;
    tab[N][H][3].limit = TG_ULONG_MAX;
    tab[N][H][3].type  = ty_ulongtype;
    tab[N][H][4].limit = ULONG_MAX;
    tab[N][H][4].type  = ty_inttype;

    /* U, decimal, octal or hex */
    tab[U][H][0].limit = tab[U][D][0].limit = TG_UINT_MAX;
    tab[U][H][0].type  = tab[U][D][0].type  = ty_unsignedtype;
    tab[U][H][1].limit = tab[U][D][1].limit = TG_ULONG_MAX;
    tab[U][H][1].type  = tab[U][D][1].type  = ty_ulongtype;
    tab[U][H][2].limit = tab[U][D][2].limit = ULONG_MAX;
    tab[U][H][2].type  = tab[U][D][2].type  = ty_inttype;

    /* L, decimal, octal or hex */
    tab[L][H][0].limit = tab[L][D][0].limit = TG_LONG_MAX;
    tab[L][H][0].type  = tab[L][D][0].type  = ty_longtype;
    tab[L][H][1].limit = tab[L][D][1].limit = TG_ULONG_MAX;
    tab[L][H][1].type  = tab[L][D][1].type  = ty_ulongtype;
    tab[L][H][2].limit = tab[L][D][2].limit = ULONG_MAX;
    tab[L][H][2].type  = tab[L][D][2].type  = ty_inttype;

    /* UL or LU, decimal, octal or hex */
    tab[X][H][0].limit = tab[X][D][0].limit = TG_ULONG_MAX;
    tab[X][H][0].type  = tab[X][D][0].type  = ty_ulongtype;
    tab[X][H][1].limit = tab[X][D][1].limit = ULONG_MAX;
    tab[X][H][1].type  = tab[X][D][1].type  = ty_inttype;

    assert(in_limit);
    assert(in_cp);

    radix = (radix == 10)? D: H;
    if (in_limit - in_cp < 2)    /* no need to preserve lex_tok */
        in_fillbuf();
    suffix = 0;
    if (((in_cp[0] == 'u' || in_cp[0] == 'U') && (in_cp[1] == 'l' || in_cp[1] == 'L')) ||
        ((in_cp[0] == 'l' || in_cp[0] == 'L') && (in_cp[1] == 'u' || in_cp[1] == 'U')))
        in_cp += 2, suffix = X;
    else if (*in_cp == 'u' || *in_cp == 'U')
        in_cp++, suffix = U;
    else if (*in_cp == 'l' || *in_cp == 'L')
        in_cp++, suffix = L;

    for (p = tab[suffix][radix]; n > p->limit; p++)
        continue;
    if (ovf || (p->limit == ULONG_MAX && p->type == ty_inttype)) {
        err_issuex(0, ERR_CONST_LARGEINT);
        n = TG_ULONG_MAX;
        tval.type = ty_ulongtype;
    } else
        tval.type = p->type;

    if (tval.type->op == TY_INT || tval.type->op == TY_LONG)
        tval.u.c.v.li = n;
    else    /* tval.type->op == TY_UNSIGNED || tval.type->op == TY_ULONG */
        tval.u.c.v.ul = n;

    ppnumber(LEX_ICON);

    return &tval;
}

#undef H
#undef D
#undef X
#undef L
#undef U
#undef N


/*
 *  recognizes floating constants;
 *  ASSUMPTION: fp types of the host are same as those of the target;
 *  ASSUMPTION: strtold() supported on the host
 */
static sym_t *fcon(int toolong)
{
    long double ld;

    assert(in_cp);
    assert(in_limit);
    assert(ty_floattype);    /* ensures types initialized */

    assert(*in_cp == '.' || *in_cp == 'e' || *in_cp == 'E');
    if (*in_cp == '.')
        do {
            if (*in_cp == '\n') {
                if (in_cp < in_limit)
                    break;
                toolong = 1;
                in_fillbuf();
                if (in_cp == in_limit)
                    break;
                continue;
            }
            in_cp++;
        } while(ISCH_DN(*in_cp));
    if (*in_cp == 'e' || *in_cp == 'E') {
        const unsigned char *rcp;
        if (in_limit - in_cp < 3) {
            toolong = 1;
            in_fillbuf();
        }
        assert(in_cp < in_limit);
        rcp = in_cp + 1;    /* skips e or E */
        if (*rcp == '-' || *rcp == '+')
            rcp++;
        if (isdigit(*rcp)) {
            in_cp = rcp;
            do {
                if (*in_cp == '\n') {
                    if (in_cp < in_limit)
                        break;
                    toolong = 1;
                    in_fillbuf();
                    if (in_cp == in_limit)
                        break;
                    continue;
                }
                in_cp++;
            } while(ISCH_DN(*in_cp));
        } else
            err_issue(ERR_CONST_ILLFPCNST);
    }
    if (toolong)    /* lex_tok might be useless */
        err_issuex(0, ERR_CONST_LONGFP);
    else {
        errno = 0;
        ld = strtold(lex_tok, NULL);
    }

    switch(*in_cp) {
        case 'f':
        case 'F':
            in_cp++;
            if (toolong)
                ld = TG_FLT_NAN;
            else if ((OVF(ld) && errno == ERANGE) || ld > TG_FLT_MAX) {
                err_issuex(0, ERR_CONST_LARGEFP);
                ld = TG_FLT_MAX;
            } else if ((ld == 0.0 && errno == ERANGE) || (ld > 0.0 && ld < TG_FLT_MIN)) {
                err_issuex(0, ERR_CONST_TRUNCFP);
                ld = 0.0f;
            }
            tval.type = ty_floattype;
            tval.u.c.v.f = (float)ld;
            break;
        case 'l':
        case 'L':
            in_cp++;
            if (toolong)
                ld = TG_LDBL_NAN;
            else if ((OVF(ld) && errno == ERANGE) || ld > TG_LDBL_MAX) {
                err_issuex(0, ERR_CONST_LARGEFP);
                ld = TG_LDBL_MAX;
            } else if ((ld == 0.0 && errno == ERANGE) || (ld > 0.0 && ld < TG_LDBL_MIN))
                err_issuex(0, ERR_CONST_TRUNCFP);
            tval.type = ty_ldoubletype;
            tval.u.c.v.ld = (long double)ld;
            break;
        default:
            if (toolong)
                ld = TG_DBL_NAN;
            else if ((OVF(ld) && errno == ERANGE) || ld > TG_DBL_MAX) {
                err_issuex(0, ERR_CONST_LARGEFP);
                ld = (double)TG_DBL_MAX;
            } else if ((ld == 0.0 && errno == ERANGE) || (ld > 0.0 && ld < TG_DBL_MIN)) {
                err_issuex(0, ERR_CONST_TRUNCFP);
                ld = 0.0;
            }
            tval.type = ty_doubletype;
            tval.u.c.v.d = (double)ld;
            break;
    }

    ppnumber(LEX_FCON);

    return &tval;
}


/*
 *  recognizes an escape sequence
 */
static unsigned long backslash(unsigned char **pp, int stopidx, int w)
{
    int c;
    int ovf;
    unsigned long n, lim;
    const char *hex = "0123456789abcdef";
    unsigned char *p = &lex_bp->s.p[lex_bp->t.p[stopidx--].idx];

    assert(pp);
    assert(*pp);
    assert(stopidx >= 0);
    assert(ty_uchartype);    /* ensures types initialized */
    assert(ULONG_MAX >= TG_UCHAR_MAX);
    assert(ULONG_MAX >= TG_WUCHAR_MAX);

    lim = (w)? TG_WUCHAR_MAX: TG_UCHAR_MAX;

    switch(*(*pp)++) {
        case 'a':
            return '\a';
        case 'b':
            return '\b';
        case 'f':
            return '\f';
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        case 'v':
            return '\v';
        case '\'':
        case '"':
        case '\\':
        case '\?':
            break;
        case 'x':    /* \xh...h */
            ovf = 0;
            if (isxdigit(**pp) == 0) {
                err_issuep(&lex_bp->t.p[stopidx].pos, ERR_CONST_ILLHEXESC);
                return 0;
            }
            c = n = 0;
            do {
                c = strchr(hex, tolower(*(*pp)++)) - hex;
                if (n & ~(lim >> 4))
                    ovf = 1;
                else
                    n = (n << 4) + c;
            } while(*pp < p && isxdigit(**pp));
            if (ovf) {
                err_issuep(&lex_bp->t.p[stopidx].pos, ERR_CONST_LARGEHEX);
                n = lim;
            }
            return n & lim;
        case '0':    /* \ooo */
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
            c = 1;
            n = (*pp)[-1] - '0';
            if (*pp < p && **pp >= '0' && **pp <= '7') {
                n = (n << 3) + (*(*pp)++ - '0'), c++;
                if (*pp < p && **pp >= '0' && **pp <= '7')
                    n = (n << 3) + (*(*pp)++ - '0'), c++;
            }
            if (isdigit((*pp)[0])) {
                lex_pos_t pos = lex_bp->t.p[stopidx].pos;
                pos.x += c + 1;
                if (c < 3 && ((*pp)[0] == '8' || (*pp)[0] == '9'))
                    err_issuep(&pos, ERR_CONST_ESCOCT89);
                else if (c == 3)
                    err_issuep(&pos, ERR_CONST_ESCOCT3DIG);
            }
            if (n > lim) {
                err_issuep(&lex_bp->t.p[stopidx].pos, ERR_CONST_LARGEOCT);
                n = lim;
            }
            return n & lim;
        case '\n':
            err_issuep(&lex_bp->t.p[stopidx].pos, ERR_INPUT_LINESPLICE);
            break;
            /* no break */
        default:
            err_issuep(&lex_bp->t.p[stopidx].pos, ERR_CONST_UNKNOWNESC);
            break;
    }

    return (*pp)[-1];
}


/*
 *  recognizes string literals;
 *  not reentrant, so cannot be directly used in resync();
 *  ASSUMPTION: char on the host has the same number of bits as on the target
 */
unsigned long (lex_scon)(int q, int *w, int linep)
{
    int cbyte;
    int i, stopidx;
    unsigned char *p, *pp;
    unsigned long len, clen;
#ifdef HAVE_ICONV
    iconv_t *cd;
#endif    /* HAVE_ICONV */

    assert(lex_bp->s.p);
    assert(lex_bp->t.p);
    assert(in_cp);
    assert(in_limit);
    assert(BUFUNIT > 2);
    assert(!*w || ty_wchartype);    /* ensures types initialized */
    assert(ir_cur);

    stopidx = 0;
    p = lex_bp->s.p;
    do {
        in_cp++;    /* skips ' or " */
        lex_bp->t.p[stopidx].idx = p - lex_bp->s.p;
        lex_bp->t.p[stopidx].pos.c = in_cpos.c;
        lex_bp->t.p[stopidx].pos.fy = in_cpos.fy;
        lex_bp->t.p[stopidx].pos.f = in_cpos.f;
        lex_bp->t.p[stopidx].pos.y = in_cpos.y;
        lex_bp->t.p[stopidx++].pos.x = in_cp - in_line + in_outlen + 1;
        while (*in_cp != q) {
            int c;
            if (*in_cp == '\n')
                IN_FILLBREAK(in_cp) /* ; */
            c = *in_cp++;
            if (stopidx + 2 > lex_bp->t.n)    /* esc seq and (adj str or end) */
                MEM_RESIZE(lex_bp->t.p, (lex_bp->t.n += 10) * sizeof(*lex_bp->t.p));
            if ((len=p-lex_bp->s.p) + 2 > lex_bp->s.n) {
                MEM_RESIZE(lex_bp->s.p, (lex_bp->s.n += BUFUNIT) + 1);    /* +1 for NUL */
                p = lex_bp->s.p + len;
            }
            if (c == '\\') {
                if (linep)
                    in_cp--, err_issue(ERR_PP_ESCINFNAME), in_cp++;
                if (in_limit - in_cp < 2)
                    in_fillbuf();
                lex_bp->t.p[stopidx].idx = p - lex_bp->s.p;
                lex_bp->t.p[stopidx].pos = lex_bp->t.p[stopidx-1].pos;
                lex_bp->t.p[stopidx].pos.x += (p - lex_bp->s.p - lex_bp->t.p[stopidx-1].idx);
                stopidx++;
                *p++ = c;
                c = *in_cp;
                if (c != '\n')
                    in_cp++;
            }
            *p++ = c;
        }
        if (*in_cp == q)
            in_cp++;
        else
            err_issue(ERR_PP_UNCLOSESTR, q);
        if (!linep && q == '"') {
            if (lex_getchr() == 'L') {
                if (in_limit - in_cp < 2)
                    in_fillbuf();
                assert(in_cp < in_limit);
                if (in_cp[1] == '"') {
                    in_cp++;    /* skips L */
                    if (!*w) {    /* mb + wide = wide */
                        *w = 1;
                        in_cp--;
                        err_issue(ERR_CONST_MBWIDE);
                        err_issue(ERR_CONST_MBWIDESTD);
                        in_cp++;
                    }
                }
            } else if (*in_cp == '"') {
                if (*w) {    /* wide + mb = wide */
                    err_issue(ERR_CONST_MBWIDE);
                    err_issue(ERR_CONST_MBWIDESTD);
                }
            }
        }
    } while (q == '"' && *in_cp == '"');
    lex_tok = (char *)p;    /* used in printtok() */
    *p++ = 0;    /* no buffer overrun; see strg.c and lex_bp->s.n */
    /* need not remember end position */
    lex_bp->t.p[stopidx++].idx = p - lex_bp->s.p;

    /* converts string literal */
    cbyte = (!*w)? 1: ty_wchartype->size;
    assert(BUFUNIT >= cbyte);

#ifdef HAVE_ICONV
    cd = (linep)? main_ntoc:
         (!*w)? main_ntoe: main_ntow;
    clen = 0;
#endif    /* HAVE_ICONV */
    len = 0;
    pp = lex_bp->s.p;
    i = 1;    /* lex_bp->t.p[0] is always start */
    while (i < stopidx) {
        p = &lex_bp->s.p[lex_bp->t.p[i].idx];
        if (p == pp) {
            i++;
            continue;
        }
#ifdef HAVE_ICONV
        if (cd && i > 0) {
            ICONV_DECL(NULL, 0);
            UNUSED(ilenv);
            UNUSED(ibufv);
            obuf = (char *)lex_bp->b.p, obufv = (char *)lex_bp->b.p + len;
            olen = lex_bp->b.n, olenv = lex_bp->b.n - len;
            ICONV_INIT(cd);
            lex_bp->b.p = (unsigned char *)obuf, lex_bp->b.n = olen;
            len += (lex_bp->b.n - len - olenv);
        }
#endif    /* HAVE_ICONV */
        if (*pp == '\\') {    /* escape sequences */
            unsigned long c;

            assert(sizeof(c) >= cbyte);
            pp++;    /* skips \ */
            c = backslash(&pp, i, *w);
            if (len + cbyte > lex_bp->b.n)
                MEM_RESIZE(lex_bp->b.p, lex_bp->b.n += BUFUNIT);
            if (*w) {
                if (LITTLE != ir_cur->f.little_endian)
                    CHGENDIAN(c, sizeof(c));
                memcpy(lex_bp->b.p+len, &c + ((ir_cur->f.little_endian)? 0: sizeof(c)-cbyte),
                       cbyte);
                len += cbyte;
            } else {
                lex_bp->b.p[len++] = c;
#ifdef HAVE_ICONV
                clen++;
#endif    /* HAVE_ICONV */
            }
        } else {    /* ordinary characters */
#ifdef HAVE_ICONV
            if (cd) {
                ICONV_DECL((char *)pp, p - pp);
                obuf = (char *)lex_bp->b.p, obufv = (char *)lex_bp->b.p + len;
                olen = lex_bp->b.n, olenv = lex_bp->b.n - len;
                ICONV_DO(cd, 1, { pos = lex_bp->t.p[i-1].pos;
                                  pos.x += (ibufv - (char *)&lex_bp->s.p[lex_bp->t.p[i-1].idx]);
                                  lex_bp->b.p = (unsigned char *)obuf;
                                  err_issuep(&pos, ERR_CONST_CONVFAIL); });
                lex_bp->b.p = (unsigned char *)obuf, lex_bp->b.n = olen;    /* for later reuse */
                len += (lex_bp->b.n - len - olenv);
            } else {
#else    /* !HAVE_ICONV */
            {
#endif    /* HAVE_ICONV */
                unsigned long d;

                assert(TG_CHAR_BIT == CHAR_BIT);
                assert(p > pp);
                d = p - pp;
                while (len + (d*cbyte) > lex_bp->b.n)    /* rarely iterates */
                    MEM_RESIZE(lex_bp->b.p, lex_bp->b.n += BUFUNIT);
                if (*w) {
                    while (pp < p) {
                        lex_bp->b.p[len + ((ir_cur->f.little_endian)? 0: cbyte-1)] = *pp++;
                        memset(lex_bp->b.p + len + ((ir_cur->f.little_endian)? 1: 0), 0, cbyte-1);
                        len += cbyte;
                    }
                } else {
                    memcpy(&lex_bp->b.p[len], pp, d);
                    len += d;
                }
            }
#ifdef HAVE_ICONV
            if (main_iton && !*w)
                clen += in_cntchar(pp, p);
#endif    /* HAVE_ICONV */
            i++;
            pp = p;
        }
    }

    if (len % cbyte != 0)
        err_issuex(0, ERR_CONST_WIDENOTFIT);
#ifdef HAVE_ICONV
    if (!main_iton || *w)
#endif    /* HAVE_ICONV */
        clen = len /= cbyte;

    if (clen - 1 > TL_STR_STD && q == '"' && !linep) {    /* note TL_STR_STD is of unsigned long */
        err_issuex(0, ERR_CONST_LONGSTR);
        err_issuex(0, ERR_CONST_LONGSTRSTD, (unsigned long)TL_STR_STD);
    }

    return len;
}


/*
 *  retrieves a token;
 *  ASSUMPTION: signed integers are compatible with unsigned ones on the host
 */
int (lex_next)(void)
{
    int w;

    assert(in_cp);
    assert(in_limit);
    assert(ty_chartype);    /* ensures types initialized */
    assert(ir_cur);
    assert(ULONG_MAX >= TG_ULONG_MAX);

    lex_ppos = lex_cpos;

    while (1) {
        register const unsigned char *rcp = in_cp;

        IN_SKIPSP(rcp);

        if (in_limit - rcp < IN_MAXTOKEN)
            in_cp = rcp, in_fillbuf(), rcp = in_cp;

        lex_cpos.c = in_cpos.c;
        lex_cpos.fy = in_cpos.fy;
        lex_cpos.f = in_cpos.f;
        lex_cpos.y = in_cpos.y;
        lex_cpos.x = rcp-in_line + in_outlen + 1;

        in_cp = rcp + 1;
        switch(*rcp++) {
            /* whitespaces */
            case '\n':
                in_nextline();
                if (in_cp == in_limit)
                    return LEX_EOI;
                continue;
            case '\v':
            case '\f':
            case '\r':
            case '\t':
            case ' ':
                assert(!"missed whitespace -- should never reach here");
                continue;
            /* punctuations */
            case '!':    /* != and ! */
                if (*rcp == '=')
                    return (in_cp++, LEX_NEQ);    /* != */
                return '!';
            case '"':    /* string literal */
                w = 0;
            strlit:
                {
                    unsigned long len = lex_scon(*--in_cp, &w, 0);
                    err_entersite(&lex_cpos);    /* enters with " or L */
                    tval.type = ty_array_s((!w)? ty_chartype: ty_wchartype, len);
                    err_exitsite();    /* exits from " or L */
                    tval.u.c.v.hp = lex_bp->b.p;
                    lex_sym = &tval;
                }
                return LEX_SCON;
            case '#':    /* ## and # */
                err_issuex(0, ERR_LEX_SHARP);
                if (*rcp == '#')
                    in_cp++;
                continue;
            case '%':    /* %>, %: and % */
                if (*rcp == '>')
                    return (in_cp++, '}');
                if (*rcp == ':') {
                    err_issuex(0, ERR_LEX_SHARP);
                    in_cp += (rcp[1] == '%' && rcp[2] == ':')? 3: 1;
                    continue;
                }
                return '%';
            case '&':    /* && and & */
                if (*rcp == '&')
                    return (in_cp++, LEX_ANDAND);    /* && */
                return '&';
            case '\'':    /* character constant */
                w = 0;
            charconst:
                {
                    unsigned long len = lex_scon(*--in_cp, &w, 0);
                    if (len < 2)
                        err_issuex(0, ERR_CONST_EMPTYCHAR);
                    else if (len > 2)
                        err_issuex(0, ERR_CONST_LARGECHAR);
                    tval.type = (!w)? ty_inttype: ty_wchartype;
                    if (w) {
                        tval.u.c.v.ul = 0;
                        if (LITTLE != ir_cur->f.little_endian)
                            CHGENDIAN(lex_bp->b.p[0], ty_wchartype->size);
                        memcpy((char *)&tval.u.c.v.ul +
                                   ((ir_cur->f.little_endian)?
                                       0: sizeof(tval.u.c.v.ul)-ty_wchartype->size),
                               lex_bp->b.p, ty_wchartype->size);
                    } else
                        tval.u.c.v.li = (main_opt()->uchar)? *lex_bp->b.p:
                                                             SYM_CROPSC(*lex_bp->b.p);
                    lex_sym = &tval;
                }
                return LEX_CCON;
            case '+':    /* ++ and + */
                if (*rcp == '+')
                    return (in_cp++, LEX_INCR);    /* ++ */
                return '+';
            case '-':    /* ->, -- and - */
                if (*rcp == '>')
                    return (in_cp++, LEX_DEREF);    /* -> */
                if (*rcp == '-')
                    return (in_cp++, LEX_DECR);    /* -- */
                return '-';
            case '.':    /* ..., . and fp consts */
                if (rcp[0] == '.' && rcp[1] == '.')
                    return (in_cp += 2, LEX_ELLIPSIS);
                if (!isdigit(*rcp))
                    return '.';
                if (in_limit - in_cp < IN_MAXLINE)    /* apprx max length of fp const */
                    in_cp = rcp-1, in_fillbuf(), rcp = ++in_cp;
                in_cp = rcp - 1;
                lex_tok = (char *)in_cp;
                lex_sym = fcon(0);
                return LEX_FCON;
            case '/':    /* comments, //-comments and / */
                if (*rcp == '*') {    /* comments */
                    int c = 0;
                    rcp++;    /* skips * */
                    while (!(c == '*' && *rcp == '/')) {
                        if (c == '/' && *rcp == '*') {
                            in_cp = rcp - 1;
                            err_issue(ERR_PP_CMTINCMT);
                        }
                        if (*rcp == '\n') {
                            if (rcp < in_limit)
                                c = *rcp;
                            in_cp = rcp + 1;
                            in_nextline();
                            rcp = in_cp;
                            if (rcp == in_limit)
                                break;
                        } else
                            c = *rcp++;
                    }
                    in_cp = rcp;
                    if (in_cp < in_limit)
                        in_cp++;
                    else
                        err_issue(ERR_PP_UNCLOSECMT);
                    continue;
                }
                if (*rcp == '/') {
                    if (main_opt()->std == 1)
                        in_cp--, err_issue(ERR_PP_C99CMT), in_cp++;
                    else {    /* //-commnets supported */
                        IN_DISCARD(rcp);
                        in_cp = rcp;
                        in_nextline();
                        continue;
                    }
                }
                return '/';
            case ':':    /* :> and : */
                if (*rcp == '>')
                    return (in_cp++, ']');
                return ':';
            case '<':    /* <=, <<, <:, <% and < */
                switch(*rcp) {
                    case '=':
                        return (in_cp++, LEX_LEQ);    /* <= */
                    case '<':
                        return (in_cp++, LEX_LSHFT);    /* << */
                    case ':':
                        return (in_cp++, '[');
                    case '%':
                        return (in_cp++, '{');
                }
                return '<';
            case '=':    /* == and = */
                if (*rcp == '=')
                    return (in_cp++, LEX_EQEQ);    /* == */
                return '=';
            case '>':    /* >=, >> and > */
                if (*rcp == '=')
                    return (in_cp++, LEX_GEQ);    /* >= */
                if (*rcp == '>')
                    return (in_cp++, LEX_RSHFT);    /* >> */
                return '>';
            case '\\':    /* \ and line splicing */
                err_issuex(0, (*rcp == '\n')? ERR_INPUT_LINESPLICE: ERR_LEX_STRAYBS);
                continue;
            case '|':    /* || and | */
                if (*rcp == '|')
                    return (in_cp++, LEX_OROR);    /* || */
                return '|';
            case '(':    /* one-char tokens */
            case ')':
            case '*':
            case ',':
            case ';':
            case '?':
            case '~':
            case '[':
            case ']':
            case '^':
            case '{':
            case '}':
                return rcp[-1];
            /* digits */
            case '0':    /* int and fp consts */
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                {
                    unsigned long n = 0;
                    if (in_limit - rcp < IN_MAXLINE)    /* apprx max length of fp const */
                        in_cp = rcp-1, in_fillbuf(), rcp = ++in_cp;
                    lex_tok = (char *)rcp - 1;
                    if (*lex_tok == '0' && (*rcp == 'x' || *rcp == 'X')) {
                        int d, ovf = 0;
                        const char *hex = "0123456789abcdef";
                        rcp++;    /* skips x or X */
                        if (isxdigit(*rcp))
                            do {
                                if (*rcp == '\n')
                                    IN_FILLBREAK(rcp) /* ; */
                                d = strchr(hex, tolower(*rcp++)) - hex;
                                if (n & ~(ULONG_MAX >> 4))
                                    ovf = 1;
                                else
                                    n = (n << 4) + d;
                            } while(ISCH_XN(*rcp));
                        else
                            in_cp = rcp, err_issue(ERR_CONST_ILLINTCNST);
                        in_cp = rcp;
                        lex_sym = icon(n, ovf, 16);
                    } else {
                        int base, d, err = 0, ovf = 0, toolong = 0;
                        base = (*lex_tok == '0')? 8: 10;
                        for (n = *lex_tok - '0'; isdigit(*rcp) || *rcp == '\n'; ) {
                            if (*rcp == '\n') {
                                if (rcp < in_limit)
                                    break;
                                toolong = 1;
                                in_cp = rcp;
                                in_fillbuf();
                                rcp = in_cp;
                                if (rcp == in_limit)
                                    break;
                                continue;
                            }
                            d = *rcp++ - '0';
                            if (base == 8) {
                                if (d == 8 || d == 9)
                                    err = 1;
                                if (n & ~(ULONG_MAX >> 3))
                                    ovf = 1;
                                else
                                    n = (n << 3) + d;
                            } else {    /* base == 10 */
                                if (n > (ULONG_MAX - d) / 10)
                                    ovf = 1;
                                else
                                    n = 10 * n + d;
                            }
                        }
                        if (*rcp == '.' || *rcp == 'e' || *rcp == 'E') {
                            in_cp = rcp;
                            lex_sym = fcon(toolong);
                            return LEX_FCON;
                        }
                        if (err)
                            err_issuex(0, ERR_CONST_ILLOCTESC);
                        in_cp = rcp;
                        lex_sym = icon(n, ovf, base);
                    }
                }
                return LEX_ICON;
            /* letters */
            case 'a':    /* auto and ids */
                if (rcp[0] == 'u' && rcp[1] == 't' && rcp[2] == 'o' && !ISCH_I(rcp[3])) {
                    in_cp = rcp + 3;
                    return LEX_AUTO;
                }
                goto id;
            case 'b':    /* break and ids */
                if (rcp[0] == 'r' && rcp[1] == 'e' && rcp[2] == 'a' && rcp[3] == 'k' &&
                    !ISCH_I(rcp[4])) {
                    in_cp = rcp + 4;
                    return LEX_BREAK;
                }
                goto id;
            case 'c':    /* case, char, const, continue and ids */
                if (rcp[0] == 'a' && rcp[1] == 's' && rcp[2] == 'e' && !ISCH_I(rcp[3])) {
                    in_cp = rcp + 3;
                    return LEX_CASE;
                }
                if (rcp[0] == 'h' && rcp[1] == 'a' && rcp[2] == 'r' && !ISCH_I(rcp[3])) {
                    in_cp = rcp + 3;
                    return LEX_CHAR;
                }
                if (rcp[0] == 'o' && rcp[1] == 'n') {
                    if (rcp[2] == 's' && rcp[3] == 't' && !ISCH_I(rcp[4])) {
                        in_cp = rcp + 4;
                        return LEX_CONST;
                    }
                    if (rcp[2] == 't' && rcp[3] == 'i' && rcp[4] == 'n' && rcp[5] == 'u' &&
                        rcp[6] == 'e' && !ISCH_I(rcp[7])) {
                        in_cp = rcp + 7;
                        return LEX_CONTINUE;
                    }
                }
                goto id;
            case 'd':    /* default, do, double and ids */
                if (rcp[0] == 'e' && rcp[1] == 'f' && rcp[2] == 'a' && rcp[3] == 'u' &&
                    rcp[4] == 'l' && rcp[5] == 't' && !ISCH_I(rcp[6])) {
                    in_cp = rcp+6;
                    return LEX_DEFAULT;
                }
                if (rcp[0] == 'o') {
                    if (!ISCH_I(rcp[1])) {
                        in_cp = rcp + 1;
                        return LEX_DO;
                    }
                    if (rcp[1] == 'u' && rcp[2] == 'b' && rcp[3] == 'l' && rcp[4] == 'e' &&
                        !ISCH_I(rcp[5])) {
                        in_cp = rcp + 5;
                        return LEX_DOUBLE;
                    }
                }
                goto id;
            case 'e':    /* else, enum, extern and ids */
                if (rcp[0] == 'l' && rcp[1] == 's' && rcp[2] == 'e' && !ISCH_I(rcp[3])) {
                    in_cp = rcp + 3;
                    return LEX_ELSE;
                }
                if (rcp[0] == 'n' && rcp[1] == 'u' && rcp[2] == 'm' && !ISCH_I(rcp[3])) {
                    in_cp = rcp + 3;
                    return LEX_ENUM;
                }
                if (rcp[0] == 'x' && rcp[1] == 't' && rcp[2] == 'e' && rcp[3] == 'r' &&
                    rcp[4] == 'n' && !ISCH_I(rcp[5])) {
                    in_cp = rcp + 5;
                    return LEX_EXTERN;
                }
                goto id;
            case 'f':    /* float, for and ids */
                if (rcp[0] == 'l' && rcp[1] == 'o' && rcp[2] == 'a' && rcp[3] == 't' &&
                    !ISCH_I(rcp[4])) {
                    in_cp = rcp + 4;
                    return LEX_FLOAT;
                }
                if (rcp[0] == 'o' && rcp[1] == 'r' && !ISCH_I(rcp[2])) {
                    in_cp = rcp + 2;
                    return LEX_FOR;
                }
                goto id;
            case 'g':    /* goto and ids */
                if (rcp[0] == 'o' && rcp[1] == 't' && rcp[2] == 'o' && !ISCH_I(rcp[3])) {
                    in_cp = rcp + 3;
                    return LEX_GOTO;
                }
                goto id;
            case 'i':    /* if, int and ids */
                if (rcp[0] == 'f' && !ISCH_I(rcp[1])) {
                    in_cp = rcp + 1;
                    return LEX_IF;
                }
                if (rcp[0] == 'n' && rcp[1] == 't' && !ISCH_I(rcp[2])) {
                    in_cp = rcp + 2;
                    return LEX_INT;
                }
                goto id;
            case 'l':    /* long nad ids */
                if (rcp[0] == 'o' && rcp[1] == 'n' && rcp[2] == 'g' && !ISCH_I(rcp[3])) {
                    in_cp = rcp + 3;
                    return LEX_LONG;
                }
                goto id;
            case 'r':    /* register, return and ids */
                if (rcp[0] == 'e') {
                    if (rcp[1] == 'g' && rcp[2] == 'i' && rcp[3] == 's' && rcp[4] == 't' &&
                        rcp[5] == 'e' && rcp[6] == 'r' && !ISCH_I(rcp[7])) {
                        in_cp = rcp + 7;
                        return LEX_REGISTER;
                    }
                    if (rcp[1] == 't' && rcp[2] == 'u' && rcp[3] == 'r' && rcp[4] == 'n' &&
                        !ISCH_I(rcp[5])) {
                        in_cp = rcp + 5;
                        return LEX_RETURN;
                    }
                }
                goto id;
            case 's':    /* short, signed, sizeof, static, struct, switch and ids */
                if (rcp[0] == 'h' && rcp[1] == 'o' && rcp[2] == 'r' && rcp[3] == 't' &&
                    !ISCH_I(rcp[4])) {
                    in_cp = rcp + 4;
                    return LEX_SHORT;
                }
                if (rcp[0] == 'i') {
                    if (rcp[1] == 'g' && rcp[2] == 'n' && rcp[3] == 'e' && rcp[4] == 'd' &&
                        !ISCH_I(rcp[5])) {
                        in_cp = rcp + 5;
                        return LEX_SIGNED;
                    }
                    if (rcp[1] == 'z' && rcp[2] == 'e' && rcp[3] == 'o' && rcp[4] == 'f' &&
                        !ISCH_I(rcp[5])) {
                        in_cp = rcp + 5;
                        return LEX_SIZEOF;
                    }
                }
                if (rcp[0] == 't') {
                    if (rcp[1] == 'a' && rcp[2] == 't' && rcp[3] == 'i' && rcp[4] == 'c' &&
                        !ISCH_I(rcp[5])) {
                        in_cp = rcp + 5;
                        return LEX_STATIC;
                    }
                    if (rcp[1] == 'r' && rcp[2] == 'u' && rcp[3] == 'c' && rcp[4] == 't' &&
                        !ISCH_I(rcp[5])) {
                        in_cp = rcp + 5;
                        return LEX_STRUCT;
                    }
                }
                if (rcp[0] == 'w' && rcp[1] == 'i' && rcp[2] == 't' && rcp[3] == 'c' &&
                    rcp[4] == 'h' && !ISCH_I(rcp[5])) {
                    in_cp = rcp + 5;
                    return LEX_SWITCH;
                }
                goto id;
            case 't':    /* typedef and ids */
                if (rcp[0] == 'y' && rcp[1] == 'p' && rcp[2] == 'e' && rcp[3] == 'd' &&
                    rcp[4] == 'e' && rcp[5] == 'f' && !ISCH_I(rcp[6])) {
                    in_cp = rcp + 6;
                    return LEX_TYPEDEF;
                }
                goto id;
            case 'u':    /* union, unsigned and ids */
                if (rcp[0] == 'n') {
                    if (rcp[1] == 'i' && rcp[2] == 'o' && rcp[3] == 'n' && !ISCH_I(rcp[4])) {
                        in_cp = rcp + 4;
                        return LEX_UNION;
                    }
                    if (rcp[1] == 's' && rcp[2] == 'i' && rcp[3] == 'g' && rcp[4] == 'n' &&
                        rcp[5] == 'e' && rcp[6] == 'd' && !ISCH_I(rcp[7])) {
                        in_cp = rcp + 7;
                        return LEX_UNSIGNED;
                    }
                }
                goto id;
            case 'v':    /* void, volatile and ids */
                if (rcp[0] == 'o') {
                    if (rcp[1] == 'i' && rcp[2] == 'd' && !ISCH_I(rcp[3])) {
                        in_cp = rcp + 3;
                        return LEX_VOID;
                    }
                    if (rcp[1] == 'l' && rcp[2] == 'a' && rcp[3] == 't' && rcp[4] == 'i' &&
                        rcp[5] == 'l' && rcp[6] == 'e' && !ISCH_I(rcp[7])) {
                        in_cp = rcp + 7;
                        return LEX_VOLATILE;
                    }
                }
                goto id;
            case 'w':    /* while and ids */
                if (rcp[0] == 'h' && rcp[1] == 'i' && rcp[2] == 'l' && rcp[3] == 'e' &&
                    !ISCH_I(rcp[4])) {
                    in_cp = rcp + 4;
                    return LEX_WHILE;
                }
                goto id;
            case 'L':    /* L'x', L"x" and ids */
                if (*rcp == '\'') {
                    w = 1;
                    in_cp++;
                    goto charconst;
                }
                if (*rcp == '\"') {
                    w = 1;
                    in_cp++;
                    goto strlit;
                }
                goto id;
            case 'h': case 'j': case 'k': case 'm': case 'n': case 'o': case 'p': case 'q':
            case 'x': case 'y': case 'z':    /* ids */
            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F': case 'G': case 'H':
            case 'I': case 'J': case 'K': case 'M': case 'N': case 'O': case 'P': case 'Q':
            case 'R': case 'S': case 'T': case 'U': case 'V': case 'W': case 'X': case 'Y':
            case 'Z':
            case '_':
            id:
                {
                    int ovf = 0;
                    if (in_limit - in_cp < IN_MAXLINE)    /* max length of significant id */
                        in_cp = rcp-1, in_fillbuf(), rcp = ++in_cp;
                    lex_tok = (char *)rcp - 1;
                    while (ISCH_I(*rcp))
                        rcp++;
                    lex_tok = hash_new(lex_tok, (char *)rcp-lex_tok);
                    lex_sym = sym_lookup(lex_tok, sym_ident);
                    while (ISCH_IN(*rcp)) {
                        if (*rcp == '\n')
                            IN_FILLBREAK(rcp) /* ; */
                        rcp++;
                        ovf++;
                    }
                    if (ovf > 0)
                        err_issuex(0, ERR_LEX_LONGIDOV, ovf, "character");
                }
                in_cp = rcp;
                return LEX_ID;
            default:
                {
                    char buf[1 + (CHAR_BIT+3)/4 + 1 + 1];    /* <HH> */
                    assert(!isspace(rcp[-1]));
                    if (isprint(rcp[-1]))
                        buf[0] = rcp[-1], buf[1] = '\0';
                    else
                        sprintf(buf, ARBCHAR, (unsigned)rcp[-1]);
#ifdef HAVE_ICONV
                    if (main_iton)
                        err_issuex(0, ERR_LEX_INVCHARCV, buf);
                    else
#endif    /* HAVE_ICONV */
                        err_issuex(0, ERR_LEX_INVCHAR, buf);
                }
                break;
        }
    }

    /* assert(!"impossible control flow -- should never reach here");
       return LEX_EOI; */
}


/*
 *  handles extra commas;
 *  designed to get removed with no other modifications
 */
int (lex_extracomma)(int c, const char *s, int opt)
{
    while (lex_tc == ',') {
        err_issuex(-1, ERR_LEX_EXTRACOMMA, s);
        lex_tc = lex_next();
    }
    if (lex_tc == c) {
        if (!opt)
            err_issuex(-1, ERR_LEX_EXTRACOMMA, s);
        return 1;
    }

    return 0;
}

/* end of lex.c */

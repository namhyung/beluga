/*
 *  declaration parsing
 */

#ifndef DECL_H
#define DECL_H

#include "lex.h"
#include "stmt.h"
#include "sym.h"
#include "ty.h"


extern sym_t *decl_cfunc;     /* function being currently parsed */
extern void **decl_callee;    /* (sym_t) callee of decl_cfunc */
extern sym_t *decl_retv;      /* struct/union return value of decl_cfunc if any */
extern int decl_ingdclr;      /* true while parsing global declarator; see lex_next() */


int decl_chkid(const char *, const lex_pos_t *, sym_tab_t *, int);
void decl_defglobal(sym_t *, int);
void decl_finalize(void);
void decl_compound(int, stmt_swtch_t *, int);
void decl_program(void);
ty_t *decl_typename(void);
void decl_errdecl(void);


#endif    /* DECL_H */

/* end of decl.h */

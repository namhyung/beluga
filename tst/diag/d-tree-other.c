void f1(void) { struct { int x; } s; char *a, *b; int c; c = a && b; c = s && b; }    /* error */
void f2(void) { double x; int i; unsigned u; i = x ^ x; u = i | i; u = i % i; }    /* error */
void f3_5(void) { struct { int x; } s; int i, *pi; void (*pf)(void); i = s==s;    /* error */
                  i = (void *)&i!=pi; i = pi==(void *)&i; i = (void *)0==pf;
                  i = (void *)&i==pf; }                                           /* error */
void f6_8(void) { struct { int x; } s; int i, *pi; void (*pf)(void); i = s<s;    /* error */
                  i = (void *)&i<=pi; i = pi>=(void *)&i; i = (void *)0<pf;      /* error */
                  i = (void *)&i>pf; }                                           /* error */
void f9(void) { double x; char i; unsigned long u; i = x << 2; i = i << u; }    /* error */

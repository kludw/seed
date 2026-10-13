// seed0.c — the stage-0 Seed compiler, written in C.
//
// Its only job is to compile seed.sd (the Seed compiler written in Seed)
// once. After that, Seed compiles itself and this file can be retired.
//
// It mirrors seed.sd function-for-function, so both produce byte-identical C.
//
// Usage: seed0 < program.sd > program.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_EOF, T_IDENT, T_NUM, T_STR, T_CHAR, T_PUNCT };

static char *src;
static long pos, line, kind, tstart, tlen;

// ---------- output & errors ----------

static void out(const char *s) { fputs(s, stdout); }

static void fail2(const char *a, const char *b) {
    fprintf(stderr, "line %ld: %s%s (found '%.*s')\n", line, a, b, (int)tlen, src + tstart);
    exit(1);
}
static void fail(const char *msg) { fail2(msg, ""); }

// ---------- lexer ----------

static int peekc(long i) { return (unsigned char)src[pos + i]; }
static int isdigit_(int c) { return c >= '0' && c <= '9'; }
static int isalpha_(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

static void skipspace(void) {
    for (;;) {
        int c = peekc(0);
        if (c == '\n') { line++; pos++; }
        else if (c == ' ' || c == '\t' || c == '\r') pos++;
        else if (c == '/' && peekc(1) == '/') { while (peekc(0) != '\n' && peekc(0) != 0) pos++; }
        else return;
    }
}

static void next(void) {
    skipspace();
    tstart = pos;
    int c = peekc(0);
    if (c == 0) kind = T_EOF;
    else if (isalpha_(c)) {
        kind = T_IDENT;
        while (isalpha_(peekc(0)) || isdigit_(peekc(0))) pos++;
    } else if (isdigit_(c)) {
        kind = T_NUM;
        while (isdigit_(peekc(0))) pos++;
    } else if (c == '"' || c == '\'') {
        kind = c == '"' ? T_STR : T_CHAR;
        pos++;
        while (peekc(0) != c) {
            if (peekc(0) == 0 || peekc(0) == '\n') fail("unterminated literal");
            if (peekc(0) == '\\') pos++;
            pos++;
        }
        pos++;
    } else {
        kind = T_PUNCT;
        int d = peekc(1);
        if ((c == '=' && d == '=') || (c == '!' && d == '=') || (c == '<' && d == '=') ||
            (c == '>' && d == '=') || (c == '&' && d == '&') || (c == '|' && d == '|'))
            pos += 2;
        else
            pos += 1;
    }
    tlen = pos - tstart;
}

static int is(const char *s) { return (long)strlen(s) == tlen && memcmp(s, src + tstart, tlen) == 0; }

static void restart(void) { pos = 0; line = 1; next(); }

// Is the token after the current one '='? (to spot `x = ...;`)
static int peekassign(void) {
    long p = tstart, l = line;
    next();
    int r = is("=");
    pos = p; line = l; next();
    return r;
}

// ---------- emitting helpers ----------

static void emittok(void) { fwrite(src + tstart, 1, tlen, stdout); putchar(' '); }
static void skip(const char *s) { if (!is(s)) fail2("expected ", s); next(); }
static void take(const char *s) { if (!is(s)) fail2("expected ", s); emittok(); next(); }
static void skipident(void) { if (kind != T_IDENT) fail("expected identifier"); next(); }
static void ident(void) { if (kind != T_IDENT) fail("expected identifier"); out("m_"); emittok(); next(); }

// ---------- expressions ----------
// Seed's operator precedence is exactly C's, so tokens pass straight through.

static void expr(void);

static int isbinop(void) {
    return kind == T_PUNCT && (is("+") || is("-") || is("*") || is("/") || is("%") || is("<") ||
                               is(">") || is("<=") || is(">=") || is("==") || is("!=") ||
                               is("&&") || is("||"));
}

static void primary(void) {
    if (kind == T_NUM || kind == T_CHAR) { emittok(); next(); }
    else if (kind == T_STR) { out("(long)"); emittok(); next(); }
    else if (kind == T_IDENT) {
        ident();
        if (is("(")) {
            take("(");
            if (!is(")")) {
                expr();
                while (is(",")) { take(","); expr(); }
            }
            take(")");
        }
    } else if (is("(")) { take("("); expr(); take(")"); }
    else fail("expected expression");
}

static void unary(void) {
    if (is("-") || is("!")) { emittok(); next(); unary(); }
    else primary();
}

static void expr(void) {
    unary();
    while (isbinop()) { emittok(); next(); unary(); }
}

// ---------- statements ----------

static void stmt(void);

static void block(void) {
    take("{"); putchar('\n');
    while (!is("}")) stmt();
    take("}"); putchar('\n');
}

static void stmt(void) {
    if (is("var")) {
        next(); out("long "); ident(); take("="); expr(); take(";");
    } else if (is("if")) {
        emittok(); next(); take("("); expr(); take(")"); block();
        if (is("else")) {
            emittok(); next();
            if (is("if")) stmt(); else block();
        }
    } else if (is("while")) {
        emittok(); next(); take("("); expr(); take(")"); block();
    } else if (is("return")) {
        emittok(); next(); expr(); take(";");
    } else if (kind == T_IDENT && peekassign()) {
        ident(); take("="); expr(); take(";");
    } else {
        expr(); take(";");
    }
    putchar('\n');
}

// ---------- top level ----------

static void skipblock(void) {
    if (!is("{")) fail2("expected ", "{");
    long depth = 0;
    do {
        if (kind == T_EOF) fail("unexpected end of file");
        if (is("{")) depth++;
        if (is("}")) depth--;
        next();
    } while (depth != 0);
}

// Pass 1: declare globals and function prototypes, so order doesn't matter.
static void declare(void) {
    while (kind != T_EOF) {
        if (is("var")) {
            next(); out("long "); ident(); skip(";"); out(";\n");
        } else if (is("fn")) {
            next(); out("long "); ident(); skip("(");
            if (is(")")) out("(void)");
            else {
                out("(long"); skipident();
                while (is(",")) { next(); out(", long"); skipident(); }
                out(")");
            }
            skip(")"); out(";\n");
            skipblock();
        } else fail("expected 'fn' or 'var' at top level");
    }
}

static void func(void) {
    skip("fn"); out("long "); ident(); take("(");
    if (is(")")) out("void ");
    else {
        out("long "); ident();
        while (is(",")) { take(","); out("long "); ident(); }
    }
    take(")");
    take("{"); putchar('\n');
    while (!is("}")) stmt();
    out("return 0;\n"); take("}"); putchar('\n');
}

// Pass 2: emit function bodies.
static void define(void) {
    while (kind != T_EOF) {
        if (is("var")) { next(); next(); next(); }
        else func();
    }
}

static void prelude(void) {
    out("#include <stdio.h>\n");
    out("#include <stdlib.h>\n");
    out("long m_getchar(void) { return getchar(); }\n");
    out("long m_putchar(long c) { putchar((int)c); return c; }\n");
    out("long m_eputchar(long c) { fputc((int)c, stderr); return c; }\n");
    out("long m_alloc(long n) { return (long)calloc(1, (size_t)n); }\n");
    out("long m_load8(long p) { return *(unsigned char *)p; }\n");
    out("long m_store8(long p, long v) { *(unsigned char *)p = (unsigned char)v; return v; }\n");
    out("long m_exit(long c) { exit((int)c); return 0; }\n");
}

static void readall(void) {
    long cap = 4000000, n = 0;
    src = calloc(1, cap + 1);
    int c;
    while ((c = getchar()) != EOF) {
        if (n == cap) fail("input too large");
        src[n++] = (char)c;
    }
}

int main(void) {
    readall();
    prelude();
    restart(); declare();
    restart(); define();
    out("int main(void) { return (int)m_main(); }\n");
    return 0;
}

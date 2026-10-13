/* seed0: the bootstrap compiler for a subset of Seed.
 *
 *   seed0 a.sd b.sd ... > out.c
 *
 * Pipeline: lex -> parse (AST) -> check (resolve names and types, annotate the
 * AST, nullable narrowing) -> generate C. The runtime (seed0/runtime.c) is
 * pasted at the top of the output, so the output is self-contained.
 * See seed0/README.md for the design. */
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *runtime_src =
#include "runtime.inc"
    ;

/* ======================================================================
 * Utilities
 * ====================================================================== */

static char *arena_p, *arena_end;

static void *alloc(size_t n) {
  n = (n + 15) & ~(size_t)15;
  if (n == 0) n = 16;
  if ((size_t)(arena_end - arena_p) < n) {
    size_t sz = n > (1 << 20) ? n : (1 << 20);
    arena_p = calloc(1, sz);
    if (!arena_p) {
      fprintf(stderr, "seed0: out of memory\n");
      exit(1);
    }
    arena_end = arena_p + sz;
  }
  void *p = arena_p;
  arena_p += n;
  return p;
}

static char *sndup(const char *s, size_t n) {
  char *p = alloc(n + 1);
  memcpy(p, s, n);
  return p;
}

static char *sdup(const char *s) { return sndup(s, strlen(s)); }

static char *fmt(const char *f, ...) {
  va_list ap;
  char tmp[512];
  va_start(ap, f);
  int n = vsnprintf(tmp, sizeof tmp, f, ap);
  va_end(ap);
  if (n < (int)sizeof tmp) return sndup(tmp, (size_t)n);
  char *p = alloc((size_t)n + 1);
  va_start(ap, f);
  vsnprintf(p, (size_t)n + 1, f, ap);
  va_end(ap);
  return p;
}

typedef struct {
  char *s;
  size_t len, cap;
} Buf;

static void bputn(Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->len + n + 1) cap *= 2;
    b->s = realloc(b->s, cap);
    b->cap = cap;
  }
  memcpy(b->s + b->len, s, n);
  b->len += n;
  b->s[b->len] = 0;
}

static void bput(Buf *b, const char *s) { bputn(b, s, strlen(s)); }

static void bprintf(Buf *b, const char *f, ...) {
  va_list ap;
  char tmp[1024];
  va_start(ap, f);
  int n = vsnprintf(tmp, sizeof tmp, f, ap);
  va_end(ap);
  if (n < (int)sizeof tmp) {
    bputn(b, tmp, (size_t)n);
    return;
  }
  char *p = malloc((size_t)n + 1);
  va_start(ap, f);
  vsnprintf(p, (size_t)n + 1, f, ap);
  va_end(ap);
  bputn(b, p, (size_t)n);
  free(p);
}

static const char *bstr(Buf *b) { return b->s ? b->s : ""; }

#define VEC(T) \
  struct {     \
    T *v;      \
    int n, cap; \
  }
#define PUSH(vec, x)                                                     \
  do {                                                                   \
    if ((vec).n == (vec).cap) {                                          \
      (vec).cap = (vec).cap ? (vec).cap * 2 : 4;                         \
      (vec).v = realloc((vec).v, (size_t)(vec).cap * sizeof(*(vec).v)); \
    }                                                                    \
    (vec).v[(vec).n++] = (x);                                            \
  } while (0)

/* String-keyed hash map. */
typedef struct {
  const char *key;
  void *val;
} HEnt;
typedef struct {
  HEnt *e;
  int cap, n;
} HMap;

static uint64_t hash_str(const char *s) {
  uint64_t h = 0xcbf29ce484222325ULL;
  while (*s) h = (h ^ (uint8_t)*s++) * 0x100000001b3ULL;
  return h;
}

static void *hget(HMap *m, const char *key) {
  if (!m->cap) return NULL;
  uint64_t i = hash_str(key) & (uint64_t)(m->cap - 1);
  while (m->e[i].key) {
    if (!strcmp(m->e[i].key, key)) return m->e[i].val;
    i = (i + 1) & (uint64_t)(m->cap - 1);
  }
  return NULL;
}

static void hput(HMap *m, const char *key, void *val) {
  if ((m->n + 1) * 2 > m->cap) {
    HMap n = {calloc((size_t)(m->cap ? m->cap * 2 : 64), sizeof(HEnt)), m->cap ? m->cap * 2 : 64, 0};
    for (int i = 0; i < m->cap; i++)
      if (m->e[i].key) hput(&n, m->e[i].key, m->e[i].val);
    free(m->e);
    *m = n;
  }
  uint64_t i = hash_str(key) & (uint64_t)(m->cap - 1);
  while (m->e[i].key) {
    if (!strcmp(m->e[i].key, key)) {
      m->e[i].val = val;
      return;
    }
    i = (i + 1) & (uint64_t)(m->cap - 1);
  }
  m->e[i].key = key;
  m->e[i].val = val;
  m->n++;
}

typedef struct {
  const char *file;
  int line, col;
} Pos;

static void die(Pos p, const char *f, ...) __attribute__((noreturn, format(printf, 2, 3)));
static void die(Pos p, const char *f, ...) {
  va_list ap;
  fprintf(stderr, "%s:%d:%d: error: ", p.file, p.line, p.col);
  va_start(ap, f);
  vfprintf(stderr, f, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

/* ======================================================================
 * Lexer
 * ====================================================================== */

enum {
  T_EOF, T_NL, T_IDENT, T_INT, T_FLOAT, T_BYTE, T_STRING,
  /* keywords */
  K_PACKAGE, K_IMPORT, K_PUB, K_FN, K_TYPE, K_STRUCT, K_ENUM, K_INTERFACE, K_IMPLEMENTS, K_CONST,
  K_IF, K_ELSE, K_FOR, K_SWITCH, K_CASE, K_DEFAULT, K_BREAK, K_CONTINUE, K_RETURN, K_DEFER, K_TRY,
  K_NULL, K_TRUE, K_FALSE, K_MUT, K_TEST, K_ASSERT, K_SELECT, K_SPAWN, K_GROUP, K_CHAN, K_MAP,
  /* punctuation */
  P_PLUS, P_MINUS, P_STAR, P_SLASH, P_PERCENT, P_AMP, P_PIPE, P_CARET, P_SHL, P_SHR, P_EQ, P_NE,
  P_LT, P_LE, P_GT, P_GE, P_ANDAND, P_OROR, P_NOT, P_ASSIGN, P_INC, P_DEC, P_LPAREN, P_RPAREN,
  P_LBRACK, P_RBRACK, P_LBRACE, P_RBRACE, P_COMMA, P_SEMI, P_COLON, P_DOT, P_QUESTION,
  T_COUNT
};

static const char *tok_names[T_COUNT] = {
  "end of file", "newline", "identifier", "integer", "float", "byte", "string",
  "package", "import", "pub", "fn", "type", "struct", "enum", "interface", "implements", "const",
  "if", "else", "for", "switch", "case", "default", "break", "continue", "return", "defer", "try",
  "null", "true", "false", "mut", "test", "assert", "select", "spawn", "group", "chan", "map",
  "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "==", "!=",
  "<", "<=", ">", ">=", "&&", "||", "!", "=", "++", "--", "(", ")",
  "[", "]", "{", "}", ",", ";", ":", ".", "?",
};

typedef struct {
  int k;
  Pos pos;
  const char *s; /* identifier name or string bytes */
  int64_t slen;
  uint64_t u;
  double f;
} Token;

typedef VEC(Token) Tokens;

static int keyword(const char *s, size_t n) {
  for (int k = K_PACKAGE; k <= K_MAP; k++)
    if (strlen(tok_names[k]) == n && !memcmp(tok_names[k], s, n)) return k;
  return T_IDENT;
}

static bool ends_statement(int k) {
  switch (k) {
  case T_IDENT: case T_INT: case T_FLOAT: case T_BYTE: case T_STRING:
  case K_TRUE: case K_FALSE: case K_NULL: case K_RETURN: case K_BREAK: case K_CONTINUE:
  case P_RPAREN: case P_RBRACK: case P_RBRACE: case P_QUESTION: case P_INC: case P_DEC:
    return true;
  }
  return false;
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void lex(const char *file, const char *src, size_t len, Tokens *out) {
  size_t i = 0;
  int line = 1, col = 1;
  int last = T_NL;
#define POS ((Pos){file, line, col})
  while (i < len) {
    char c = src[i];
    if (c == '\n') {
      if (ends_statement(last)) {
        PUSH(*out, ((Token){T_NL, POS, 0, 0, 0, 0}));
        last = T_NL;
      }
      i++;
      line++;
      col = 1;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      col++;
      continue;
    }
    if (c == '/' && i + 1 < len && src[i + 1] == '/') {
      while (i < len && src[i] != '\n') i++;
      continue;
    }
    Token t = {0};
    t.pos = POS;
    size_t start = i;
    if (isalpha((unsigned char)c) || c == '_') {
      while (i < len && (isalnum((unsigned char)src[i]) || src[i] == '_')) i++;
      t.k = keyword(src + start, i - start);
      t.s = sndup(src + start, i - start);
    } else if (isdigit((unsigned char)c)) {
      if (c == '0' && i + 1 < len && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
        i += 2;
        uint64_t v = 0;
        int nd = 0;
        while (i < len && hexval(src[i]) >= 0) {
          if (v >> 60) die(t.pos, "integer literal is too large for seed0's 64-bit int");
          v = v * 16 + (uint64_t)hexval(src[i]);
          i++;
          nd++;
        }
        if (!nd) die(t.pos, "hexadecimal literal needs digits after 0x");
        t.k = T_INT;
        t.u = v;
      } else {
        while (i < len && isdigit((unsigned char)src[i])) i++;
        if (i + 1 < len && src[i] == '.' && isdigit((unsigned char)src[i + 1])) {
          i++;
          while (i < len && isdigit((unsigned char)src[i])) i++;
          if (i < len && (src[i] == 'e' || src[i] == 'E')) {
            size_t j = i + 1;
            if (j < len && (src[j] == '+' || src[j] == '-')) j++;
            if (j < len && isdigit((unsigned char)src[j])) {
              i = j;
              while (i < len && isdigit((unsigned char)src[i])) i++;
            }
          }
          t.k = T_FLOAT;
          t.f = strtod(sndup(src + start, i - start), NULL);
        } else {
          uint64_t v = 0;
          for (size_t j = start; j < i; j++) {
            uint64_t d = (uint64_t)(src[j] - '0');
            if (v > (UINT64_MAX - d) / 10) die(t.pos, "integer literal is too large for seed0's 64-bit int");
            v = v * 10 + d;
          }
          t.k = T_INT;
          t.u = v;
        }
      }
      if (i < len && (isalpha((unsigned char)src[i]) || src[i] == '_'))
        die(t.pos, "invalid number literal (a float needs a '.', for example 1.0e9)");
    } else if (c == '"' || c == '\'') {
      char q = c;
      i++;
      Buf b = {0};
      while (i < len && src[i] != q) {
        if (src[i] == '\n') die(t.pos, "unterminated %s literal", q == '"' ? "string" : "byte");
        if (src[i] == '\\') {
          i++;
          if (i >= len) break;
          char e = src[i++];
          char ch;
          switch (e) {
          case 'n': ch = '\n'; break;
          case 't': ch = '\t'; break;
          case 'r': ch = '\r'; break;
          case '0': ch = 0; break;
          case '\\': ch = '\\'; break;
          case '"': ch = '"'; break;
          case '\'': ch = '\''; break;
          case 'x': {
            int h1 = i < len ? hexval(src[i]) : -1, h2 = i + 1 < len ? hexval(src[i + 1]) : -1;
            if (h1 < 0 || h2 < 0) die((Pos){file, line, col + (int)(i - start)}, "\\x needs two hex digits");
            ch = (char)(h1 * 16 + h2);
            i += 2;
            break;
          }
          default:
            die((Pos){file, line, col + (int)(i - start) - 1}, "unknown escape sequence '\\%c'", e);
          }
          bputn(&b, &ch, 1);
        } else {
          bputn(&b, &src[i], 1);
          i++;
        }
      }
      if (i >= len) die(t.pos, "unterminated %s literal", q == '"' ? "string" : "byte");
      i++;
      if (q == '"') {
        t.k = T_STRING;
        t.s = b.len ? sndup(b.s, b.len) : "";
        t.slen = (int64_t)b.len;
      } else {
        if (b.len != 1) die(t.pos, "a byte literal must hold exactly one byte");
        t.k = T_BYTE;
        t.u = (uint8_t)b.s[0];
      }
      free(b.s);
    } else {
      static const struct {
        const char *s;
        int k;
      } ops[] = {
        {"<<", P_SHL}, {">>", P_SHR}, {"==", P_EQ}, {"!=", P_NE}, {"<=", P_LE}, {">=", P_GE},
        {"&&", P_ANDAND}, {"||", P_OROR}, {"++", P_INC}, {"--", P_DEC},
        {"+", P_PLUS}, {"-", P_MINUS}, {"*", P_STAR}, {"/", P_SLASH}, {"%", P_PERCENT},
        {"&", P_AMP}, {"|", P_PIPE}, {"^", P_CARET}, {"<", P_LT}, {">", P_GT}, {"!", P_NOT},
        {"=", P_ASSIGN}, {"(", P_LPAREN}, {")", P_RPAREN}, {"[", P_LBRACK}, {"]", P_RBRACK},
        {"{", P_LBRACE}, {"}", P_RBRACE}, {",", P_COMMA}, {";", P_SEMI}, {":", P_COLON},
        {".", P_DOT}, {"?", P_QUESTION},
      };
      t.k = -1;
      for (size_t k = 0; k < sizeof ops / sizeof ops[0]; k++) {
        size_t n = strlen(ops[k].s);
        if (i + n <= len && !memcmp(src + i, ops[k].s, n)) {
          t.k = ops[k].k;
          i += n;
          break;
        }
      }
      if (t.k < 0) die(t.pos, "unexpected character '%c'", c);
    }
    col += (int)(i - start);
    PUSH(*out, t);
    last = t.k;
  }
  if (ends_statement(last)) PUSH(*out, ((Token){T_NL, POS, 0, 0, 0, 0}));
  PUSH(*out, ((Token){T_EOF, POS, 0, 0, 0, 0}));
#undef POS
}

/* ======================================================================
 * AST
 * ====================================================================== */

typedef struct Type Type;
typedef struct Var Var;
typedef struct FnDecl FnDecl;
typedef struct TypeDecl TypeDecl;
typedef struct ConstDecl ConstDecl;

enum { TE_NAME, TE_LIST, TE_MAP, TE_OPT };
typedef struct TypeExpr {
  int k;
  Pos pos;
  const char *name;
  struct TypeExpr *a, *b;
} TypeExpr;

typedef struct {
  TypeExpr *te;
  const char *name;
  Pos pos;
  Type *t;
} Param;
typedef VEC(Param) Params;

enum {
  E_INT, E_FLOAT, E_BYTE, E_STRING, E_BOOL, E_NULL, E_IDENT, E_FIELD, E_INDEX, E_CALL,
  E_UNARY, E_BINARY, E_TRY, E_LIST, E_MAP, E_STRUCT, E_WRAP
};

/* call kinds */
enum { CK_FUNC = 1, CK_METHOD, CK_BUILTIN, CK_PKG, CK_CONV, CK_ENUM };
/* field kinds */
enum { FK_FIELD = 1, FK_ENUMVAL };
/* index kinds */
enum { IK_LIST = 1, IK_STRING, IK_MAP };

typedef struct Expr Expr;
typedef VEC(Expr *) Exprs;

struct Expr {
  int k;
  Pos pos;
  Type *t;
  bool narrowed; /* stored as orig (a T?), read as T */
  bool paren;
  bool neg;      /* integer literal already negated */
  Type *orig;
  uint64_t u;
  double f;
  const char *s; /* string bytes, identifier or field name */
  int64_t slen;
  int op;
  Expr *a, *b;
  Exprs args;
  Exprs keys;
  VEC(const char *) names;
  VEC(Pos) npos;
  TypeExpr *te, *te2;
  /* resolution */
  int sub; /* call/field/index kind */
  int bid; /* builtin or package function id */
  Var *var;
  ConstDecl *cd;
  FnDecl *fn;
  TypeDecl *td;
  int ecase;
};

typedef struct Stmt Stmt;
typedef VEC(Stmt *) Block;

typedef struct {
  TypeExpr *te;
  const char *name;
  Expr *lv;
  bool discard;
  Pos pos;
  Var *var;
  Type *t; /* declared/target type */
} Target;

typedef struct {
  Expr *e;
  bool pattern;
  const char *tname, *cname;
  Params params;
  bool parens;
  Pos pos;
  int ecase;
  Var **vars;
} Label;

typedef struct {
  VEC(Label) labels;
  bool isdefault;
  Block body;
  Pos pos;
} Case;

enum { S_EXPR, S_ASSIGN, S_INCDEC, S_IF, S_FOR, S_SWITCH, S_RETURN, S_BREAK, S_CONTINUE, S_CONST };

struct Stmt {
  int k;
  Pos pos;
  VEC(Target) targets;
  Expr *e;
  Exprs exprs;
  bool inc;
  Expr *cond;
  Block body, els;
  bool has_else;
  Stmt *init, *post;
  VEC(Case) cases;
  TypeExpr *te;
  const char *name;
  Var *var;
  bool discard_err; /* multi-assign with the error slot discarded */
};

struct FnDecl {
  const char *name;
  Pos pos;
  bool is_method;
  const char *recv_tname;
  Pos recv_pos;
  const char *recv_name;
  bool recv_mut;
  Params params;
  VEC(TypeExpr *) results;
  Block body;
  /* resolved */
  TypeDecl *recv;
  VEC(Type *) rtypes;
  Type *ret; /* void, a single type, or a tuple */
  const char *cname;
};

typedef struct {
  const char *name;
  Pos pos;
  Params params;
} ECase;

struct TypeDecl {
  const char *name;
  Pos pos;
  bool is_enum;
  Params fields;
  VEC(ECase) cases;
  Type *t;
  int topo;
  HMap methods;
};

struct ConstDecl {
  const char *name;
  Pos pos;
  TypeExpr *te;
  Expr *e;
  Type *t;
  int state; /* 0 unchecked, 1 checking, 2 done */
};

/* ======================================================================
 * Parser
 * ====================================================================== */

static Token *tk;
static int tpos;

static VEC(FnDecl *) fns;
static VEC(TypeDecl *) tdecls;
static VEC(ConstDecl *) cdecls;
static HMap globals;  /* name -> Global */
static HMap imported; /* package name -> (void*)1 */

enum { G_FN = 1, G_TYPE, G_CONST };
typedef struct {
  int k;
  void *p;
  Pos pos;
} Global;

static Token *peek(void) { return &tk[tpos]; }
static int pk(void) { return tk[tpos].k; }
static int pkn(int n) {
  int i = tpos;
  for (int j = 0; j < n && tk[i].k != T_EOF; j++) i++;
  return tk[i].k;
}
static Token *next(void) {
  Token *t = &tk[tpos];
  if (t->k != T_EOF) tpos++;
  return t;
}
static bool accept(int k) {
  if (pk() == k) {
    next();
    return true;
  }
  return false;
}

static const char *tok_desc(Token *t) {
  if (t->k == T_IDENT) return fmt("'%s'", t->s);
  if (t->k == T_NL || t->k == T_EOF) return tok_names[t->k];
  if (t->k == T_INT || t->k == T_FLOAT || t->k == T_STRING || t->k == T_BYTE) return fmt("%s literal", tok_names[t->k]);
  return fmt("'%s'", tok_names[t->k]);
}

static Token *expect(int k, const char *ctx) {
  if (pk() != k) {
    if (k == T_NL)
      die(peek()->pos, "expected the end of the statement %s, found %s (put one statement per line)", ctx,
          tok_desc(peek()));
    die(peek()->pos, "expected '%s' %s, found %s", tok_names[k], ctx, tok_desc(peek()));
  }
  return next();
}

static const char *expect_ident(const char *ctx) {
  if (pk() != T_IDENT) die(peek()->pos, "expected a name %s, found %s", ctx, tok_desc(peek()));
  return next()->s;
}

static void skip_nl(void) {
  while (pk() == T_NL) next();
}

static Expr *new_expr(int k, Pos pos) {
  Expr *e = alloc(sizeof *e);
  e->k = k;
  e->pos = pos;
  return e;
}

static Stmt *new_stmt(int k, Pos pos) {
  Stmt *s = alloc(sizeof *s);
  s->k = k;
  s->pos = pos;
  return s;
}

static void unsupported(Pos p, const char *what) {
  die(p, "%s is not supported by seed0 (the bootstrap compiler); see docs/seed0.md", what);
}

static TypeExpr *ptype(bool must);

static TypeExpr *pbase(bool must) {
  Token *t = peek();
  TypeExpr *te = alloc(sizeof *te);
  te->pos = t->pos;
  switch (t->k) {
  case T_IDENT:
    next();
    te->k = TE_NAME;
    te->name = t->s;
    if (pk() == P_DOT && pkn(1) == T_IDENT) {
      if (!must) {
        /* a qualified name like pkg.Type: only valid as a declaration type */
        next();
        te->name = fmt("%s.%s", t->s, next()->s);
      } else {
        next();
        const char *n = next()->s;
        die(t->pos, "unknown type %s.%s: seed0 only supports the core packages, which have no types", t->s, n);
      }
    }
    return te;
  case P_LBRACK:
    if (pkn(1) != P_RBRACK) {
      if (must) die(t->pos, "expected a list type like []int");
      return NULL;
    }
    next();
    next();
    te->k = TE_LIST;
    te->a = pbase(must);
    return te->a ? te : NULL;
  case K_MAP:
    next();
    if (!accept(P_LBRACK)) {
      if (must) die(peek()->pos, "expected '[' after map");
      return NULL;
    }
    te->k = TE_MAP;
    te->a = ptype(must);
    if (!te->a) return NULL;
    if (!accept(P_RBRACK)) {
      if (must) die(peek()->pos, "expected ']' after the map key type");
      return NULL;
    }
    te->b = pbase(must);
    return te->b ? te : NULL;
  case P_LPAREN: {
    next();
    TypeExpr *in = ptype(must);
    if (!in) return NULL;
    if (!accept(P_RPAREN)) {
      if (must) die(peek()->pos, "expected ')' to close the type");
      return NULL;
    }
    return in;
  }
  case K_CHAN:
    if (must) unsupported(t->pos, "chan");
    return NULL;
  case K_FN:
    if (must) unsupported(t->pos, "a function type");
    return NULL;
  default:
    if (must) die(t->pos, "expected a type, found %s", tok_desc(t));
    return NULL;
  }
}

static TypeExpr *ptype(bool must) {
  TypeExpr *b = pbase(must);
  if (!b) return NULL;
  if (pk() == P_QUESTION) {
    Token *q = next();
    if (pk() == P_QUESTION) die(peek()->pos, "T?? is not a valid type");
    TypeExpr *o = alloc(sizeof *o);
    o->k = TE_OPT;
    o->pos = q->pos;
    o->a = b;
    return o;
  }
  return b;
}

/* Try to parse "Type name" at the current position; restore on failure. */
static TypeExpr *try_typed_name(void) {
  int save = tpos;
  int k = pk();
  if (k != T_IDENT && k != P_LBRACK && k != K_MAP && k != P_LPAREN) return NULL;
  TypeExpr *te = ptype(false);
  if (te && pk() == T_IDENT) return te;
  tpos = save;
  return NULL;
}

static Expr *pexpr(void);
static Block pblock(void);

static void pargs(Exprs *args, int close) {
  skip_nl();
  while (pk() != close) {
    PUSH(*args, pexpr());
    skip_nl();
    if (!accept(P_COMMA)) break;
    skip_nl();
  }
  skip_nl();
  expect(close, close == P_RPAREN ? "to close the argument list" : "to close the literal");
}

static Expr *pprimary(void) {
  Token *t = peek();
  Expr *e;
  switch (t->k) {
  case T_INT:
    next();
    e = new_expr(E_INT, t->pos);
    e->u = t->u;
    return e;
  case T_FLOAT:
    next();
    e = new_expr(E_FLOAT, t->pos);
    e->f = t->f;
    return e;
  case T_BYTE:
    next();
    e = new_expr(E_BYTE, t->pos);
    e->u = t->u;
    return e;
  case T_STRING:
    next();
    e = new_expr(E_STRING, t->pos);
    e->s = t->s;
    e->slen = t->slen;
    return e;
  case K_TRUE:
  case K_FALSE:
    next();
    e = new_expr(E_BOOL, t->pos);
    e->u = t->k == K_TRUE;
    return e;
  case K_NULL:
    next();
    return new_expr(E_NULL, t->pos);
  case T_IDENT:
    next();
    if (pk() == P_LBRACE) {
      e = new_expr(E_STRUCT, t->pos);
      e->s = t->s;
      next();
      skip_nl();
      while (pk() != P_RBRACE) {
        Token *f = peek();
        PUSH(e->names, expect_ident("for a struct field"));
        PUSH(e->npos, f->pos);
        expect(P_COLON, "after the field name");
        skip_nl();
        PUSH(e->args, pexpr());
        skip_nl();
        if (!accept(P_COMMA)) break;
        skip_nl();
      }
      skip_nl();
      expect(P_RBRACE, "to close the struct literal");
      return e;
    }
    e = new_expr(E_IDENT, t->pos);
    e->s = t->s;
    return e;
  case P_LPAREN:
    next();
    skip_nl();
    e = pexpr();
    skip_nl();
    expect(P_RPAREN, "to close the parenthesis");
    e->paren = true;
    return e;
  case P_LBRACK: {
    e = new_expr(E_LIST, t->pos);
    TypeExpr *te = pbase(true);
    e->te = te->a;
    expect(P_LBRACE, "after the list type (a list literal looks like []int{1, 2})");
    pargs(&e->args, P_RBRACE);
    return e;
  }
  case K_MAP: {
    e = new_expr(E_MAP, t->pos);
    TypeExpr *te = pbase(true);
    e->te = te->a;
    e->te2 = te->b;
    expect(P_LBRACE, "after the map type (a map literal looks like map[string]int{\"a\": 1})");
    skip_nl();
    while (pk() != P_RBRACE) {
      PUSH(e->keys, pexpr());
      expect(P_COLON, "between a map key and its value");
      skip_nl();
      PUSH(e->args, pexpr());
      skip_nl();
      if (!accept(P_COMMA)) break;
      skip_nl();
    }
    skip_nl();
    expect(P_RBRACE, "to close the map literal");
    return e;
  }
  case K_CHAN: unsupported(t->pos, "chan");
  case K_GROUP: unsupported(t->pos, "group");
  case K_FN: unsupported(t->pos, "a function literal");
  default:
    die(t->pos, "expected an expression, found %s", tok_desc(t));
  }
}

static Expr *ppostfix(void) {
  Expr *e = pprimary();
  for (;;) {
    Token *t = peek();
    if (t->k == P_DOT) {
      next();
      Token *n = peek();
      Expr *f = new_expr(E_FIELD, n->pos);
      f->s = expect_ident("after '.'");
      f->a = e;
      e = f;
    } else if (t->k == P_LBRACK) {
      next();
      skip_nl();
      Expr *ix = new_expr(E_INDEX, t->pos);
      ix->a = e;
      ix->b = pexpr();
      skip_nl();
      expect(P_RBRACK, "to close the index");
      e = ix;
    } else if (t->k == P_LPAREN) {
      next();
      Expr *c = new_expr(E_CALL, e->pos);
      c->a = e;
      pargs(&c->args, P_RPAREN);
      e = c;
    } else {
      return e;
    }
  }
}

static Expr *punary(void) {
  Token *t = peek();
  if (t->k == P_MINUS || t->k == P_NOT) {
    next();
    Expr *a = punary();
    if (t->k == P_MINUS && a->k == E_INT && !a->paren && !a->neg) {
      if (a->u > (uint64_t)INT64_MAX + 1) die(a->pos, "integer literal is too large for seed0's 64-bit int");
      a->u = (uint64_t)0 - a->u;
      a->neg = true;
      a->pos = t->pos;
      return a;
    }
    Expr *e = new_expr(E_UNARY, t->pos);
    e->op = t->k;
    e->a = a;
    return e;
  }
  if (t->k == K_TRY) {
    next();
    Expr *e = new_expr(E_TRY, t->pos);
    e->a = punary();
    return e;
  }
  return ppostfix();
}

static int binprec(int k) {
  switch (k) {
  case P_OROR: return 1;
  case P_ANDAND: return 2;
  case P_EQ: case P_NE: case P_LT: case P_LE: case P_GT: case P_GE: return 3;
  case P_PLUS: case P_MINUS: case P_PIPE: case P_CARET: return 4;
  case P_STAR: case P_SLASH: case P_PERCENT: case P_SHL: case P_SHR: case P_AMP: return 5;
  }
  return 0;
}

static Expr *pbinary(int minprec) {
  Expr *l = punary();
  for (;;) {
    Token *t = peek();
    int p = binprec(t->k);
    if (!p || p < minprec) return l;
    if (p == 3 && l->k == E_BINARY && binprec(l->op) == 3 && !l->paren)
      die(t->pos, "comparison operators do not chain; use && to combine comparisons");
    next();
    skip_nl();
    Expr *r = pbinary(p + 1);
    Expr *b = new_expr(E_BINARY, t->pos);
    b->op = t->k;
    b->a = l;
    b->b = r;
    l = b;
  }
}

static Expr *pexpr(void) { return pbinary(1); }

static Stmt *psimple(void) {
  Pos pos = peek()->pos;
  Stmt *s = new_stmt(S_ASSIGN, pos);
  for (;;) {
    Target tg = {0};
    tg.pos = peek()->pos;
    TypeExpr *te = try_typed_name();
    if (te) {
      tg.te = te;
      tg.pos = peek()->pos;
      tg.name = next()->s;
      if (!strcmp(tg.name, "_")) tg.discard = true;
    } else {
      Expr *e = pexpr();
      if (e->k == E_IDENT && !strcmp(e->s, "_")) tg.discard = true;
      else tg.lv = e;
    }
    PUSH(s->targets, tg);
    if (!accept(P_COMMA)) break;
  }
  if (accept(P_ASSIGN)) {
    skip_nl();
    s->e = pexpr();
    return s;
  }
  Target *t0 = &s->targets.v[0];
  if (s->targets.n > 1 || t0->te) die(peek()->pos, "expected '=' in the declaration or assignment, found %s", tok_desc(peek()));
  if (t0->discard) die(peek()->pos, "expected '=' after _");
  if (pk() == P_INC || pk() == P_DEC) {
    s->k = S_INCDEC;
    s->inc = next()->k == P_INC;
    s->e = t0->lv;
    return s;
  }
  s->k = S_EXPR;
  s->e = t0->lv;
  return s;
}

static Stmt *pstmt(void);

static Stmt *pif(void) {
  Stmt *s = new_stmt(S_IF, next()->pos);
  expect(P_LPAREN, "after if (conditions are always in parentheses)");
  s->cond = pexpr();
  expect(P_RPAREN, "to close the if condition");
  s->body = pblock();
  if (accept(K_ELSE)) {
    s->has_else = true;
    if (pk() == K_IF) {
      PUSH(s->els, pif());
    } else {
      s->els = pblock();
    }
  }
  return s;
}

static Stmt *pfor(void) {
  Stmt *s = new_stmt(S_FOR, next()->pos);
  expect(P_LPAREN, "after for (a loop looks like for (int i = 0; i < n; i++))");
  if (pk() != P_SEMI) s->init = psimple();
  expect(P_SEMI, "after the loop initializer");
  if (pk() != P_SEMI) s->cond = pexpr();
  expect(P_SEMI, "after the loop condition");
  if (pk() != P_RPAREN) s->post = psimple();
  expect(P_RPAREN, "to close the for header");
  if (s->init && s->init->k != S_ASSIGN) die(s->init->pos, "the loop initializer must be a declaration or an assignment");
  if (s->post && s->post->k == S_EXPR) die(s->post->pos, "the loop post statement must be an assignment, ++ or --");
  s->body = pblock();
  return s;
}

static void pparams(Params *ps, const char *ctx) {
  skip_nl();
  while (pk() != P_RPAREN) {
    Param p = {0};
    p.te = ptype(true);
    p.pos = peek()->pos;
    p.name = expect_ident(ctx);
    PUSH(*ps, p);
    skip_nl();
    if (!accept(P_COMMA)) break;
    skip_nl();
  }
  expect(P_RPAREN, "to close the parameter list");
}

static Label plabel(void) {
  Label l = {0};
  l.pos = peek()->pos;
  if (pk() == T_IDENT && pkn(1) == P_DOT && pkn(2) == T_IDENT) {
    int k3 = pkn(3);
    if (k3 == P_COLON || k3 == P_COMMA) {
      l.pattern = true;
      l.tname = next()->s;
      next();
      l.cname = next()->s;
      Expr *b = new_expr(E_IDENT, l.pos);
      b->s = l.tname;
      l.e = new_expr(E_FIELD, l.pos);
      l.e->a = b;
      l.e->s = l.cname;
      return l;
    }
    if (k3 == P_LPAREN) {
      int save = tpos;
      Token *tn = next();
      next();
      Token *cn = next();
      next();
      bool pat = pk() == P_RPAREN;
      if (!pat) {
        int s2 = tpos;
        TypeExpr *te = ptype(false);
        pat = te && pk() == T_IDENT;
        tpos = s2;
      }
      if (pat) {
        l.pattern = true;
        l.parens = true;
        l.tname = tn->s;
        l.cname = cn->s;
        pparams(&l.params, "for the binding");
        return l;
      }
      tpos = save;
    }
  }
  l.e = pexpr();
  return l;
}

static Stmt *pswitch(void) {
  Stmt *s = new_stmt(S_SWITCH, next()->pos);
  expect(P_LPAREN, "after switch (the value is always in parentheses)");
  s->e = pexpr();
  expect(P_RPAREN, "to close the switch value");
  expect(P_LBRACE, "to open the switch body");
  skip_nl();
  while (pk() == K_CASE || pk() == K_DEFAULT) {
    Case c = {0};
    c.pos = peek()->pos;
    if (next()->k == K_DEFAULT) {
      c.isdefault = true;
    } else {
      for (;;) {
        PUSH(c.labels, plabel());
        if (!accept(P_COMMA)) break;
        skip_nl();
      }
    }
    expect(P_COLON, "after the case");
    skip_nl();
    while (pk() != K_CASE && pk() != K_DEFAULT && pk() != P_RBRACE && pk() != T_EOF) {
      PUSH(c.body, pstmt());
      if (pk() != P_RBRACE) expect(T_NL, "");
      skip_nl();
    }
    PUSH(s->cases, c);
  }
  expect(P_RBRACE, "to close the switch (expected case, default or '}')");
  return s;
}

static Stmt *pstmt(void) {
  Token *t = peek();
  switch (t->k) {
  case K_IF: return pif();
  case K_FOR: return pfor();
  case K_SWITCH: return pswitch();
  case K_RETURN: {
    Stmt *s = new_stmt(S_RETURN, next()->pos);
    if (pk() != T_NL && pk() != P_RBRACE) {
      for (;;) {
        PUSH(s->exprs, pexpr());
        if (!accept(P_COMMA)) break;
        skip_nl();
      }
    }
    return s;
  }
  case K_BREAK: next(); return new_stmt(S_BREAK, t->pos);
  case K_CONTINUE: next(); return new_stmt(S_CONTINUE, t->pos);
  case K_CONST: {
    Stmt *s = new_stmt(S_CONST, next()->pos);
    s->te = ptype(true);
    s->pos = peek()->pos;
    s->name = expect_ident("for the constant");
    expect(P_ASSIGN, "after the constant name (a constant needs a value)");
    skip_nl();
    s->e = pexpr();
    return s;
  }
  case K_DEFER: unsupported(t->pos, "defer");
  case K_SELECT: unsupported(t->pos, "select");
  case K_SPAWN: unsupported(t->pos, "spawn");
  case K_ASSERT: unsupported(t->pos, "assert");
  case K_ELSE: die(t->pos, "else must be on the same line as the closing '}' of the if");
  case K_CASE:
  case K_DEFAULT: die(t->pos, "%s outside of a switch", tok_names[t->k]);
  case P_LBRACE: die(t->pos, "unexpected '{': a block must belong to an if, for or switch");
  default: return psimple();
  }
}

static Block pblock(void) {
  Block b = {0};
  expect(P_LBRACE, "to open the block (braces go on the same line)");
  skip_nl();
  while (pk() != P_RBRACE) {
    if (pk() == T_EOF) die(peek()->pos, "unexpected end of file: missing '}'");
    PUSH(b, pstmt());
    if (pk() != P_RBRACE) expect(T_NL, "");
    skip_nl();
  }
  next();
  return b;
}

static void add_global(const char *name, int k, void *p, Pos pos) {
  Global *g = hget(&globals, name);
  if (g) die(pos, "%s is already declared at %s:%d:%d", name, g->pos.file, g->pos.line, g->pos.col);
  g = alloc(sizeof *g);
  g->k = k;
  g->p = p;
  g->pos = pos;
  hput(&globals, name, g);
}

static void pfn(void) {
  Pos fpos = next()->pos;
  FnDecl *f = alloc(sizeof *f);
  if (accept(P_LPAREN)) {
    f->is_method = true;
    TypeExpr *te = ptype(true);
    if (te->k != TE_NAME) die(te->pos, "a method receiver must be a struct or enum type declared in this package");
    f->recv_tname = te->name;
    f->recv_pos = te->pos;
    f->recv_name = expect_ident("for the receiver");
    expect(P_RPAREN, "after the receiver");
    if (accept(K_MUT)) f->recv_mut = true;
  }
  f->pos = peek()->pos;
  f->name = expect_ident("for the function");
  (void)fpos;
  expect(P_LPAREN, "after the function name");
  pparams(&f->params, "for the parameter (parameters are written Type name)");
  if (pk() == P_LPAREN) {
    next();
    for (;;) {
      PUSH(f->results, ptype(true));
      if (!accept(P_COMMA)) break;
    }
    expect(P_RPAREN, "to close the result list");
  } else if (pk() != P_LBRACE) {
    PUSH(f->results, ptype(true));
  }
  f->body = pblock();
  PUSH(fns, f);
  if (!f->is_method) add_global(f->name, G_FN, f, f->pos);
}

static void ptypedecl(void) {
  next();
  TypeDecl *td = alloc(sizeof *td);
  td->pos = peek()->pos;
  td->name = expect_ident("for the type");
  Token *k = next();
  if (k->k == K_INTERFACE) unsupported(k->pos, "interface");
  if (k->k != K_STRUCT && k->k != K_ENUM) die(k->pos, "expected struct or enum after the type name");
  if (pk() == K_IMPLEMENTS) unsupported(peek()->pos, "implements");
  td->is_enum = k->k == K_ENUM;
  expect(P_LBRACE, "to open the type body");
  skip_nl();
  while (pk() != P_RBRACE) {
    if (td->is_enum) {
      ECase c = {0};
      c.pos = peek()->pos;
      c.name = expect_ident("for the enum case");
      if (accept(P_LPAREN)) pparams(&c.params, "for the case field");
      PUSH(td->cases, c);
    } else {
      Param p = {0};
      p.te = ptype(true);
      p.pos = peek()->pos;
      p.name = expect_ident("for the field (fields are written Type name)");
      PUSH(td->fields, p);
    }
    if (pk() != P_RBRACE) expect(T_NL, "");
    skip_nl();
  }
  next();
  PUSH(tdecls, td);
  add_global(td->name, G_TYPE, td, td->pos);
}

static void pconst(void) {
  next();
  ConstDecl *c = alloc(sizeof *c);
  c->te = ptype(true);
  c->pos = peek()->pos;
  c->name = expect_ident("for the constant");
  expect(P_ASSIGN, "after the constant name (a constant needs a value)");
  skip_nl();
  c->e = pexpr();
  PUSH(cdecls, c);
  add_global(c->name, G_CONST, c, c->pos);
}

static const char *pkg_name;

static void parse_file(const char *file, Tokens *toks) {
  tk = toks->v;
  tpos = 0;
  skip_nl();
  expect(K_PACKAGE, "at the start of the file (every file starts with 'package name')");
  Token *pn = peek();
  const char *name = expect_ident("for the package");
  if (!pkg_name) pkg_name = name;
  else if (strcmp(pkg_name, name)) die(pn->pos, "all files must belong to one package: found %s, expected %s", name, pkg_name);
  expect(T_NL, "after the package name");
  skip_nl();
  while (pk() == K_IMPORT) {
    next();
    Token *s = expect(T_STRING, "after import");
    if (strcmp(s->s, "fmt") && strcmp(s->s, "os") && strcmp(s->s, "fs") && strcmp(s->s, "errors"))
      die(s->pos, "seed0 can only import the core packages fmt, os, fs and errors, not \"%s\"", s->s);
    hput(&imported, s->s, (void *)1);
    expect(T_NL, "after the import");
    skip_nl();
  }
  while (pk() != T_EOF) {
    accept(K_PUB);
    Token *t = peek();
    switch (t->k) {
    case K_FN: pfn(); break;
    case K_TYPE: ptypedecl(); break;
    case K_CONST: pconst(); break;
    case K_TEST: unsupported(t->pos, "test");
    case K_IMPORT: die(t->pos, "imports must come before every declaration");
    default: die(t->pos, "expected fn, type or const at the top level, found %s", tok_desc(t));
    }
    if (pk() != T_EOF) expect(T_NL, "");
    skip_nl();
  }
  (void)file;
}

/* ======================================================================
 * Types
 * ====================================================================== */

enum {
  TY_VOID, TY_INT, TY_FLOAT, TY_BOOL, TY_BYTE, TY_STRING, TY_ERROR, TY_NULL,
  TY_LIST, TY_MAP, TY_OPT, TY_STRUCT, TY_ENUM, TY_TUPLE
};

struct Type {
  int k;
  Type *elem, *key; /* list/opt element; map key and value (elem) */
  TypeDecl *td;
  Type **parts;
  int np;
  const char *mangle; /* unique, C-identifier-safe */
  const char *name;   /* as written in Seed */
  unsigned helpers;   /* generated helper functions, see H_* */
  int rt;             /* needs retain on copy: -1 unknown, 0 no, 1 yes, 2 computing */
};

static HMap typetab;
static VEC(Type *) tuples;
static Type *t_void, *t_int, *t_float, *t_bool, *t_byte, *t_string, *t_error, *t_null;

static Type *mktype(int k, const char *mangle, const char *name, bool *fresh) {
  Type *t = hget(&typetab, mangle);
  if (fresh) *fresh = !t;
  if (t) return t;
  t = alloc(sizeof *t);
  t->k = k;
  t->mangle = mangle;
  t->name = name;
  t->rt = -1;
  hput(&typetab, mangle, t);
  return t;
}

static Type *t_list(Type *e) {
  Type *t = mktype(TY_LIST, fmt("L%s", e->mangle), e->k == TY_OPT ? fmt("[](%s)", e->name) : fmt("[]%s", e->name), NULL);
  t->elem = e;
  return t;
}

static Type *t_map(Type *k, Type *v) {
  Type *t = mktype(TY_MAP, fmt("M%s%s", k->mangle, v->mangle), fmt("map[%s]%s", k->name, v->name), NULL);
  t->key = k;
  t->elem = v;
  return t;
}

static Type *t_opt(Type *e) {
  Type *t = mktype(TY_OPT, fmt("O%s", e->mangle), fmt("%s?", e->name), NULL);
  t->elem = e;
  return t;
}

static Type *t_tuple(Type **parts, int n) {
  Buf m = {0}, nm = {0};
  bprintf(&m, "T%d", n);
  bput(&nm, "(");
  for (int i = 0; i < n; i++) {
    bput(&m, parts[i]->mangle);
    bprintf(&nm, "%s%s", i ? ", " : "", parts[i]->name);
  }
  bput(&nm, ")");
  bool fresh;
  Type *t = mktype(TY_TUPLE, sdup(m.s), sdup(nm.s), &fresh);
  free(m.s);
  free(nm.s);
  if (fresh) {
    t->parts = alloc(sizeof(Type *) * (size_t)n);
    memcpy(t->parts, parts, sizeof(Type *) * (size_t)n);
    t->np = n;
    PUSH(tuples, t);
  }
  return t;
}

static Type *t_tuple2(Type *a, Type *b) {
  Type *p[2] = {a, b};
  return t_tuple(p, 2);
}

static void init_types(void) {
  t_void = mktype(TY_VOID, "v", "void", NULL);
  t_int = mktype(TY_INT, "i", "int", NULL);
  t_float = mktype(TY_FLOAT, "f", "float", NULL);
  t_bool = mktype(TY_BOOL, "b", "bool", NULL);
  t_byte = mktype(TY_BYTE, "y", "byte", NULL);
  t_string = mktype(TY_STRING, "s", "string", NULL);
  t_error = mktype(TY_ERROR, "e", "error", NULL);
  t_null = mktype(TY_NULL, "n", "null", NULL);
}


/* ---- "did you mean" suggestions ---- */

static int edit_distance(const char *a, const char *b) {
  int la = (int)strlen(a), lb = (int)strlen(b);
  if (la > 64 || lb > 64) return 99;
  int d[65][65];
  for (int i = 0; i <= la; i++) d[i][0] = i;
  for (int j = 0; j <= lb; j++) d[0][j] = j;
  for (int i = 1; i <= la; i++)
    for (int j = 1; j <= lb; j++) {
      int c = d[i - 1][j - 1] + (tolower((unsigned char)a[i - 1]) != tolower((unsigned char)b[j - 1]));
      if (d[i - 1][j] + 1 < c) c = d[i - 1][j] + 1;
      if (d[i][j - 1] + 1 < c) c = d[i][j - 1] + 1;
      d[i][j] = c;
    }
  return d[la][lb];
}

typedef struct {
  const char *best;
  int dist;
  const char *name;
} Suggest;

static void consider(Suggest *s, const char *cand) {
  if (!cand || !strcmp(cand, "_")) return;
  int d = edit_distance(s->name, cand);
  int limit = (int)strlen(s->name) / 3 + 1;
  if (limit > 3) limit = 3;
  if (d <= limit && d < s->dist) {
    s->dist = d;
    s->best = cand;
  }
}

static const char *hint(Suggest *s) { return s->best ? fmt(" (did you mean %s?)", s->best) : ""; }

static const char *builtin_type_names[] = {"int", "float", "bool", "byte", "string", "error"};

static Type *resolve(TypeExpr *te) {
  switch (te->k) {
  case TE_NAME: {
    const char *n = te->name;
    if (!strcmp(n, "int")) return t_int;
    if (!strcmp(n, "float")) return t_float;
    if (!strcmp(n, "bool")) return t_bool;
    if (!strcmp(n, "byte")) return t_byte;
    if (!strcmp(n, "string")) return t_string;
    if (!strcmp(n, "error")) return t_error;
    Global *g = hget(&globals, n);
    if (g && g->k == G_TYPE) return ((TypeDecl *)g->p)->t;
    if (g) die(te->pos, "%s is not a type", n);
    Suggest s = {0, 99, n};
    for (int i = 0; i < 6; i++) consider(&s, builtin_type_names[i]);
    for (int i = 0; i < tdecls.n; i++) consider(&s, tdecls.v[i]->name);
    die(te->pos, "unknown type %s%s", n, hint(&s));
  }
  case TE_LIST: return t_list(resolve(te->a));
  case TE_MAP: {
    Type *k = resolve(te->a), *v = resolve(te->b);
    if (k->k != TY_INT && k->k != TY_BYTE && k->k != TY_BOOL && k->k != TY_STRING)
      die(te->a->pos, "map keys must be int, byte, bool or string, not %s", k->name);
    if (v->k == TY_OPT)
      die(te->b->pos, "map values cannot be nullable: m[k] already returns %s, and %s? is not a valid type", v->name, v->name);
    return t_map(k, v);
  }
  case TE_OPT: {
    Type *in = resolve(te->a);
    if (in->k == TY_ERROR) die(te->pos, "error is already nullable; write error instead of error?");
    if (in->k == TY_OPT) die(te->pos, "T?? is not a valid type");
    return t_opt(in);
  }
  }
  return NULL;
}

/* ======================================================================
 * Checker: resolves names and types, annotates the AST
 * ====================================================================== */

enum { VK_LOCAL, VK_PARAM, VK_RECV, VK_MUTRECV, VK_LCONST };

struct Var {
  const char *name;
  Type *t;
  int k;
  int id;
  Pos pos;
};

typedef struct {
  const char *key;
  bool dead;
} Narrow;

static VEC(Var *) scope;
static VEC(int) scope_marks;
static VEC(Narrow) narrows;
static VEC(int) narrow_marks;
static FnDecl *cur_fn;
static int loop_depth;
static int var_ids;
static HMap mut_names; /* names of user mut methods, for loop narrowing scans */

enum {
  B_LEN = 1, B_STRING, B_SSLICE, B_SCONTAINS, B_SINDEX, B_SSTARTS, B_SENDS, B_SPLIT, B_TRIM, B_REPLACE,
  B_UPPER, B_LOWER, B_TOINT, B_TOFLOAT, B_BYTES, B_PUSH, B_POP, B_INSERT, B_REMOVE, B_LCONTAINS,
  B_LINDEX, B_LSLICE, B_REVERSE, B_SORT, B_JOIN, B_DELETE, B_KEYS, B_MESSAGE
};

enum {
  F_PRINT = 1, F_PRINTLN, F_EPRINT, F_EPRINTLN, F_EXIT, F_ENV, F_RUN, F_READ, F_WRITE, F_EXISTS,
  F_ISDIR, F_LIST, F_MKDIR, F_REMOVE, F_ERRNEW, F_EXECUTABLE, F_CWD
};

static const struct {
  const char *pkg, *name;
  int id;
} pkgfns[] = {
  {"fmt", "print", F_PRINT}, {"fmt", "println", F_PRINTLN}, {"fmt", "eprint", F_EPRINT},
  {"fmt", "eprintln", F_EPRINTLN}, {"os", "exit", F_EXIT}, {"os", "env", F_ENV}, {"os", "run", F_RUN},
  {"fs", "read", F_READ}, {"fs", "write", F_WRITE}, {"fs", "exists", F_EXISTS}, {"fs", "isDir", F_ISDIR},
  {"fs", "list", F_LIST}, {"fs", "mkdir", F_MKDIR}, {"fs", "remove", F_REMOVE}, {"errors", "new", F_ERRNEW},
  {"os", "executable", F_EXECUTABLE}, {"os", "cwd", F_CWD},
};

static bool is_pkg(const char *n) {
  return !strcmp(n, "fmt") || !strcmp(n, "os") || !strcmp(n, "fs") || !strcmp(n, "errors");
}

static void push_scope(void) {
  PUSH(scope_marks, scope.n);
  PUSH(narrow_marks, narrows.n);
}

static void pop_scope(void) {
  scope.n = scope_marks.v[--scope_marks.n];
  narrows.n = narrow_marks.v[--narrow_marks.n];
}

static Var *lookup_var(const char *name) {
  for (int i = scope.n - 1; i >= 0; i--)
    if (!strcmp(scope.v[i]->name, name)) return scope.v[i];
  return NULL;
}

static Var *declare(const char *name, Type *t, int k, Pos pos) {
  if (!strcmp(name, "_")) return NULL;
  Var *old = lookup_var(name);
  if (old)
    die(pos, "%s is already declared at line %d; shadowing is not allowed (assign with = or choose another name)", name,
        old->pos.line);
  Var *v = alloc(sizeof *v);
  v->name = name;
  v->t = t;
  v->k = k;
  v->id = ++var_ids;
  v->pos = pos;
  PUSH(scope, v);
  return v;
}

/* ---- narrowing ---- */

/* Key for a narrowable path: a variable followed by struct fields. */
static const char *path_key(Expr *e) {
  if (e->k == E_IDENT) {
    Var *v = e->var ? e->var : lookup_var(e->s);
    return v ? fmt("v%d", v->id) : NULL;
  }
  if (e->k == E_FIELD && e->sub != FK_ENUMVAL) {
    const char *b = path_key(e->a);
    return b ? fmt("%s.%s", b, e->s) : NULL;
  }
  return NULL;
}

static bool is_narrowed(const char *key) {
  for (int i = narrows.n - 1; i >= 0; i--)
    if (!strcmp(narrows.v[i].key, key)) return !narrows.v[i].dead;
  return false;
}

static void add_narrow(const char *key) { PUSH(narrows, ((Narrow){key, false})); }

static bool key_extends(const char *k, const char *prefix, bool strict) {
  size_t n = strlen(prefix);
  if (strncmp(k, prefix, n)) return false;
  if (k[n] == 0) return !strict;
  return k[n] == '.';
}

/* The path was assigned: it and everything under it may be null again. */
static void kill_assign(const char *key) {
  if (!key) return;
  for (int i = 0; i < narrows.n; i++)
    if (key_extends(narrows.v[i].key, key, false)) narrows.v[i].dead = true;
}

/* A mut method was called on the path: everything under it may change. */
static void kill_mut(const char *key) {
  if (!key) return;
  for (int i = 0; i < narrows.n; i++)
    if (key_extends(narrows.v[i].key, key, true)) narrows.v[i].dead = true;
}

typedef VEC(const char *) Keys;

static void cond_keys(Expr *e, bool when, Keys *out) {
  if (e->k == E_BINARY) {
    if ((e->op == P_NE && when) || (e->op == P_EQ && !when)) {
      Expr *x = e->a->k == E_NULL ? e->b : e->b->k == E_NULL ? e->a : NULL;
      if (x) {
        const char *k = path_key(x);
        if (k) PUSH(*out, k);
      }
    } else if ((e->op == P_ANDAND && when) || (e->op == P_OROR && !when)) {
      cond_keys(e->a, when, out);
      cond_keys(e->b, when, out);
    }
  } else if (e->k == E_UNARY && e->op == P_NOT) {
    cond_keys(e->a, !when, out);
  }
}

static void narrow_on(Expr *cond, bool when) {
  Keys ks = {0};
  cond_keys(cond, when, &ks);
  for (int i = 0; i < ks.n; i++) add_narrow(ks.v[i]);
  free(ks.v);
}

/* Before a loop: any path assigned inside the loop is not narrowed at its start. */
static void scan_expr_kills(Expr *e);

static void scan_block_kills(Block *b);

static void scan_stmt_kills(Stmt *s) {
  if (!s) return;
  switch (s->k) {
  case S_ASSIGN:
    for (int i = 0; i < s->targets.n; i++)
      if (s->targets.v[i].lv) {
        kill_assign(path_key(s->targets.v[i].lv));
        scan_expr_kills(s->targets.v[i].lv);
      }
    scan_expr_kills(s->e);
    break;
  case S_INCDEC: kill_assign(path_key(s->e)); break;
  case S_EXPR: case S_CONST: scan_expr_kills(s->e); break;
  case S_RETURN:
    for (int i = 0; i < s->exprs.n; i++) scan_expr_kills(s->exprs.v[i]);
    break;
  case S_IF:
    scan_expr_kills(s->cond);
    scan_block_kills(&s->body);
    scan_block_kills(&s->els);
    break;
  case S_FOR:
    scan_stmt_kills(s->init);
    if (s->cond) scan_expr_kills(s->cond);
    scan_stmt_kills(s->post);
    scan_block_kills(&s->body);
    break;
  case S_SWITCH:
    scan_expr_kills(s->e);
    for (int i = 0; i < s->cases.n; i++) scan_block_kills(&s->cases.v[i].body);
    break;
  }
}

static void scan_block_kills(Block *b) {
  for (int i = 0; i < b->n; i++) scan_stmt_kills(b->v[i]);
}

static bool is_builtin_mut(const char *n) {
  return !strcmp(n, "push") || !strcmp(n, "pop") || !strcmp(n, "insert") || !strcmp(n, "remove") ||
         !strcmp(n, "reverse") || !strcmp(n, "sort") || !strcmp(n, "delete");
}

static void scan_expr_kills(Expr *e) {
  if (!e) return;
  if (e->k == E_CALL && e->a->k == E_FIELD && (is_builtin_mut(e->a->s) || hget(&mut_names, e->a->s)))
    kill_mut(path_key(e->a->a));
  scan_expr_kills(e->a);
  scan_expr_kills(e->b);
  for (int i = 0; i < e->args.n; i++) scan_expr_kills(e->args.v[i]);
  for (int i = 0; i < e->keys.n; i++) scan_expr_kills(e->keys.v[i]);
}

/* ---- expressions ---- */

static void check_expr(Expr *e);
static bool check_block(Block *b);
static void check_const(ConstDecl *c);

static const char *expr_text(Expr *e) {
  if (e->k == E_IDENT) return e->s;
  if (e->k == E_FIELD) {
    const char *b = expr_text(e->a);
    return b ? fmt("%s.%s", b, e->s) : NULL;
  }
  if (e->k == E_CALL) {
    const char *b = expr_text(e->a);
    return b ? fmt("%s()", b) : NULL;
  }
  return NULL;
}

static const char *describe(Expr *e) {
  const char *t = expr_text(e);
  return t ? t : "the value";
}

static void not_value(Expr *e) {
  if (e->t == t_void) die(e->pos, "%s does not return a value", describe(e));
  if (e->t->k == TY_TUPLE)
    die(e->pos, "%s returns %d values %s; assign them with a declaration like  T a, error err = ...", describe(e),
        e->t->np, e->t->name);
}

static Expr *coerce(Expr *e, Type *want, const char *ctx) {
  Type *t = e->t;
  if (t == want) return e;
  if (t == t_void || t->k == TY_TUPLE) not_value(e);
  if (t == t_null && (want->k == TY_OPT || want == t_error)) {
    e->t = want;
    return e;
  }
  if (want->k == TY_OPT && t == want->elem) {
    if (e->narrowed && e->orig == want) {
      e->narrowed = false;
      e->t = want;
      return e;
    }
    Expr *w = new_expr(E_WRAP, e->pos);
    w->a = e;
    w->t = want;
    return w;
  }
  if (t == t_null) die(e->pos, "null cannot be used as %s %s: only nullable types (T?) and error can be null", want->name, ctx);
  if (t->k == TY_OPT && t->elem == want)
    die(e->pos, "%s is nullable (%s) and cannot be used as %s %s; check it against null first", describe(e), t->name,
        want->name, ctx);
  die(e->pos, "cannot use %s (type %s) as %s %s; there are no implicit conversions", describe(e), t->name, want->name, ctx);
}

static Expr *check_value(Expr *e, Type *want, const char *ctx) {
  check_expr(e);
  return coerce(e, want, ctx);
}

static void check_args(Expr *call, Type **params, int n, const char *what) {
  if (call->args.n != n)
    die(call->pos, "%s takes %d argument%s but %d %s given", what, n, n == 1 ? "" : "s", call->args.n,
        call->args.n == 1 ? "was" : "were");
  for (int i = 0; i < n; i++)
    call->args.v[i] = check_value(call->args.v[i], params[i], fmt("as argument %d of %s", i + 1, what));
}

static Type *fn_ret(FnDecl *f) { return f->ret; }

static void apply_narrowing(Expr *e) {
  if (e->t->k != TY_OPT) return;
  const char *k = path_key(e);
  if (k && is_narrowed(k)) {
    e->narrowed = true;
    e->orig = e->t;
    e->t = e->t->elem;
  }
}

static Param *find_field(TypeDecl *td, const char *name) {
  for (int i = 0; i < td->fields.n; i++)
    if (!strcmp(td->fields.v[i].name, name)) return &td->fields.v[i];
  return NULL;
}

static int find_case(TypeDecl *td, const char *name) {
  for (int i = 0; i < td->cases.n; i++)
    if (!strcmp(td->cases.v[i].name, name)) return i;
  return -1;
}

static void unknown_case(Pos p, TypeDecl *td, const char *name) {
  Suggest s = {0, 99, name};
  for (int i = 0; i < td->cases.n; i++) consider(&s, td->cases.v[i].name);
  die(p, "enum %s has no case %s%s", td->name, name, hint(&s));
}

static void unknown_field(Pos p, TypeDecl *td, const char *name) {
  Suggest s = {0, 99, name};
  for (int i = 0; i < td->fields.n; i++) consider(&s, td->fields.v[i].name);
  die(p, "struct %s has no field %s%s", td->name, name, hint(&s));
}

/* A type name used in expression position (for Type.Case and Type{...}). */
static TypeDecl *type_named(Expr *e) {
  if (e->k != E_IDENT || lookup_var(e->s)) return NULL;
  Global *g = hget(&globals, e->s);
  return g && g->k == G_TYPE ? g->p : NULL;
}

static bool pkg_named(Expr *e) {
  return e->k == E_IDENT && !lookup_var(e->s) && !hget(&globals, e->s) && is_pkg(e->s);
}

/* Checks that e (already checked) is a place that may be modified. */
static void require_mutable(Expr *e, const char *what) {
  Expr *r = e;
  for (;;) {
    if (r->k == E_IDENT) break;
    if (r->k == E_FIELD && r->sub == FK_FIELD) { r = r->a; continue; }
    if (r->k == E_INDEX && (r->sub == IK_LIST || (r->sub == IK_MAP && r == e))) { r = r->a; continue; }
    if (r->k == E_INDEX && r->sub == IK_STRING) die(e->pos, "strings are immutable; build a new string instead");
    die(e->pos, "cannot %s a temporary value; store it in a local variable first", what);
  }
  Var *v = r->var;
  if (!v) die(e->pos, "cannot %s the constant %s", what, r->s);
  if (v->k == VK_PARAM || v->k == VK_RECV)
    die(e->pos, "cannot %s %s: parameters are read-only (copy it into a local variable first, or make the method mut)", what,
        describe(e));
  if (v->k == VK_LCONST) die(e->pos, "cannot %s the constant %s", what, v->name);
}

static void check_lvalue(Expr *e) {
  switch (e->k) {
  case E_IDENT: {
    Var *v = lookup_var(e->s);
    if (!v) {
      Global *g = hget(&globals, e->s);
      if (g && g->k == G_CONST) die(e->pos, "cannot assign to the constant %s", e->s);
      die(e->pos, "undefined: %s", e->s);
    }
    e->var = v;
    e->t = v->t;
    break;
  }
  case E_FIELD: {
    if (type_named(e->a)) die(e->pos, "cannot assign to an enum case");
    check_expr(e->a);
    Type *bt = e->a->t;
    if (bt->k == TY_OPT) die(e->a->pos, "%s is nullable (%s); check it against null first", describe(e->a), bt->name);
    if (bt->k != TY_STRUCT) die(e->pos, "%s has no field %s", bt->name, e->s);
    Param *f = find_field(bt->td, e->s);
    if (!f) unknown_field(e->pos, bt->td, e->s);
    e->sub = FK_FIELD;
    e->t = f->t;
    break;
  }
  case E_INDEX: {
    check_expr(e->a);
    Type *bt = e->a->t;
    if (bt->k == TY_LIST) {
      e->sub = IK_LIST;
      e->b = check_value(e->b, t_int, "as a list index");
      e->t = bt->elem;
    } else if (bt->k == TY_MAP) {
      e->sub = IK_MAP;
      e->b = check_value(e->b, bt->key, "as a map key");
      e->t = bt->elem;
    } else if (bt->k == TY_STRING) {
      die(e->pos, "strings are immutable; build a new string instead");
    } else if (bt->k == TY_OPT) {
      die(e->a->pos, "%s is nullable (%s); check it against null first", describe(e->a), bt->name);
    } else {
      die(e->pos, "cannot index %s", bt->name);
    }
    break;
  }
  default:
    die(e->pos, "cannot assign to this expression");
  }
  require_mutable(e, "assign to");
}

static void check_builtin(Expr *e, Expr *recv, const char *name) {
  Type *rt = recv->t;
  Type *p1[2];
  int bid = 0;
  Type *res = NULL;
  int np = 0;
  bool mut = false;
  switch (rt->k) {
  case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_BYTE:
    if (!strcmp(name, "string")) { bid = B_STRING; res = t_string; }
    break;
  case TY_STRING:
    if (!strcmp(name, "len")) { bid = B_LEN; res = t_int; }
    else if (!strcmp(name, "string")) { bid = B_STRING; res = t_string; }
    else if (!strcmp(name, "slice")) { bid = B_SSLICE; np = 2; p1[0] = p1[1] = t_int; res = t_string; }
    else if (!strcmp(name, "contains")) { bid = B_SCONTAINS; np = 1; p1[0] = t_string; res = t_bool; }
    else if (!strcmp(name, "index")) { bid = B_SINDEX; np = 1; p1[0] = t_string; res = t_opt(t_int); }
    else if (!strcmp(name, "startsWith")) { bid = B_SSTARTS; np = 1; p1[0] = t_string; res = t_bool; }
    else if (!strcmp(name, "endsWith")) { bid = B_SENDS; np = 1; p1[0] = t_string; res = t_bool; }
    else if (!strcmp(name, "split")) { bid = B_SPLIT; np = 1; p1[0] = t_string; res = t_list(t_string); }
    else if (!strcmp(name, "trim")) { bid = B_TRIM; res = t_string; }
    else if (!strcmp(name, "replace")) { bid = B_REPLACE; np = 2; p1[0] = p1[1] = t_string; res = t_string; }
    else if (!strcmp(name, "toUpper")) { bid = B_UPPER; res = t_string; }
    else if (!strcmp(name, "toLower")) { bid = B_LOWER; res = t_string; }
    else if (!strcmp(name, "toInt")) { bid = B_TOINT; res = t_tuple2(t_int, t_error); }
    else if (!strcmp(name, "toFloat")) { bid = B_TOFLOAT; res = t_tuple2(t_float, t_error); }
    else if (!strcmp(name, "bytes")) { bid = B_BYTES; res = t_list(t_byte); }
    else if (!strcmp(name, "chars")) unsupported(e->pos, "string.chars()");
    break;
  case TY_LIST: {
    Type *el = rt->elem;
    if (!strcmp(name, "len")) { bid = B_LEN; res = t_int; }
    else if (!strcmp(name, "push")) { bid = B_PUSH; np = 1; p1[0] = el; res = t_void; mut = true; }
    else if (!strcmp(name, "pop")) {
      if (el->k == TY_OPT) die(e->pos, "pop() is not available on a list of nullable values in seed0 (%s? is not a valid type)", el->name);
      bid = B_POP; res = t_opt(el); mut = true;
    }
    else if (!strcmp(name, "insert")) { bid = B_INSERT; np = 2; p1[0] = t_int; p1[1] = el; res = t_void; mut = true; }
    else if (!strcmp(name, "remove")) { bid = B_REMOVE; np = 1; p1[0] = t_int; res = t_void; mut = true; }
    else if (!strcmp(name, "contains")) { bid = B_LCONTAINS; np = 1; p1[0] = el; res = t_bool; }
    else if (!strcmp(name, "index")) { bid = B_LINDEX; np = 1; p1[0] = el; res = t_opt(t_int); }
    else if (!strcmp(name, "slice")) { bid = B_LSLICE; np = 2; p1[0] = p1[1] = t_int; res = rt; }
    else if (!strcmp(name, "reverse")) { bid = B_REVERSE; res = t_void; mut = true; }
    else if (!strcmp(name, "sort")) {
      if (el != t_int && el != t_float && el != t_string && el != t_byte)
        die(e->pos, "sort() only works on []int, []float, []string and []byte, not %s", rt->name);
      bid = B_SORT; res = t_void; mut = true;
    }
    else if (!strcmp(name, "join")) {
      if (el != t_string) die(e->pos, "join() only works on []string, not %s", rt->name);
      bid = B_JOIN; np = 1; p1[0] = t_string; res = t_string;
    }
    else if (!strcmp(name, "sortBy")) unsupported(e->pos, "sortBy (function values)");
    break;
  }
  case TY_MAP:
    if (!strcmp(name, "len")) { bid = B_LEN; res = t_int; }
    else if (!strcmp(name, "delete")) { bid = B_DELETE; np = 1; p1[0] = rt->key; res = t_void; mut = true; }
    else if (!strcmp(name, "keys")) { bid = B_KEYS; res = t_list(rt->key); }
    break;
  case TY_ERROR:
    if (!strcmp(name, "message")) { bid = B_MESSAGE; res = t_string; }
    break;
  case TY_OPT:
    die(recv->pos, "%s is nullable (%s); check it against null before calling %s()", describe(recv), rt->name, name);
  default:
    break;
  }
  if (!bid) die(e->pos, "%s has no method %s", rt->name, name);
  if (mut) {
    require_mutable(recv, fmt("call the mut method %s() on", name));
    kill_mut(path_key(recv));
  }
  check_args(e, p1, np, fmt("%s()", name));
  e->sub = CK_BUILTIN;
  e->bid = bid;
  e->t = res;
}

static void check_pkg_call(Expr *e, const char *pkg, const char *name) {
  int id = 0;
  for (size_t i = 0; i < sizeof pkgfns / sizeof pkgfns[0]; i++)
    if (!strcmp(pkgfns[i].pkg, pkg) && !strcmp(pkgfns[i].name, name)) id = pkgfns[i].id;
  if (!id) {
    Suggest s = {0, 99, name};
    for (size_t i = 0; i < sizeof pkgfns / sizeof pkgfns[0]; i++)
      if (!strcmp(pkgfns[i].pkg, pkg)) consider(&s, pkgfns[i].name);
    die(e->pos, "package %s has no function %s%s", pkg, name, hint(&s));
  }
  if (!hget(&imported, pkg)) die(e->pos, "package %s is not imported; add  import \"%s\"  at the top of the file", pkg, pkg);
  Type *p[1] = {t_string};
  Type *res = t_void;
  int np = 1;
  switch (id) {
  case F_PRINT: case F_PRINTLN: case F_EPRINT: case F_EPRINTLN: break;
  case F_EXIT: p[0] = t_int; break;
  case F_ENV: res = t_opt(t_string); break;
  case F_RUN: p[0] = t_list(t_string); res = t_tuple2(t_int, t_error); break;
  case F_READ: res = t_tuple2(t_string, t_error); break;
  case F_WRITE: {
    Type *p2[2] = {t_string, t_string};
    check_args(e, p2, 2, fmt("%s.%s", pkg, name));
    e->sub = CK_PKG;
    e->bid = id;
    e->t = t_error;
    return;
  }
  case F_EXISTS: case F_ISDIR: res = t_bool; break;
  case F_LIST: res = t_tuple2(t_list(t_string), t_error); break;
  case F_MKDIR: case F_REMOVE: res = t_error; break;
  case F_ERRNEW: res = t_error; break;
  case F_EXECUTABLE: np = 0; res = t_string; break;
  case F_CWD: np = 0; res = t_tuple2(t_string, t_error); break;
  }
  check_args(e, p, np, fmt("%s.%s", pkg, name));
  e->sub = CK_PKG;
  e->bid = id;
  e->t = res;
}

static void check_call(Expr *e) {
  Expr *c = e->a;
  if (c->k == E_IDENT) {
    const char *n = c->s;
    Var *v = lookup_var(n);
    if (v) die(c->pos, "%s is a variable of type %s, not a function", n, v->t->name);
    Global *g = hget(&globals, n);
    if (g && g->k == G_FN) {
      FnDecl *f = g->p;
      e->sub = CK_FUNC;
      e->fn = f;
      Type **ps = alloc(sizeof(Type *) * (size_t)(f->params.n + 1));
      for (int i = 0; i < f->params.n; i++) ps[i] = f->params.v[i].t;
      check_args(e, ps, f->params.n, fmt("%s()", n));
      e->t = fn_ret(f);
      return;
    }
    if (!g && (!strcmp(n, "int") || !strcmp(n, "float") || !strcmp(n, "byte") || !strcmp(n, "string") ||
               !strcmp(n, "bool"))) {
      if (e->args.n != 1) die(e->pos, "the conversion %s() takes exactly one argument", n);
      check_expr(e->args.v[0]);
      Type *from = e->args.v[0]->t;
      if (from == t_void || from->k == TY_TUPLE) not_value(e->args.v[0]);
      Type *to = NULL;
      if (!strcmp(n, "int") && (from == t_int || from == t_float || from == t_byte)) to = t_int;
      if (!strcmp(n, "float") && (from == t_int || from == t_float)) to = t_float;
      if (!strcmp(n, "byte") && (from == t_int || from == t_byte)) to = t_byte;
      if (!strcmp(n, "string") && (from == t_string || from == t_list(t_byte))) to = t_string;
      if (!strcmp(n, "bool") && from == t_bool) to = t_bool;
      if (!to) {
        if (from->k == TY_OPT) die(e->args.v[0]->pos, "%s is nullable (%s); check it against null first", describe(e->args.v[0]), from->name);
        if (!strcmp(n, "string")) die(e->pos, "cannot convert %s to string; use .string() to get text, or string(bytes) for a []byte", from->name);
        die(e->pos, "cannot convert %s to %s", from->name, n);
      }
      e->sub = CK_CONV;
      e->t = to;
      return;
    }
    if (g && g->k == G_TYPE) die(c->pos, "%s is a type; a struct value is written %s{field: value, ...}", n, n);
    if (g && g->k == G_CONST) die(c->pos, "%s is a constant, not a function", n);
    if (!strcmp(n, "assert")) unsupported(c->pos, "assert");
    Suggest s = {0, 99, n};
    for (int i = 0; i < fns.n; i++)
      if (!fns.v[i]->is_method) consider(&s, fns.v[i]->name);
    for (int i = scope.n - 1; i >= 0; i--) consider(&s, scope.v[i]->name);
    die(c->pos, "undefined function %s%s", n, hint(&s));
  }
  if (c->k == E_FIELD) {
    Expr *base = c->a;
    const char *name = c->s;
    if (pkg_named(base)) {
      check_pkg_call(e, base->s, name);
      return;
    }
    TypeDecl *td = type_named(base);
    if (td) {
      if (!td->is_enum) die(c->pos, "%s is a struct type; it has no case %s", td->name, name);
      int ci = find_case(td, name);
      if (ci < 0) unknown_case(c->pos, td, name);
      ECase *ec = &td->cases.v[ci];
      if (!ec->params.n) die(e->pos, "%s.%s carries no data; write it without parentheses", td->name, name);
      Type **ps = alloc(sizeof(Type *) * (size_t)ec->params.n);
      for (int i = 0; i < ec->params.n; i++) ps[i] = ec->params.v[i].t;
      check_args(e, ps, ec->params.n, fmt("%s.%s", td->name, name));
      e->sub = CK_ENUM;
      e->td = td;
      e->ecase = ci;
      e->t = td->t;
      return;
    }
    check_expr(base);
    Type *bt = base->t;
    if (bt == t_void || bt->k == TY_TUPLE) not_value(base);
    if (bt->k == TY_STRUCT || bt->k == TY_ENUM) {
      FnDecl *m = hget(&bt->td->methods, name);
      if (!m) {
        Suggest s = {0, 99, name};
        for (int i = 0; i < bt->td->methods.cap; i++) consider(&s, bt->td->methods.e[i].key);
        if (bt->k == TY_STRUCT && find_field(bt->td, name))
          die(c->pos, "%s is a field of %s, not a method", name, bt->name);
        die(c->pos, "%s has no method %s%s", bt->name, name, hint(&s));
      }
      if (m->recv_mut) {
        require_mutable(base, fmt("call the mut method %s() on", name));
        kill_mut(path_key(base));
      }
      Type **ps = alloc(sizeof(Type *) * (size_t)(m->params.n + 1));
      for (int i = 0; i < m->params.n; i++) ps[i] = m->params.v[i].t;
      check_args(e, ps, m->params.n, fmt("%s.%s()", bt->name, name));
      e->sub = CK_METHOD;
      e->fn = m;
      e->t = fn_ret(m);
      return;
    }
    check_builtin(e, base, name);
    return;
  }
  check_expr(c);
  die(e->pos, "this expression cannot be called; seed0 has no function values");
}

static void check_expr(Expr *e) {
  switch (e->k) {
  case E_INT:
    if (!e->neg && e->u > (uint64_t)INT64_MAX) die(e->pos, "integer literal is too large for seed0's 64-bit int");
    e->t = t_int;
    return;
  case E_FLOAT: e->t = t_float; return;
  case E_BYTE: e->t = t_byte; return;
  case E_STRING: e->t = t_string; return;
  case E_BOOL: e->t = t_bool; return;
  case E_NULL: e->t = t_null; return;
  case E_IDENT: {
    const char *n = e->s;
    if (!strcmp(n, "_")) die(e->pos, "_ can only be assigned to, not read");
    Var *v = lookup_var(n);
    if (v) {
      e->var = v;
      e->t = v->t;
      apply_narrowing(e);
      return;
    }
    Global *g = hget(&globals, n);
    if (g && g->k == G_CONST) {
      e->cd = g->p;
      check_const(e->cd);
      e->t = e->cd->t;
      return;
    }
    if (g && g->k == G_FN) die(e->pos, "%s is a function; seed0 has no function values, so it can only be called", n);
    if (g && g->k == G_TYPE) die(e->pos, "%s is a type, not a value", n);
    if (is_pkg(n)) die(e->pos, "%s is a package; use it as %s.name(...)", n, n);
    Suggest s = {0, 99, n};
    for (int i = scope.n - 1; i >= 0; i--) consider(&s, scope.v[i]->name);
    for (int i = 0; i < cdecls.n; i++) consider(&s, cdecls.v[i]->name);
    die(e->pos, "undefined: %s%s", n, hint(&s));
  }
  case E_FIELD: {
    TypeDecl *td = type_named(e->a);
    if (td) {
      if (!td->is_enum) die(e->pos, "%s is a struct type; it has no case %s", td->name, e->s);
      int ci = find_case(td, e->s);
      if (ci < 0) unknown_case(e->pos, td, e->s);
      if (td->cases.v[ci].params.n)
        die(e->pos, "%s.%s carries data; create it with %s.%s(...)", td->name, e->s, td->name, e->s);
      e->sub = FK_ENUMVAL;
      e->td = td;
      e->ecase = ci;
      e->t = td->t;
      return;
    }
    if (pkg_named(e->a)) die(e->pos, "%s.%s is a function; call it", e->a->s, e->s);
    check_expr(e->a);
    Type *bt = e->a->t;
    if (bt == t_void || bt->k == TY_TUPLE) not_value(e->a);
    if (bt->k == TY_OPT)
      die(e->a->pos, "%s is nullable (%s); check it against null before reading .%s", describe(e->a), bt->name, e->s);
    if (bt->k != TY_STRUCT) {
      die(e->pos, "%s has no field %s%s", bt->name, e->s, "");
    }
    Param *f = find_field(bt->td, e->s);
    if (!f) {
      if (hget(&bt->td->methods, e->s)) die(e->pos, "%s is a method; call it with %s()", e->s, e->s);
      unknown_field(e->pos, bt->td, e->s);
    }
    e->sub = FK_FIELD;
    e->t = f->t;
    apply_narrowing(e);
    return;
  }
  case E_INDEX: {
    check_expr(e->a);
    Type *bt = e->a->t;
    if (bt->k == TY_LIST) {
      e->sub = IK_LIST;
      e->b = check_value(e->b, t_int, "as a list index");
      e->t = bt->elem;
    } else if (bt->k == TY_STRING) {
      e->sub = IK_STRING;
      e->b = check_value(e->b, t_int, "as a string index");
      e->t = t_byte;
    } else if (bt->k == TY_MAP) {
      e->sub = IK_MAP;
      e->b = check_value(e->b, bt->key, "as a map key");
      e->t = t_opt(bt->elem);
    } else if (bt->k == TY_OPT) {
      die(e->a->pos, "%s is nullable (%s); check it against null before indexing", describe(e->a), bt->name);
    } else {
      if (bt == t_void || bt->k == TY_TUPLE) not_value(e->a);
      die(e->pos, "cannot index a value of type %s", bt->name);
    }
    return;
  }
  case E_CALL: check_call(e); return;
  case E_UNARY:
    check_expr(e->a);
    if (e->a->t == t_void || e->a->t->k == TY_TUPLE) not_value(e->a);
    if (e->op == P_MINUS) {
      if (e->a->t != t_int && e->a->t != t_float) die(e->pos, "unary - needs an int or float, not %s", e->a->t->name);
    } else if (e->a->t != t_bool) {
      die(e->pos, "! needs a bool, not %s", e->a->t->name);
    }
    e->t = e->a->t;
    return;
  case E_BINARY: {
    int op = e->op;
    const char *ops = tok_names[op];
    if (op == P_ANDAND || op == P_OROR) {
      e->a = check_value(e->a, t_bool, fmt("with %s", ops));
      PUSH(narrow_marks, narrows.n);
      narrow_on(e->a, op == P_ANDAND);
      e->b = check_value(e->b, t_bool, fmt("with %s", ops));
      narrows.n = narrow_marks.v[--narrow_marks.n];
      e->t = t_bool;
      return;
    }
    check_expr(e->a);
    check_expr(e->b);
    Expr *a = e->a, *b = e->b;
    if (a->t == t_void || a->t->k == TY_TUPLE) not_value(a);
    if (b->t == t_void || b->t->k == TY_TUPLE) not_value(b);
    if (op == P_EQ || op == P_NE) {
      if (a->k == E_NULL || b->k == E_NULL) {
        Expr *x = a->k == E_NULL ? b : a, *n = a->k == E_NULL ? a : b;
        if (x->k == E_NULL) die(e->pos, "comparing null with null");
        if (x->narrowed) {
          x->narrowed = false;
          x->t = x->orig;
        }
        if (x->t->k != TY_OPT && x->t != t_error)
          die(x->pos, "%s has type %s, which is never null", describe(x), x->t->name);
        n->t = x->t;
        e->t = t_bool;
        return;
      }
      if (a->t != b->t) {
        if ((a->t->k == TY_OPT && a->t->elem == b->t) || (b->t->k == TY_OPT && b->t->elem == a->t))
          die(e->pos, "a nullable value can only be compared with null; check it against null first");
        die(e->pos, "mismatched types %s and %s for %s", a->t->name, b->t->name, ops);
      }
      if (a->t->k == TY_OPT) die(e->pos, "a nullable value (%s) can only be compared with null; check it first", a->t->name);
      e->t = t_bool;
      return;
    }
    if (a->t != b->t && a->t->k == TY_OPT && a->t->elem == b->t)
      die(a->pos, "%s is nullable (%s); check it against null first", describe(a), a->t->name);
    if (a->t != b->t && b->t->k == TY_OPT && b->t->elem == a->t)
      die(b->pos, "%s is nullable (%s); check it against null first", describe(b), b->t->name);
    if (a->t != b->t)
      die(e->pos, "mismatched types %s and %s for %s (there are no implicit conversions; use float(x), int(x), ...)",
          a->t->name, b->t->name, ops);
    Type *t = a->t;
    bool ok;
    switch (op) {
    case P_PLUS: ok = t == t_int || t == t_float || t == t_string; break;
    case P_MINUS: case P_STAR: case P_SLASH: ok = t == t_int || t == t_float; break;
    case P_PERCENT: ok = t == t_int; break;
    case P_AMP: case P_PIPE: case P_CARET: case P_SHL: case P_SHR: ok = t == t_int || t == t_byte; break;
    default: ok = t == t_int || t == t_float || t == t_string || t == t_byte; break;
    }
    if (!ok) {
      if (t->k == TY_OPT) die(e->pos, "%s is nullable (%s); check it against null first", describe(a), t->name);
      die(e->pos, "operator %s is not defined on %s", ops, t->name);
    }
    e->t = binprec(op) == 3 ? t_bool : t;
    return;
  }
  case E_TRY: {
    check_expr(e->a);
    Expr *c = e->a;
    Type *t = c->t;
    bool ok = c->k == E_CALL && (t == t_error || (t->k == TY_TUPLE && t->parts[t->np - 1] == t_error));
    if (!ok) die(e->pos, "try needs a call that returns an error as its last result; %s returns %s", describe(c), t->name);
    if (!cur_fn) die(e->pos, "try cannot be used in a constant");
    if (!cur_fn->rtypes.n || cur_fn->rtypes.v[cur_fn->rtypes.n - 1] != t_error)
      die(e->pos, "try can only be used in a function whose last result is error");
    if (t == t_error) e->t = t_void;
    else if (t->np == 2) e->t = t->parts[0];
    else e->t = t_tuple(t->parts, t->np - 1);
    return;
  }
  case E_LIST: {
    Type *el = resolve(e->te);
    for (int i = 0; i < e->args.n; i++) e->args.v[i] = check_value(e->args.v[i], el, "as a list element");
    e->t = t_list(el);
    return;
  }
  case E_MAP: {
    TypeExpr mte = {TE_MAP, e->pos, NULL, e->te, e->te2};
    Type *mt = resolve(&mte);
    for (int i = 0; i < e->keys.n; i++) {
      e->keys.v[i] = check_value(e->keys.v[i], mt->key, "as a map key");
      e->args.v[i] = check_value(e->args.v[i], mt->elem, "as a map value");
    }
    e->t = mt;
    return;
  }
  case E_STRUCT: {
    Global *g = hget(&globals, e->s);
    if (!g || g->k != G_TYPE) {
      Suggest s = {0, 99, e->s};
      for (int i = 0; i < tdecls.n; i++) consider(&s, tdecls.v[i]->name);
      die(e->pos, "unknown struct type %s%s", e->s, hint(&s));
    }
    TypeDecl *td = g->p;
    if (td->is_enum) die(e->pos, "%s is an enum; create a value with %s.Case", td->name, td->name);
    for (int i = 0; i < e->names.n; i++) {
      Param *f = find_field(td, e->names.v[i]);
      if (!f) unknown_field(e->npos.v[i], td, e->names.v[i]);
      for (int j = 0; j < i; j++)
        if (!strcmp(e->names.v[j], e->names.v[i])) die(e->npos.v[i], "field %s is set twice", e->names.v[i]);
      e->args.v[i] = check_value(e->args.v[i], f->t, fmt("for field %s", f->name));
    }
    for (int i = 0; i < td->fields.n; i++) {
      bool found = false;
      for (int j = 0; j < e->names.n; j++)
        if (!strcmp(e->names.v[j], td->fields.v[i].name)) found = true;
      if (!found)
        die(e->pos, "missing field %s in %s literal; a struct literal must set every field", td->fields.v[i].name, td->name);
    }
    e->td = td;
    e->t = td->t;
    return;
  }
  case E_WRAP: return;
  }
}

/* ---- statements ---- */

static bool is_exit_call(Expr *e) { return e->k == E_CALL && e->sub == CK_PKG && e->bid == F_EXIT; }

static bool has_break(Block *b) {
  for (int i = 0; i < b->n; i++) {
    Stmt *s = b->v[i];
    if (s->k == S_BREAK) return true;
    if (s->k == S_IF && (has_break(&s->body) || has_break(&s->els))) return true;
    if (s->k == S_SWITCH)
      for (int j = 0; j < s->cases.n; j++)
        if (has_break(&s->cases.v[j].body)) return true;
  }
  return false;
}

static bool assignable(Type *from, Type *to) { return from == to || (to->k == TY_OPT && to->elem == from); }

static bool check_stmt(Stmt *s);

static void check_assign(Stmt *s) {
  int n = s->targets.n;
  if (n == 1) {
    Target *tg = &s->targets.v[0];
    if (tg->te) {
      tg->t = resolve(tg->te);
      s->e = check_value(s->e, tg->t, fmt("to initialize %s", tg->name));
      tg->var = declare(tg->name, tg->t, VK_LOCAL, tg->pos);
    } else if (tg->discard) {
      check_expr(s->e);
      if (s->e->t == t_void) not_value(s->e);
      if (s->e->t->k == TY_TUPLE)
        die(s->pos, "%s returns %d values; write one target per value, using _ for the ones you discard", describe(s->e),
            s->e->t->np);
    } else {
      check_lvalue(tg->lv);
      tg->t = tg->lv->t;
      s->e = check_value(s->e, tg->t, fmt("in the assignment to %s", describe(tg->lv)));
      kill_assign(path_key(tg->lv));
    }
    return;
  }
  check_expr(s->e);
  Type *rt = s->e->t;
  if (rt->k != TY_TUPLE || rt->np != n) {
    if (rt->k == TY_TUPLE) die(s->pos, "%d targets but %s returns %d values", n, describe(s->e), rt->np);
    die(s->pos, "%d targets but the right side is a single value of type %s", n, rt->name);
  }
  bool errslot = s->e->k == E_CALL && rt->parts[n - 1] == t_error && s->targets.v[n - 1].discard;
  s->discard_err = errslot;
  for (int i = 0; i < n; i++) {
    Target *tg = &s->targets.v[i];
    Type *st = rt->parts[i];
    if (tg->discard) continue;
    if (tg->te) {
      tg->t = resolve(tg->te);
    } else {
      check_lvalue(tg->lv);
      tg->t = tg->lv->t;
    }
    if (!assignable(st, tg->t))
      die(tg->pos, "value %d has type %s, which cannot be assigned to %s (type %s)", i + 1, st->name,
          tg->te ? tg->name : describe(tg->lv), tg->t->name);
    if (errslot && i < n - 1 && tg->t->k != TY_OPT && tg->t != t_error)
      die(tg->pos,
          "the error is discarded with _, so %s may be null and must be declared as %s? (and checked before use)",
          tg->te ? tg->name : describe(tg->lv), tg->t->name);
  }
  for (int i = 0; i < n; i++) {
    Target *tg = &s->targets.v[i];
    if (tg->discard) continue;
    if (tg->te) {
      for (int j = 0; j < i; j++)
        if (s->targets.v[j].te && !strcmp(s->targets.v[j].name, tg->name)) die(tg->pos, "%s is declared twice", tg->name);
      tg->var = declare(tg->name, tg->t, VK_LOCAL, tg->pos);
    } else {
      kill_assign(path_key(tg->lv));
    }
  }
}

static bool check_switch(Stmt *s) {
  check_expr(s->e);
  Type *t = s->e->t;
  if (t == t_void || t->k == TY_TUPLE) not_value(s->e);
  bool all_term = true, has_default = false;
  if (t->k == TY_ENUM) {
    TypeDecl *td = t->td;
    bool *seen = alloc((size_t)td->cases.n + 1);
    for (int i = 0; i < s->cases.n; i++) {
      Case *c = &s->cases.v[i];
      if (c->isdefault)
        die(c->pos, "a switch over the enum %s cannot have default; list every case so new cases are caught", td->name);
      push_scope();
      for (int j = 0; j < c->labels.n; j++) {
        Label *l = &c->labels.v[j];
        if (!l->pattern) die(l->pos, "expected a case of %s, like case %s.%s:", td->name, td->name, td->cases.v[0].name);
        if (strcmp(l->tname, td->name)) die(l->pos, "%s is not a case of %s", fmt("%s.%s", l->tname, l->cname), td->name);
        int ci = find_case(td, l->cname);
        if (ci < 0) unknown_case(l->pos, td, l->cname);
        if (seen[ci]) die(l->pos, "case %s.%s is listed twice", td->name, l->cname);
        seen[ci] = true;
        l->ecase = ci;
        ECase *ec = &td->cases.v[ci];
        if (l->parens) {
          if (c->labels.n > 1 && l->params.n)
            die(l->pos, "a case with bindings must be the only label in its case");
          if (l->params.n != ec->params.n)
            die(l->pos, "%s.%s carries %d value%s but the pattern binds %d", td->name, ec->name, ec->params.n,
                ec->params.n == 1 ? "" : "s", l->params.n);
          l->vars = alloc(sizeof(Var *) * (size_t)(l->params.n + 1));
          for (int k = 0; k < l->params.n; k++) {
            Param *p = &l->params.v[k];
            p->t = resolve(p->te);
            if (p->t != ec->params.v[k].t)
              die(p->pos, "%s.%s field %d has type %s, not %s", td->name, ec->name, k + 1, ec->params.v[k].t->name,
                  p->t->name);
            l->vars[k] = declare(p->name, p->t, VK_LOCAL, p->pos);
          }
        }
      }
      if (!check_block(&c->body)) all_term = false;
      pop_scope();
    }
    for (int i = 0; i < td->cases.n; i++)
      if (!seen[i]) die(s->pos, "switch over %s does not handle case %s.%s", td->name, td->name, td->cases.v[i].name);
    return all_term && s->cases.n > 0;
  }
  if (t != t_int && t != t_byte && t != t_string && t != t_bool) {
    if (t->k == TY_OPT) die(s->e->pos, "%s is nullable (%s); check it against null first", describe(s->e), t->name);
    die(s->e->pos, "cannot switch on %s; switch works on enums, int, byte, string and bool", t->name);
  }
  for (int i = 0; i < s->cases.n; i++) {
    Case *c = &s->cases.v[i];
    if (c->isdefault) {
      if (has_default) die(c->pos, "switch has two default cases");
      has_default = true;
    }
    for (int j = 0; j < c->labels.n; j++) {
      Label *l = &c->labels.v[j];
      if (l->parens) die(l->pos, "%s is not an enum, so case patterns with bindings cannot be used", t->name);
      l->e = check_value(l->e, t, "as a case value");
    }
    push_scope();
    if (!check_block(&c->body)) all_term = false;
    pop_scope();
  }
  if (!has_default) die(s->pos, "a switch over %s must end with a default case", t->name);
  return all_term;
}

static bool check_stmt(Stmt *s) {
  switch (s->k) {
  case S_EXPR: {
    check_expr(s->e);
    Expr *e = s->e;
    if (e->k != E_CALL && e->k != E_TRY) die(e->pos, "this expression is not used; only calls can be statements");
    if (e->k == E_CALL && (e->t == t_error || (e->t->k == TY_TUPLE && e->t->parts[e->t->np - 1] == t_error)))
      die(e->pos,
          "the error returned by %s is not handled; use try, assign it (error err = ...), or discard it with _ = ...",
          describe(e));
    return is_exit_call(e);
  }
  case S_ASSIGN: check_assign(s); return false;
  case S_INCDEC:
    check_lvalue(s->e);
    if (s->e->t != t_int && s->e->t != t_float) die(s->pos, "%s needs an int or float, not %s", s->inc ? "++" : "--", s->e->t->name);
    kill_assign(path_key(s->e));
    return false;
  case S_CONST: {
    Type *t = resolve(s->te);
    s->e = check_value(s->e, t, fmt("to initialize %s", s->name));
    s->var = declare(s->name, t, VK_LCONST, s->pos);
    return false;
  }
  case S_IF: {
    s->cond = check_value(s->cond, t_bool, "as the if condition");
    push_scope();
    narrow_on(s->cond, true);
    bool then_term = check_block(&s->body);
    pop_scope();
    bool else_term = false;
    if (s->has_else) {
      push_scope();
      narrow_on(s->cond, false);
      else_term = check_block(&s->els);
      pop_scope();
    }
    if (then_term && !else_term) narrow_on(s->cond, false);
    if (else_term && !then_term) narrow_on(s->cond, true);
    return then_term && else_term;
  }
  case S_FOR: {
    push_scope();
    if (s->init) check_stmt(s->init);
    scan_block_kills(&s->body);
    scan_stmt_kills(s->post);
    if (s->cond) scan_expr_kills(s->cond);
    if (s->cond) s->cond = check_value(s->cond, t_bool, "as the loop condition");
    push_scope();
    if (s->cond) narrow_on(s->cond, true);
    loop_depth++;
    check_block(&s->body);
    loop_depth--;
    pop_scope();
    if (s->post) {
      if (s->post->k == S_ASSIGN)
        for (int i = 0; i < s->post->targets.n; i++)
          if (s->post->targets.v[i].te) die(s->post->pos, "the loop post statement cannot declare a variable");
      check_stmt(s->post);
    }
    pop_scope();
    return !s->cond && !has_break(&s->body);
  }
  case S_SWITCH: return check_switch(s);
  case S_RETURN: {
    FnDecl *f = cur_fn;
    int n = f->rtypes.n;
    bool has_err = n > 0 && f->rtypes.v[n - 1] == t_error;
    if (s->exprs.n == 0) {
      if (n) die(s->pos, "missing return value%s: %s returns %s", n > 1 ? "s" : "", f->name, f->ret->name);
      return true;
    }
    if (n == 0) die(s->pos, "%s does not return a value", f->name);
    if (s->exprs.n == 1 && n > 1) {
      check_expr(s->exprs.v[0]);
      if (s->exprs.v[0]->t != f->ret)
        die(s->pos, "%s returns %d values %s, but this returns %s", f->name, n, f->ret->name, s->exprs.v[0]->t->name);
      return true;
    }
    if (s->exprs.n != n) die(s->pos, "%s returns %d value%s but this returns %d", f->name, n, n == 1 ? "" : "s", s->exprs.n);
    for (int i = 0; i < n; i++) {
      Expr *e = s->exprs.v[i];
      check_expr(e);
      if (e->k == E_NULL && n > 1 && has_err && f->rtypes.v[i]->k != TY_OPT && f->rtypes.v[i] != t_error) {
        e->t = f->rtypes.v[i]; /* null in a non-nullable slot next to an error: a zero value */
        continue;
      }
      s->exprs.v[i] = coerce(e, f->rtypes.v[i], n > 1 ? fmt("as return value %d", i + 1) : "as the return value");
    }
    return true;
  }
  case S_BREAK:
  case S_CONTINUE:
    if (!loop_depth) die(s->pos, "%s outside of a loop", s->k == S_BREAK ? "break" : "continue");
    return true;
  }
  return false;
}

static bool check_block(Block *b) {
  bool term = false;
  push_scope();
  for (int i = 0; i < b->n; i++)
    if (check_stmt(b->v[i])) term = true;
  pop_scope();
  return term;
}

static void check_const(ConstDecl *c) {
  if (c->state == 2) return;
  if (c->state == 1) die(c->pos, "constant %s depends on itself", c->name);
  c->state = 1;
  c->t = resolve(c->te);
  FnDecl *saved = cur_fn;
  cur_fn = NULL;
  c->e = check_value(c->e, c->t, fmt("to initialize %s", c->name));
  cur_fn = saved;
  c->state = 2;
}

static void check_program(void) {
  /* types */
  for (int i = 0; i < tdecls.n; i++) {
    TypeDecl *td = tdecls.v[i];
    td->t = mktype(td->is_enum ? TY_ENUM : TY_STRUCT, fmt("%c%zu%s", td->is_enum ? 'E' : 'S', strlen(td->name), td->name),
                   td->name, NULL);
    td->t->td = td;
  }
  for (int i = 0; i < tdecls.n; i++) {
    TypeDecl *td = tdecls.v[i];
    for (int j = 0; j < td->fields.n; j++) {
      Param *p = &td->fields.v[j];
      p->t = resolve(p->te);
      for (int k = 0; k < j; k++)
        if (!strcmp(td->fields.v[k].name, p->name)) die(p->pos, "field %s is declared twice", p->name);
    }
    for (int j = 0; j < td->cases.n; j++) {
      ECase *c = &td->cases.v[j];
      for (int k = 0; k < j; k++)
        if (!strcmp(td->cases.v[k].name, c->name)) die(c->pos, "case %s is declared twice", c->name);
      for (int k = 0; k < c->params.n; k++) c->params.v[k].t = resolve(c->params.v[k].te);
    }
  }
  /* signatures */
  for (int i = 0; i < fns.n; i++) {
    FnDecl *f = fns.v[i];
    for (int j = 0; j < f->params.n; j++) f->params.v[j].t = resolve(f->params.v[j].te);
    for (int j = 0; j < f->results.n; j++) PUSH(f->rtypes, resolve(f->results.v[j]));
    f->ret = f->rtypes.n == 0 ? t_void : f->rtypes.n == 1 ? f->rtypes.v[0] : t_tuple(f->rtypes.v, f->rtypes.n);
    if (f->is_method) {
      Global *g = hget(&globals, f->recv_tname);
      if (!g || g->k != G_TYPE) {
        TypeExpr te = {TE_NAME, f->recv_pos, f->recv_tname, 0, 0};
        resolve(&te);
        die(f->recv_pos, "methods can only be declared on struct and enum types of this package, not %s", f->recv_tname);
      }
      f->recv = g->p;
      if (hget(&f->recv->methods, f->name)) die(f->pos, "method %s.%s is declared twice", f->recv->name, f->name);
      hput(&f->recv->methods, f->name, f);
      if (f->recv_mut) hput(&mut_names, f->name, (void *)1);
      f->cname = fmt("sd_m_%s__%s", f->recv->name, f->name);
    } else {
      f->cname = fmt("sd_f_%s", f->name);
    }
  }
  Global *mg = hget(&globals, "main");
  if (!mg || mg->k != G_FN) die((Pos){fns.n ? fns.v[0]->pos.file : "<input>", 1, 1}, "missing fn main([]string args) error");
  FnDecl *mf = mg->p;
  if (mf->params.n != 1 || mf->params.v[0].t != t_list(t_string) || mf->ret != t_error)
    die(mf->pos, "main must be declared as fn main([]string args) error");
  /* constants */
  for (int i = 0; i < cdecls.n; i++) check_const(cdecls.v[i]);
  /* bodies */
  for (int i = 0; i < fns.n; i++) {
    FnDecl *f = fns.v[i];
    cur_fn = f;
    scope.n = 0;
    narrows.n = 0;
    push_scope();
    if (f->is_method) declare(f->recv_name, f->recv->t, f->recv_mut ? VK_MUTRECV : VK_RECV, f->recv_pos);
    for (int j = 0; j < f->params.n; j++) declare(f->params.v[j].name, f->params.v[j].t, VK_PARAM, f->params.v[j].pos);
    bool term = check_block(&f->body);
    if (f->rtypes.n && !term) {
      Pos end = f->pos;
      die(end, "missing return at the end of %s", f->name);
    }
    pop_scope();
  }
  cur_fn = NULL;
}

/* ======================================================================
 * C generation
 * ====================================================================== */

/* Helper functions generated per type on demand. */
enum { H_EQ = 1, H_EQP = 2, H_RT = 4, H_RTP = 8, H_RL = 16 };

typedef struct {
  Type *t;
  unsigned h;
} HelperReq;

static VEC(HelperReq) helper_queue;
static Buf out_protos, out_helpers;
static Buf *out;
static int indent;
static int ntmp;

static const char *tmp(void) { return fmt("_t%d", ++ntmp); }

static void emit(const char *f, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *f, ...) {
  for (int i = 0; i < indent; i++) bput(out, "  ");
  va_list ap;
  va_start(ap, f);
  char tmpb[1024];
  int n = vsnprintf(tmpb, sizeof tmpb, f, ap);
  va_end(ap);
  if (n < (int)sizeof tmpb) {
    bputn(out, tmpb, (size_t)n);
  } else {
    char *p = malloc((size_t)n + 1);
    va_start(ap, f);
    vsnprintf(p, (size_t)n + 1, f, ap);
    va_end(ap);
    bputn(out, p, (size_t)n);
    free(p);
  }
  bput(out, "\n");
}

static const char *ctype(Type *t) {
  switch (t->k) {
  case TY_VOID: return "void";
  case TY_INT: return "int64_t";
  case TY_FLOAT: return "double";
  case TY_BOOL: return "bool";
  case TY_BYTE: return "uint8_t";
  case TY_STRING: return "sd_str";
  case TY_ERROR: return "sd_err*";
  case TY_NULL: return "void*";
  case TY_LIST: return "sd_list*";
  case TY_MAP: return "sd_map*";
  case TY_STRUCT: return fmt("sd_S_%s", t->td->name);
  case TY_ENUM: return fmt("sd_E_%s*", t->td->name);
  case TY_TUPLE: return fmt("sd_T_%s", t->mangle);
  case TY_OPT:
    switch (t->elem->k) {
    case TY_INT: return "sd_O_i";
    case TY_FLOAT: return "sd_O_f";
    case TY_BOOL: return "sd_O_b";
    case TY_BYTE: return "sd_O_y";
    case TY_STRUCT: return fmt("sd_B_%s*", t->elem->td->name);
    default: return ctype(t->elem);
    }
  }
  return "?";
}

static bool needs_rt(Type *t) {
  switch (t->k) {
  case TY_LIST:
  case TY_MAP: return true;
  case TY_OPT: return t->elem->k == TY_LIST || t->elem->k == TY_MAP || t->elem->k == TY_STRUCT;
  case TY_STRUCT: {
    if (t->rt == 0 || t->rt == 1) return t->rt;
    if (t->rt == 2) return false;
    t->rt = 2;
    bool r = false;
    for (int i = 0; i < t->td->fields.n; i++)
      if (needs_rt(t->td->fields.v[i].t)) r = true;
    t->rt = r;
    return r;
  }
  default: return false;
  }
}

static const char *helper(Type *t, unsigned h, const char *prefix) {
  if (!(t->helpers & h)) {
    t->helpers |= h;
    PUSH(helper_queue, ((HelperReq){t, h}));
  }
  return fmt("%s%s", prefix, t->mangle);
}

static Type *base_of(Type *t) { return t->k == TY_OPT ? t->elem : t; }

static const char *rt_fn(Type *t) {
  Type *b = base_of(t);
  if (b->k == TY_LIST) return "sd_rt_list";
  if (b->k == TY_MAP) return "sd_rt_map";
  return helper(t, H_RT, "sd_rt_");
}

static const char *rl_fn(Type *t) {
  Type *b = base_of(t);
  if (b->k == TY_LIST) return "sd_rl_list";
  if (b->k == TY_MAP) return "sd_rl_map";
  return helper(t, H_RL, "sd_rl_");
}

static const char *rtp_fn(Type *t) { return needs_rt(t) ? helper(t, H_RTP, "sd_rtp_") : "NULL"; }

static const char *eqp_fn(Type *t) { return helper(t, H_EQP, "sd_eqp_"); }

static const char *eq_expr(Type *t, const char *a, const char *b) {
  switch (t->k) {
  case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_BYTE: return fmt("((%s) == (%s))", a, b);
  case TY_STRING: return fmt("sd_str_eq(%s, %s)", a, b);
  case TY_ERROR: return fmt("sd_err_eq(%s, %s)", a, b);
  default: return fmt("%s(%s, %s)", helper(t, H_EQ, "sd_eq_"), a, b);
  }
}

static const char *null_of(Type *t) {
  if (t->k == TY_OPT) {
    switch (t->elem->k) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_BYTE: return fmt("((%s){0})", ctype(t));
    case TY_STRING: return "((sd_str){0})";
    default: return "NULL";
    }
  }
  return "NULL";
}

static const char *zero_of(Type *t) {
  switch (t->k) {
  case TY_INT: case TY_BYTE: return "0";
  case TY_FLOAT: return "0.0";
  case TY_BOOL: return "false";
  case TY_STRING: return "((sd_str){0})";
  case TY_STRUCT: case TY_TUPLE: return fmt("((%s){0})", ctype(t));
  case TY_OPT: return null_of(t);
  default: return "NULL";
  }
}

static const char *wrap_val(Type *opt, const char *v) {
  switch (opt->elem->k) {
  case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_BYTE: return fmt("((%s){true, %s})", ctype(opt), v);
  case TY_STRUCT: return fmt("sd_bx_%s(%s)", opt->elem->td->name, v);
  default: return v;
  }
}

static const char *unwrap_val(Type *opt, const char *v) {
  switch (opt->elem->k) {
  case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_BYTE: return fmt("(%s).v", v);
  case TY_STRUCT: return fmt("(%s)->v", v);
  default: return v;
  }
}

static const char *unwrap_place(Type *opt, const char *p) {
  switch (opt->elem->k) {
  case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_BYTE: return fmt("(&(%s)->v)", p);
  case TY_STRUCT: return fmt("(&sd_bm_%s(%s)->v)", opt->elem->td->name, p);
  default: return p;
  }
}

static const char *is_null(Type *t, const char *v) {
  if (t->k == TY_OPT) {
    switch (t->elem->k) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_BYTE: return fmt("(!(%s).has)", v);
    case TY_STRING: return fmt("((%s).p == NULL)", v);
    default: break;
    }
  }
  return fmt("((%s) == NULL)", v);
}

static const char *c_string(const char *s, int64_t n) {
  Buf b = {0};
  bput(&b, "\"");
  for (int64_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') bprintf(&b, "\\%c", c);
    else if (c == '?') bput(&b, "\\?");
    else if (c >= 32 && c < 127) bputn(&b, (const char *)&c, 1);
    else bprintf(&b, "\\%03o", c);
  }
  bput(&b, "\"");
  const char *r = sdup(b.s);
  free(b.s);
  return r;
}

static const char *key_suffix(Type *k) { return k == t_string ? "s" : "i"; }
static const char *key_arg(Type *k, const char *v) { return k == t_string ? v : fmt("(int64_t)(%s)", v); }

static bool is_mut_builtin(int bid) {
  return bid == B_PUSH || bid == B_POP || bid == B_INSERT || bid == B_REMOVE || bid == B_REVERSE || bid == B_SORT ||
         bid == B_DELETE;
}

static bool has_effects(Expr *e) {
  if (!e) return false;
  if (e->k == E_TRY) return true;
  if (e->k == E_CALL && (e->sub == CK_FUNC || e->sub == CK_METHOD || e->sub == CK_PKG)) return true;
  if (e->k == E_CALL && e->sub == CK_BUILTIN && is_mut_builtin(e->bid)) return true;
  if (e->k == E_CALL && e->sub == CK_BUILTIN && e->a->a && has_effects(e->a->a)) return true;
  if (e->k != E_CALL && (has_effects(e->a) || has_effects(e->b))) return true;
  for (int i = 0; i < e->args.n; i++)
    if (has_effects(e->args.v[i])) return true;
  for (int i = 0; i < e->keys.n; i++)
    if (has_effects(e->keys.v[i])) return true;
  return false;
}

static const char *gexpr(Expr *e);
static const char *gowned(Expr *e);
static const char *gplace(Expr *e);

static bool is_borrowed(Expr *e) {
  switch (e->k) {
  case E_IDENT: return true;
  case E_FIELD: return e->sub == FK_FIELD;
  case E_INDEX: return e->sub == IK_LIST || (e->sub == IK_MAP && e->t->k == TY_OPT && e->t->elem->k != TY_STRUCT);
  default: return false;
  }
}

/* A value that will be stored somewhere: retain it if it is still held elsewhere. */
static const char *gowned(Expr *e) {
  if (!needs_rt(e->t)) return gexpr(e);
  if (e->k == E_WRAP) return wrap_val(e->t, gowned(e->a));
  if (is_borrowed(e)) return fmt("%s(%s)", rt_fn(e->t), gexpr(e));
  return gexpr(e);
}

/* Generates expressions in order. When more than one has side effects (or
 * force is set), they are evaluated into temporaries first, written to pre. */
static const char **gen_list(Expr **es, int n, bool owned, bool force, Buf *pre) {
  const char **r = alloc(sizeof(char *) * (size_t)(n + 1));
  int eff = 0;
  for (int i = 0; i < n; i++)
    if (has_effects(es[i])) eff++;
  bool hoist = force || eff >= 2;
  for (int i = 0; i < n; i++) {
    const char *c = owned ? gowned(es[i]) : gexpr(es[i]);
    if (hoist) {
      const char *t = tmp();
      bprintf(pre, "%s %s = %s; ", ctype(es[i]->t), t, c);
      r[i] = t;
    } else {
      r[i] = c;
    }
  }
  return r;
}

static const char *with_pre(Buf *pre, const char *e) {
  if (!pre->len) return e;
  const char *r = fmt("({ %s%s; })", pre->s, e);
  free(pre->s);
  *pre = (Buf){0};
  return r;
}

static const char *join(const char **xs, int n, const char *first) {
  Buf b = {0};
  bput(&b, "");
  if (first) bput(&b, first);
  for (int i = 0; i < n; i++) {
    if (i || first) bput(&b, ", ");
    bput(&b, xs[i]);
  }
  const char *r = sdup(b.s);
  free(b.s);
  return r;
}

static const char *errret(const char *err) {
  Type *r = cur_fn->ret;
  if (r == t_error) return err;
  return fmt("(%s){.r%d = %s}", ctype(r), r->np - 1, err);
}

static const char *var_value(Expr *e) {
  if (e->cd) return fmt("sd_c_%s()", e->cd->name);
  if (e->var->k == VK_MUTRECV) return fmt("(*l_%s)", e->var->name);
  return fmt("l_%s", e->var->name);
}

static const char *gplace(Expr *e) {
  const char *p;
  switch (e->k) {
  case E_IDENT:
    p = e->var->k == VK_MUTRECV ? fmt("l_%s", e->var->name) : fmt("(&l_%s)", e->var->name);
    break;
  case E_FIELD: p = fmt("(&(%s)->f_%s)", gplace(e->a), e->s); break;
  case E_INDEX: {
    Type *el = e->t->k == TY_OPT && e->narrowed ? e->orig : e->t;
    if (e->narrowed) el = e->orig;
    const char *ct = ctype(el);
    if (has_effects(e->b)) {
      const char *i = tmp();
      p = fmt("({ int64_t %s = %s; (%s*)sd_list_at_mut(%s, %s, sizeof(%s), %s); })", i, gexpr(e->b), ct, gplace(e->a), i,
              ct, rtp_fn(el));
    } else {
      p = fmt("((%s*)sd_list_at_mut(%s, %s, sizeof(%s), %s))", ct, gplace(e->a), gexpr(e->b), ct, rtp_fn(el));
    }
    break;
  }
  default:
    die(e->pos, "internal error: not a place");
  }
  return e->narrowed ? unwrap_place(e->orig, p) : p;
}

/* Converts a pointer to an element (or NULL) into a T?. */
static const char *opt_from_ptr(Type *opt, const char *p, bool borrowed) {
  Type *el = opt->elem;
  const char *v = fmt("(*%s)", p);
  if (el->k == TY_STRUCT && borrowed && needs_rt(el)) v = fmt("%s(%s)", rt_fn(el), v);
  return fmt("(%s ? %s : %s)", p, wrap_val(opt, v), null_of(opt));
}

static const char *gtuple_out(Type *tt, const char *vtype, const char *call_fmt, const char *arg) {
  const char *v = tmp(), *er = tmp();
  return fmt("({ %s %s; sd_err *%s = %s; (%s){%s, %s}; })", vtype, v, er, fmt(call_fmt, arg, v), ctype(tt), v, er);
}

static const char *gbuiltin(Expr *e) {
  Expr *r = e->a->a;
  Type *rt = r->t;
  Expr **a = e->args.v;
  Buf pre = {0};
  switch (e->bid) {
  case B_LEN:
    if (rt == t_string) return fmt("(%s).len", gexpr(r));
    return fmt("(%s)->len", gexpr(r));
  case B_STRING:
    switch (rt->k) {
    case TY_INT: return fmt("sd_int_str(%s)", gexpr(r));
    case TY_FLOAT: return fmt("sd_float_str(%s)", gexpr(r));
    case TY_BOOL: return fmt("sd_bool_str(%s)", gexpr(r));
    case TY_BYTE: return fmt("sd_int_str((int64_t)(%s))", gexpr(r));
    default: return gexpr(r);
    }
  case B_TOINT: return gtuple_out(e->t, "int64_t", "sd_str_toint(%s, &%s)", gexpr(r));
  case B_TOFLOAT: return gtuple_out(e->t, "double", "sd_str_tofloat(%s, &%s)", gexpr(r));
  case B_SSLICE: case B_SCONTAINS: case B_SINDEX: case B_SSTARTS: case B_SENDS: case B_SPLIT: case B_TRIM:
  case B_REPLACE: case B_UPPER: case B_LOWER: case B_BYTES: case B_JOIN: case B_MESSAGE: {
    const char *fn = NULL;
    switch (e->bid) {
    case B_SSLICE: fn = "sd_str_slice"; break;
    case B_SCONTAINS: fn = "sd_str_contains"; break;
    case B_SINDEX: fn = "sd_str_index"; break;
    case B_SSTARTS: fn = "sd_str_starts"; break;
    case B_SENDS: fn = "sd_str_ends"; break;
    case B_SPLIT: fn = "sd_str_split"; break;
    case B_TRIM: fn = "sd_str_trim"; break;
    case B_REPLACE: fn = "sd_str_replace"; break;
    case B_UPPER: fn = "sd_str_upper"; break;
    case B_LOWER: fn = "sd_str_lower"; break;
    case B_BYTES: fn = "sd_str_bytes"; break;
    case B_JOIN: fn = "sd_list_join"; break;
    case B_MESSAGE: fn = "sd_err_message"; break;
    }
    Expr **all = alloc(sizeof(Expr *) * (size_t)(e->args.n + 1));
    all[0] = r;
    for (int i = 0; i < e->args.n; i++) all[i + 1] = a[i];
    const char **xs = gen_list(all, e->args.n + 1, false, false, &pre);
    return with_pre(&pre, fmt("%s(%s)", fn, join(xs, e->args.n + 1, NULL)));
  }
  case B_LSLICE: {
    Expr *all[3] = {r, a[0], a[1]};
    const char **xs = gen_list(all, 3, false, false, &pre);
    return with_pre(&pre, fmt("sd_list_slice(%s, %s, %s, sizeof(%s), %s)", xs[0], xs[1], xs[2], ctype(rt->elem),
                              rtp_fn(rt->elem)));
  }
  case B_LCONTAINS:
  case B_LINDEX: {
    Type *el = rt->elem;
    const char *l = tmp(), *x = tmp(), *i = tmp();
    const char *find = fmt("sd_list_find(%s, &%s, sizeof(%s), %s)", l, x, ctype(el), eqp_fn(el));
    if (e->bid == B_LCONTAINS)
      return fmt("({ sd_list *%s = %s; %s %s = %s; %s >= 0; })", l, gexpr(r), ctype(el), x, gexpr(a[0]), find);
    return fmt("({ sd_list *%s = %s; %s %s = %s; int64_t %s = %s; %s < 0 ? (sd_O_i){0} : (sd_O_i){true, %s}; })", l,
               gexpr(r), ctype(el), x, gexpr(a[0]), i, find, i, i);
  }
  case B_KEYS: {
    Type *k = rt->key;
    int kt = k == t_int ? 0 : k == t_byte ? 1 : k == t_bool ? 2 : 3;
    return fmt("sd_map_keys(%s, %d)", gexpr(r), kt);
  }
  case B_PUSH: {
    Type *el = rt->elem;
    const char *v = tmp();
    return fmt("({ %s %s = %s; *(%s*)sd_list_push(%s, sizeof(%s), %s) = %s; })", ctype(el), v, gowned(a[0]), ctype(el),
               gplace(r), ctype(el), rtp_fn(el), v);
  }
  case B_POP: {
    Type *el = rt->elem;
    const char *p = tmp();
    return fmt("({ %s *%s = (%s*)sd_list_pop(%s, sizeof(%s), %s); %s; })", ctype(el), p, ctype(el), gplace(r), ctype(el),
               rtp_fn(el), opt_from_ptr(e->t, p, false));
  }
  case B_INSERT: {
    Type *el = rt->elem;
    const char *i = tmp(), *v = tmp();
    return fmt("({ int64_t %s = %s; %s %s = %s; *(%s*)sd_list_insert(%s, %s, sizeof(%s), %s) = %s; })", i, gexpr(a[0]),
               ctype(el), v, gowned(a[1]), ctype(el), gplace(r), i, ctype(el), rtp_fn(el), v);
  }
  case B_REMOVE: {
    Type *el = rt->elem;
    const char *i = tmp();
    return fmt("({ int64_t %s = %s; sd_list_remove(%s, %s, sizeof(%s), %s); })", i, gexpr(a[0]), gplace(r), i, ctype(el),
               rtp_fn(el));
  }
  case B_REVERSE: return fmt("sd_list_reverse(%s, sizeof(%s), %s)", gplace(r), ctype(rt->elem), rtp_fn(rt->elem));
  case B_SORT: {
    Type *el = rt->elem;
    const char *cmp = el == t_int ? "sd_cmp_i" : el == t_float ? "sd_cmp_f" : el == t_string ? "sd_cmp_s" : "sd_cmp_y";
    return fmt("sd_list_sort(%s, sizeof(%s), NULL, %s)", gplace(r), ctype(el), cmp);
  }
  case B_DELETE: {
    Type *k = rt->key;
    const char *kv = tmp();
    return fmt("({ %s %s = %s; sd_map_del_%s(%s, %s, %s); })", ctype(k), kv, gexpr(a[0]), key_suffix(k), gplace(r),
               key_arg(k, kv), rtp_fn(rt->elem));
  }
  }
  die(e->pos, "internal error: unknown builtin");
}

static const char *gpkg(Expr *e) {
  Buf pre = {0};
  const char **xs = gen_list(e->args.v, e->args.n, false, false, &pre);
  const char *r = NULL;
  switch (e->bid) {
  case F_PRINT: r = fmt("sd_print(%s)", xs[0]); break;
  case F_PRINTLN: r = fmt("sd_println(%s)", xs[0]); break;
  case F_EPRINT: r = fmt("sd_eprint(%s)", xs[0]); break;
  case F_EPRINTLN: r = fmt("sd_eprintln(%s)", xs[0]); break;
  case F_EXIT: r = fmt("sd_os_exit(%s)", xs[0]); break;
  case F_ENV: r = fmt("sd_os_env(%s)", xs[0]); break;
  case F_RUN: r = gtuple_out(e->t, "int64_t", "sd_os_run(%s, &%s)", xs[0]); break;
  case F_READ: r = gtuple_out(e->t, "sd_str", "sd_fs_read(%s, &%s)", xs[0]); break;
  case F_WRITE: r = fmt("sd_fs_write(%s, %s)", xs[0], xs[1]); break;
  case F_EXISTS: r = fmt("sd_fs_exists(%s)", xs[0]); break;
  case F_ISDIR: r = fmt("sd_fs_isdir(%s)", xs[0]); break;
  case F_LIST: r = gtuple_out(e->t, "sd_list*", "sd_fs_list(%s, &%s)", xs[0]); break;
  case F_MKDIR: r = fmt("sd_fs_mkdir(%s)", xs[0]); break;
  case F_REMOVE: r = fmt("sd_fs_remove(%s)", xs[0]); break;
  case F_ERRNEW: r = fmt("sd_err_new(%s)", xs[0]); break;
  case F_EXECUTABLE: r = "sd_os_executable()"; break;
  case F_CWD: r = gtuple_out(e->t, "sd_str", "sd_os_cwd(%s&%s)", ""); break;
  }
  return with_pre(&pre, r);
}

static const char *gcall(Expr *e) {
  Buf pre = {0};
  switch (e->sub) {
  case CK_FUNC: {
    const char **xs = gen_list(e->args.v, e->args.n, false, false, &pre);
    return with_pre(&pre, fmt("%s(%s)", e->fn->cname, join(xs, e->args.n, NULL)));
  }
  case CK_METHOD: {
    FnDecl *m = e->fn;
    Expr *recv = e->a->a;
    if (m->recv_mut) {
      /* arguments first (retained: the method may modify what they alias), then the place */
      const char **xs = gen_list(e->args.v, e->args.n, true, true, &pre);
      return with_pre(&pre, fmt("%s(%s)", m->cname, join(xs, e->args.n, gplace(recv))));
    }
    Expr **all = alloc(sizeof(Expr *) * (size_t)(e->args.n + 1));
    all[0] = recv;
    for (int i = 0; i < e->args.n; i++) all[i + 1] = e->args.v[i];
    const char **xs = gen_list(all, e->args.n + 1, false, false, &pre);
    return with_pre(&pre, fmt("%s(%s)", m->cname, join(xs, e->args.n + 1, NULL)));
  }
  case CK_ENUM: {
    const char **xs = gen_list(e->args.v, e->args.n, true, false, &pre);
    return with_pre(&pre, fmt("sd_mk_%s__%s(%s)", e->td->name, e->td->cases.v[e->ecase].name, join(xs, e->args.n, NULL)));
  }
  case CK_CONV: {
    Expr *x = e->args.v[0];
    Type *from = x->t, *to = e->t;
    const char *v = gexpr(x);
    if (from == to) return v;
    if (to == t_int) return from == t_float ? fmt("sd_f2i(%s)", v) : fmt("((int64_t)(%s))", v);
    if (to == t_float) return fmt("((double)(%s))", v);
    if (to == t_byte) return fmt("sd_i2y(%s)", v);
    if (to == t_string) return fmt("sd_str_from_bytes(%s)", v);
    return v;
  }
  case CK_PKG: return gpkg(e);
  case CK_BUILTIN: return gbuiltin(e);
  }
  die(e->pos, "internal error: unresolved call");
}

static const char *gbinary(Expr *e) {
  int op = e->op;
  if (op == P_ANDAND) return fmt("(%s && %s)", gexpr(e->a), gexpr(e->b));
  if (op == P_OROR) return fmt("(%s || %s)", gexpr(e->a), gexpr(e->b));
  if ((op == P_EQ || op == P_NE) && (e->a->k == E_NULL || e->b->k == E_NULL)) {
    Expr *x = e->a->k == E_NULL ? e->b : e->a;
    const char *n = is_null(x->t, gexpr(x));
    return op == P_EQ ? n : fmt("(!%s)", n);
  }
  Buf pre = {0};
  Expr *both[2] = {e->a, e->b};
  const char **xs = gen_list(both, 2, false, false, &pre);
  const char *a = xs[0], *b = xs[1], *r;
  Type *t = e->a->t;
  bool isint = t == t_int, isbyte = t == t_byte;
  switch (op) {
  case P_EQ: r = eq_expr(t, a, b); break;
  case P_NE: r = fmt("(!%s)", eq_expr(t, a, b)); break;
  case P_LT: case P_LE: case P_GT: case P_GE:
    if (t == t_string) r = fmt("(sd_str_cmp(%s, %s) %s 0)", a, b, tok_names[op]);
    else r = fmt("((%s) %s (%s))", a, tok_names[op], b);
    break;
  case P_PLUS:
    r = isint ? fmt("sd_add(%s, %s)", a, b) : t == t_string ? fmt("sd_cat(%s, %s)", a, b) : fmt("((%s) + (%s))", a, b);
    break;
  case P_MINUS: r = isint ? fmt("sd_sub(%s, %s)", a, b) : fmt("((%s) - (%s))", a, b); break;
  case P_STAR: r = isint ? fmt("sd_mul(%s, %s)", a, b) : fmt("((%s) * (%s))", a, b); break;
  case P_SLASH: r = isint ? fmt("sd_div(%s, %s)", a, b) : fmt("((%s) / (%s))", a, b); break;
  case P_PERCENT: r = fmt("sd_mod(%s, %s)", a, b); break;
  case P_AMP: case P_PIPE: case P_CARET:
    r = isbyte ? fmt("((uint8_t)((%s) %s (%s)))", a, tok_names[op], b) : fmt("((%s) %s (%s))", a, tok_names[op], b);
    break;
  case P_SHL: r = isbyte ? fmt("sd_shl_y(%s, %s)", a, b) : fmt("sd_shl(%s, %s)", a, b); break;
  case P_SHR: r = isbyte ? fmt("sd_shr_y(%s, %s)", a, b) : fmt("sd_shr(%s, %s)", a, b); break;
  default: die(e->pos, "internal error: operator");
  }
  return with_pre(&pre, r);
}

static const char *gexpr(Expr *e) {
  switch (e->k) {
  case E_INT:
    if (e->u == (uint64_t)1 << 63) return "INT64_MIN";
    return fmt("INT64_C(%lld)", (long long)(int64_t)e->u);
  case E_FLOAT:
    if (e->f > 1.7976931348623157e308) return "HUGE_VAL";
    return fmt("%a", e->f);
  case E_BYTE: return fmt("((uint8_t)%u)", (unsigned)e->u);
  case E_STRING: return fmt("((sd_str){%s, %lld})", c_string(e->s, e->slen), (long long)e->slen);
  case E_BOOL: return e->u ? "true" : "false";
  case E_NULL: return e->t->k == TY_OPT || e->t == t_error ? null_of(e->t) : zero_of(e->t);
  case E_IDENT: {
    const char *v = var_value(e);
    return e->narrowed ? unwrap_val(e->orig, v) : v;
  }
  case E_FIELD: {
    if (e->sub == FK_ENUMVAL) return fmt("(&sd_k_%s__%s)", e->td->name, e->td->cases.v[e->ecase].name);
    const char *v = fmt("(%s).f_%s", gexpr(e->a), e->s);
    return e->narrowed ? unwrap_val(e->orig, v) : v;
  }
  case E_INDEX: {
    Buf pre = {0};
    Expr *both[2] = {e->a, e->b};
    const char **xs = gen_list(both, 2, false, false, &pre);
    if (e->sub == IK_STRING) return with_pre(&pre, fmt("sd_str_at(%s, %s)", xs[0], xs[1]));
    if (e->sub == IK_LIST) {
      Type *el = e->narrowed ? e->orig : e->t;
      const char *ct = ctype(el);
      const char *v = fmt("(*(%s*)sd_list_at(%s, %s, sizeof(%s)))", ct, xs[0], xs[1], ct);
      return with_pre(&pre, e->narrowed ? unwrap_val(e->orig, v) : v);
    }
    Type *mt = e->a->t, *vt = mt->elem;
    const char *p = tmp();
    bprintf(&pre, "%s *%s = (%s*)sd_map_get_%s(%s, %s); ", ctype(vt), p, ctype(vt), key_suffix(mt->key), xs[0],
            key_arg(mt->key, xs[1]));
    return with_pre(&pre, opt_from_ptr(e->t, p, true));
  }
  case E_CALL: return gcall(e);
  case E_UNARY:
    if (e->op == P_NOT) return fmt("(!%s)", gexpr(e->a));
    if (e->t == t_int) return fmt("sd_neg(%s)", gexpr(e->a));
    return fmt("(-(%s))", gexpr(e->a));
  case E_BINARY: return gbinary(e);
  case E_TRY: {
    Expr *c = e->a;
    Type *ct = c->t;
    const char *t = tmp();
    if (ct == t_error) return fmt("({ sd_err *%s = %s; if (%s) return %s; })", t, gexpr(c), t, errret(t));
    const char *err = fmt("%s.r%d", t, ct->np - 1);
    if (ct->np == 2) return fmt("({ %s %s = %s; if (%s) return %s; %s.r0; })", ctype(ct), t, gexpr(c), err, errret(err), t);
    Buf b = {0};
    for (int i = 0; i < ct->np - 1; i++) bprintf(&b, "%s%s.r%d", i ? ", " : "", t, i);
    const char *r = fmt("({ %s %s = %s; if (%s) return %s; (%s){%s}; })", ctype(ct), t, gexpr(c), err, errret(err),
                        ctype(e->t), b.s);
    free(b.s);
    return r;
  }
  case E_LIST: {
    Type *el = e->t->elem;
    const char *ct = ctype(el);
    if (!e->args.n) return fmt("sd_list_new(sizeof(%s), 0)", ct);
    const char *l = tmp(), *d = tmp();
    Buf b = {0};
    bprintf(&b, "({ sd_list *%s = sd_list_new(sizeof(%s), %d); %s *%s = (%s*)%s->data; ", l, ct, e->args.n, ct, d, ct, l);
    for (int i = 0; i < e->args.n; i++) bprintf(&b, "%s[%d] = %s; ", d, i, gowned(e->args.v[i]));
    bprintf(&b, "%s->len = %d; %s; })", l, e->args.n, l);
    const char *r = sdup(b.s);
    free(b.s);
    return r;
  }
  case E_MAP: {
    Type *mt = e->t;
    const char *m = tmp();
    Buf b = {0};
    bprintf(&b, "({ sd_map *%s = sd_map_new(%d, sizeof(%s)); ", m, mt->key == t_string, ctype(mt->elem));
    for (int i = 0; i < e->keys.n; i++) {
      const char *k = tmp(), *v = tmp();
      bprintf(&b, "{ %s %s = %s; %s %s = %s; *(%s*)sd_map_slot_%s(&%s, %s, NULL) = %s; } ", ctype(mt->key), k,
              gexpr(e->keys.v[i]), ctype(mt->elem), v, gowned(e->args.v[i]), ctype(mt->elem), key_suffix(mt->key), m,
              key_arg(mt->key, k), v);
    }
    bprintf(&b, "%s; })", m);
    const char *r = sdup(b.s);
    free(b.s);
    return r;
  }
  case E_STRUCT: {
    const char *ct = ctype(e->t);
    if (!e->names.n) return fmt("((%s){0})", ct);
    const char *v = tmp();
    Buf b = {0};
    bprintf(&b, "({ %s %s; ", ct, v);
    for (int i = 0; i < e->names.n; i++) bprintf(&b, "%s.f_%s = %s; ", v, e->names.v[i], gowned(e->args.v[i]));
    bprintf(&b, "%s; })", v);
    const char *r = sdup(b.s);
    free(b.s);
    return r;
  }
  case E_WRAP: return wrap_val(e->t, gexpr(e->a));
  }
  die(e->pos, "internal error: expression");
}

/* ---- statements ---- */

static void gblock(Block *b);
static void gstmt(Stmt *s);

/* Stores an owned value into the place lv (an lvalue checked by check_lvalue). */
static void gstore(Expr *lv, const char *val) {
  if (lv->k == E_INDEX && lv->sub == IK_MAP) {
    Type *mt = lv->a->t;
    const char *v = tmp(), *k = tmp();
    emit("{ %s %s = %s; %s %s = %s; *(%s*)sd_map_slot_%s(%s, %s, %s) = %s; }", ctype(mt->elem), v, val, ctype(mt->key), k,
         gexpr(lv->b), ctype(mt->elem), key_suffix(mt->key), gplace(lv->a), key_arg(mt->key, k), rtp_fn(mt->elem), v);
    return;
  }
  Type *t = lv->t;
  if (lv->k == E_IDENT && lv->var->k != VK_MUTRECV && !needs_rt(t)) {
    emit("l_%s = %s;", lv->var->name, val);
    return;
  }
  const char *v = tmp(), *p = tmp();
  if (needs_rt(t))
    emit("{ %s %s = %s; %s *%s = %s; %s(*%s); *%s = %s; }", ctype(t), v, val, ctype(t), p, gplace(lv), rl_fn(t), p, p, v);
  else
    emit("{ %s %s = %s; %s *%s = %s; *%s = %s; }", ctype(t), v, val, ctype(t), p, gplace(lv), p, v);
}

static void gassign(Stmt *s) {
  int n = s->targets.n;
  if (n == 1) {
    Target *tg = &s->targets.v[0];
    if (tg->te) {
      if (tg->var) emit("%s l_%s = %s;", ctype(tg->t), tg->name, gowned(s->e));
      else emit("(void)(%s);", gexpr(s->e));
    } else if (tg->discard) {
      emit("(void)(%s);", gexpr(s->e));
    } else {
      gstore(tg->lv, gowned(s->e));
    }
    return;
  }
  Type *rt = s->e->t;
  for (int i = 0; i < n; i++) {
    Target *tg = &s->targets.v[i];
    if (tg->te && tg->var) emit("%s l_%s;", ctype(tg->t), tg->name);
  }
  emit("{");
  indent++;
  const char *t = tmp();
  emit("%s %s = %s;", ctype(rt), t, gexpr(s->e));
  for (int i = 0; i < n; i++) {
    Target *tg = &s->targets.v[i];
    if (tg->discard || (tg->te && !tg->var)) continue;
    const char *val = fmt("%s.r%d", t, i);
    if (tg->t != rt->parts[i]) val = wrap_val(tg->t, val);
    if (s->discard_err) val = fmt("(%s.r%d ? %s : %s)", t, n - 1, null_of(tg->t), val);
    if (tg->te) emit("l_%s = %s;", tg->name, val);
    else gstore(tg->lv, val);
  }
  indent--;
  emit("}");
}

static bool mentions_var(Expr *e, Var *v) {
  if (!e) return false;
  if (e->k == E_IDENT && e->var == v) return true;
  if (mentions_var(e->a, v) || mentions_var(e->b, v)) return true;
  for (int i = 0; i < e->args.n; i++)
    if (mentions_var(e->args.v[i], v)) return true;
  for (int i = 0; i < e->keys.n; i++)
    if (mentions_var(e->keys.v[i], v)) return true;
  return false;
}

/* A returned local variable dies with the frame, so it can be moved out without a retain. */
static const char *greturn_value(Stmt *s, int idx) {
  Expr *e = s->exprs.v[idx];
  Expr *x = e->k == E_WRAP ? e->a : e;
  if (x->k == E_IDENT && x->var && x->var->k == VK_LOCAL && !x->narrowed) {
    bool once = true;
    for (int i = 0; i < s->exprs.n; i++)
      if (i != idx && mentions_var(s->exprs.v[i], x->var)) once = false;
    if (once) return e == x ? gexpr(x) : wrap_val(e->t, gexpr(x));
  }
  return gowned(e);
}

static const char *capture_stmt(Stmt *s) {
  Buf *saved = out;
  int saved_indent = indent;
  Buf b = {0};
  out = &b;
  indent = 0;
  gstmt(s);
  out = saved;
  indent = saved_indent;
  for (size_t i = 0; i < b.len; i++)
    if (b.s[i] == '\n') b.s[i] = ' ';
  const char *r = sdup(bstr(&b));
  free(b.s);
  return r;
}

static void gswitch(Stmt *s) {
  Type *t = s->e->t;
  const char *sv = tmp();
  emit("{");
  indent++;
  emit("%s %s = %s;", ctype(t), sv, gexpr(s->e));
  bool first = true;
  Case *def = NULL;
  for (int i = 0; i < s->cases.n; i++) {
    Case *c = &s->cases.v[i];
    if (c->isdefault) {
      def = c;
      continue;
    }
    Buf cond = {0};
    for (int j = 0; j < c->labels.n; j++) {
      Label *l = &c->labels.v[j];
      if (j) bput(&cond, " || ");
      if (t->k == TY_ENUM) bprintf(&cond, "%s->tag == %d", sv, l->ecase);
      else bput(&cond, eq_expr(t, sv, gexpr(l->e)));
    }
    emit("%sif (%s) {", first ? "" : "} else ", cond.s);
    free(cond.s);
    first = false;
    indent++;
    if (t->k == TY_ENUM && c->labels.n == 1 && c->labels.v[0].vars) {
      Label *l = &c->labels.v[0];
      ECase *ec = &t->td->cases.v[l->ecase];
      for (int k = 0; k < l->params.n; k++) {
        if (!l->vars[k]) continue;
        Type *pt = l->params.v[k].t;
        const char *v = fmt("%s->u.c_%s.f%d", sv, ec->name, k);
        if (needs_rt(pt)) v = fmt("%s(%s)", rt_fn(pt), v);
        emit("%s l_%s = %s;", ctype(pt), l->params.v[k].name, v);
      }
    }
    gblock(&c->body);
    indent--;
  }
  if (def) {
    if (first) {
      emit("{");
    } else {
      emit("} else {");
    }
    indent++;
    gblock(&def->body);
    indent--;
    emit("}");
  } else if (!first) {
    emit("}");
  }
  indent--;
  emit("}");
}

static void gstmt(Stmt *s) {
  switch (s->k) {
  case S_EXPR: emit("%s;", gexpr(s->e)); return;
  case S_ASSIGN: gassign(s); return;
  case S_INCDEC: {
    Expr *lv = s->e;
    const char *opi = s->inc ? "sd_add" : "sd_sub";
    if (lv->k == E_IDENT && lv->var->k != VK_MUTRECV) {
      if (lv->t == t_int) emit("l_%s = %s(l_%s, 1);", lv->var->name, opi, lv->var->name);
      else emit("l_%s = l_%s %s 1.0;", lv->var->name, lv->var->name, s->inc ? "+" : "-");
      return;
    }
    const char *p = tmp();
    if (lv->t == t_int) emit("{ int64_t *%s = %s; *%s = %s(*%s, 1); }", p, gplace(lv), p, opi, p);
    else emit("{ double *%s = %s; *%s = *%s %s 1.0; }", p, gplace(lv), p, p, s->inc ? "+" : "-");
    return;
  }
  case S_CONST: emit("%s l_%s = %s;", ctype(s->var->t), s->name, gowned(s->e)); return;
  case S_IF:
    emit("if (%s) {", gexpr(s->cond));
    indent++;
    gblock(&s->body);
    indent--;
    if (s->has_else) {
      emit("} else {");
      indent++;
      gblock(&s->els);
      indent--;
    }
    emit("}");
    return;
  case S_FOR: {
    emit("{");
    indent++;
    if (s->init) gstmt(s->init);
    const char *post = s->post ? fmt("({ %s })", capture_stmt(s->post)) : "";
    emit("for (; %s; %s) {", s->cond ? gexpr(s->cond) : "", post);
    indent++;
    gblock(&s->body);
    indent--;
    emit("}");
    indent--;
    emit("}");
    return;
  }
  case S_SWITCH: gswitch(s); return;
  case S_RETURN: {
    int n = cur_fn->rtypes.n;
    if (n == 0) {
      emit("return;");
    } else if (s->exprs.n == 1 && n == 1) {
      emit("return %s;", greturn_value(s, 0));
    } else if (s->exprs.n == 1) {
      emit("return %s;", gexpr(s->exprs.v[0]));
    } else {
      int eff = 0;
      for (int i = 0; i < n; i++)
        if (has_effects(s->exprs.v[i])) eff++;
      Buf b = {0};
      if (eff >= 2) {
        emit("{");
        indent++;
        for (int i = 0; i < n; i++) {
          const char *t = tmp();
          emit("%s %s = %s;", ctype(s->exprs.v[i]->t), t, greturn_value(s, i));
          bprintf(&b, "%s.r%d = %s", i ? ", " : "", i, t);
        }
        emit("return (%s){%s};", ctype(cur_fn->ret), b.s);
        indent--;
        emit("}");
      } else {
        for (int i = 0; i < n; i++) bprintf(&b, "%s.r%d = %s", i ? ", " : "", i, greturn_value(s, i));
        emit("return (%s){%s};", ctype(cur_fn->ret), b.s);
      }
      free(b.s);
    }
    return;
  }
  case S_BREAK: emit("break;"); return;
  case S_CONTINUE: emit("continue;"); return;
  }
}

static void gblock(Block *b) {
  for (int i = 0; i < b->n; i++) gstmt(b->v[i]);
}

/* ---- declarations ---- */

static const char *fn_sig(FnDecl *f) {
  Buf b = {0};
  bprintf(&b, "static %s %s(", ctype(f->ret), f->cname);
  int n = 0;
  if (f->is_method) {
    bprintf(&b, "%s%s l_%s", ctype(f->recv->t), f->recv_mut ? " *" : "", f->recv_name);
    n++;
  }
  for (int i = 0; i < f->params.n; i++, n++) {
    Param *p = &f->params.v[i];
    if (!strcmp(p->name, "_")) bprintf(&b, "%s%s _p%d", n ? ", " : "", ctype(p->t), i);
    else bprintf(&b, "%s%s l_%s", n ? ", " : "", ctype(p->t), p->name);
  }
  if (!n) bput(&b, "void");
  bput(&b, ")");
  const char *r = sdup(b.s);
  free(b.s);
  return r;
}

static void gen_helper(Type *t, unsigned h) {
  const char *ct = ctype(t), *m = t->mangle;
  Buf *o = &out_helpers;
  const char *attr = "static __attribute__((unused))";
  switch (h) {
  case H_RT:
    bprintf(&out_protos, "%s %s sd_rt_%s(%s v);\n", attr, ct, m, ct);
    bprintf(o, "%s %s sd_rt_%s(%s v) {\n", attr, ct, m, ct);
    if (t->k == TY_OPT) {
      bput(o, "  if (v) v->rc++;\n");
    } else {
      for (int i = 0; i < t->td->fields.n; i++) {
        Param *f = &t->td->fields.v[i];
        if (needs_rt(f->t)) bprintf(o, "  v.f_%s = %s(v.f_%s);\n", f->name, rt_fn(f->t), f->name);
      }
    }
    bput(o, "  return v;\n}\n");
    break;
  case H_RL:
    bprintf(&out_protos, "%s void sd_rl_%s(%s v);\n", attr, m, ct);
    bprintf(o, "%s void sd_rl_%s(%s v) {\n", attr, m, ct);
    if (t->k == TY_OPT) {
      bput(o, "  if (v && v->rc > 0) v->rc--;\n");
    } else {
      for (int i = 0; i < t->td->fields.n; i++) {
        Param *f = &t->td->fields.v[i];
        if (needs_rt(f->t)) bprintf(o, "  %s(v.f_%s);\n", rl_fn(f->t), f->name);
      }
    }
    bput(o, "}\n");
    break;
  case H_RTP:
    bprintf(&out_protos, "%s void sd_rtp_%s(void *p);\n", attr, m);
    bprintf(o, "%s void sd_rtp_%s(void *p) { *(%s*)p = %s(*(%s*)p); }\n", attr, m, ct, rt_fn(t), ct);
    break;
  case H_EQP:
    bprintf(&out_protos, "%s bool sd_eqp_%s(const void *a, const void *b);\n", attr, m);
    bprintf(o, "%s bool sd_eqp_%s(const void *a, const void *b) { return %s; }\n", attr, m,
            eq_expr(t, fmt("*(%s*)a", ct), fmt("*(%s*)b", ct)));
    break;
  case H_EQ: {
    bprintf(&out_protos, "%s bool sd_eq_%s(%s a, %s b);\n", attr, m, ct, ct);
    bprintf(o, "%s bool sd_eq_%s(%s a, %s b) {\n", attr, m, ct, ct);
    switch (t->k) {
    case TY_LIST:
      bprintf(o, "  return sd_list_eq(a, b, sizeof(%s), %s);\n", ctype(t->elem), eqp_fn(t->elem));
      break;
    case TY_MAP: bprintf(o, "  return sd_map_eq(a, b, %s);\n", eqp_fn(t->elem)); break;
    case TY_STRUCT:
      bput(o, "  return true");
      for (int i = 0; i < t->td->fields.n; i++) {
        Param *f = &t->td->fields.v[i];
        bprintf(o, " && %s", eq_expr(f->t, fmt("a.f_%s", f->name), fmt("b.f_%s", f->name)));
      }
      bput(o, ";\n");
      break;
    case TY_ENUM: {
      bput(o, "  if (a == b) return true;\n  if (!a || !b || a->tag != b->tag) return false;\n  switch (a->tag) {\n");
      TypeDecl *td = t->td;
      for (int i = 0; i < td->cases.n; i++) {
        ECase *c = &td->cases.v[i];
        if (!c->params.n) continue;
        bprintf(o, "  case %d: return true", i);
        for (int k = 0; k < c->params.n; k++)
          bprintf(o, " && %s", eq_expr(c->params.v[k].t, fmt("a->u.c_%s.f%d", c->name, k), fmt("b->u.c_%s.f%d", c->name, k)));
        bput(o, ";\n");
      }
      bput(o, "  }\n  return true;\n");
      break;
    }
    case TY_OPT: {
      Type *el = t->elem;
      switch (el->k) {
      case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_BYTE:
        bprintf(o, "  return a.has == b.has && (!a.has || %s);\n", eq_expr(el, "a.v", "b.v"));
        break;
      case TY_STRING: bput(o, "  if (!a.p || !b.p) return a.p == b.p;\n  return sd_str_eq(a, b);\n"); break;
      case TY_STRUCT:
        bprintf(o, "  if (!a || !b) return a == b;\n  return %s;\n", eq_expr(el, "a->v", "b->v"));
        break;
      default: bprintf(o, "  if (!a || !b) return a == b;\n  return %s;\n", eq_expr(el, "a", "b")); break;
      }
      break;
    }
    default: bput(o, "  return false;\n"); break;
    }
    bput(o, "}\n");
    break;
  }
  }
}

static Buf out_types;

static void gen_struct_def(TypeDecl *td) {
  if (td->topo == 2) return;
  if (td->topo == 1) die(td->pos, "struct %s contains itself; use a nullable field (%s?) or a list to break the cycle", td->name, td->name);
  td->topo = 1;
  for (int i = 0; i < td->fields.n; i++)
    if (td->fields.v[i].t->k == TY_STRUCT) gen_struct_def(td->fields.v[i].t->td);
  td->topo = 2;
  bprintf(&out_types, "struct sd_S_%s {", td->name);
  for (int i = 0; i < td->fields.n; i++) bprintf(&out_types, " %s f_%s;", ctype(td->fields.v[i].t), td->fields.v[i].name);
  if (!td->fields.n) bput(&out_types, " char unused_;");
  bput(&out_types, " };\n");
}

static void gen_types(void) {
  for (int i = 0; i < tdecls.n; i++) {
    TypeDecl *td = tdecls.v[i];
    if (td->is_enum) bprintf(&out_types, "typedef struct sd_E_%s sd_E_%s;\n", td->name, td->name);
    else bprintf(&out_types, "typedef struct sd_S_%s sd_S_%s;\ntypedef struct sd_B_%s sd_B_%s;\n", td->name, td->name, td->name, td->name);
  }
  for (int i = 0; i < tdecls.n; i++)
    if (!tdecls.v[i]->is_enum) gen_struct_def(tdecls.v[i]);
  const char *attr = "static __attribute__((unused))";
  for (int i = 0; i < tdecls.n; i++) {
    TypeDecl *td = tdecls.v[i];
    if (td->is_enum) continue;
    const char *n = td->name;
    bprintf(&out_types, "struct sd_B_%s { int64_t rc; sd_S_%s v; };\n", n, n);
    bprintf(&out_protos, "%s sd_B_%s *sd_bx_%s(sd_S_%s v);\n%s sd_B_%s *sd_bm_%s(sd_B_%s **pb);\n", attr, n, n, n, attr, n, n, n);
    bprintf(&out_helpers, "%s sd_B_%s *sd_bx_%s(sd_S_%s v) { sd_B_%s *b = sd_alloc(sizeof *b); b->rc = 1; b->v = v; return b; }\n",
            attr, n, n, n, n);
    const char *copy = needs_rt(td->t) ? fmt("%s(b->v)", rt_fn(td->t)) : "b->v";
    bprintf(&out_helpers,
            "%s sd_B_%s *sd_bm_%s(sd_B_%s **pb) { sd_B_%s *b = *pb; if (b->rc > 1) { b->rc--; b = sd_bx_%s(%s); *pb = b; } return b; }\n",
            attr, n, n, n, n, n, copy);
  }
  for (int i = 0; i < tdecls.n; i++) {
    TypeDecl *td = tdecls.v[i];
    if (!td->is_enum) continue;
    const char *n = td->name;
    bprintf(&out_types, "struct sd_E_%s { int tag;", n);
    bool any = false;
    for (int j = 0; j < td->cases.n; j++)
      if (td->cases.v[j].params.n) any = true;
    if (any) {
      bput(&out_types, " union {");
      for (int j = 0; j < td->cases.n; j++) {
        ECase *c = &td->cases.v[j];
        if (!c->params.n) continue;
        bput(&out_types, " struct {");
        for (int k = 0; k < c->params.n; k++) bprintf(&out_types, " %s f%d;", ctype(c->params.v[k].t), k);
        bprintf(&out_types, " } c_%s;", c->name);
      }
      bput(&out_types, " } u;");
    }
    bput(&out_types, " };\n");
    for (int j = 0; j < td->cases.n; j++) {
      ECase *c = &td->cases.v[j];
      if (!c->params.n) {
        bprintf(&out_helpers, "static sd_E_%s sd_k_%s__%s = {%d};\n", n, n, c->name, j);
        continue;
      }
      Buf ps = {0}, body = {0};
      for (int k = 0; k < c->params.n; k++) {
        bprintf(&ps, "%s%s a%d", k ? ", " : "", ctype(c->params.v[k].t), k);
        bprintf(&body, " e->u.c_%s.f%d = a%d;", c->name, k, k);
      }
      bprintf(&out_protos, "%s sd_E_%s *sd_mk_%s__%s(%s);\n", attr, n, n, c->name, ps.s);
      bprintf(&out_helpers, "%s sd_E_%s *sd_mk_%s__%s(%s) { sd_E_%s *e = sd_alloc(sizeof *e); e->tag = %d;%s return e; }\n",
              attr, n, n, c->name, ps.s, n, j, body.s);
      free(ps.s);
      free(body.s);
    }
  }
}

static bool is_literal(Expr *e) {
  return e->k == E_INT || e->k == E_FLOAT || e->k == E_BYTE || e->k == E_STRING || e->k == E_BOOL;
}

static Buf out_consts, out_fns;

static void gen_program(void) {
  gen_types();
  /* constants: lazily initialised on first use */
  out = &out_consts;
  for (int i = 0; i < cdecls.n; i++) {
    ConstDecl *c = cdecls.v[i];
    const char *ct = ctype(c->t);
    bprintf(&out_protos, "static %s sd_c_%s(void);\n", ct, c->name);
    if (is_literal(c->e)) {
      emit("static %s sd_c_%s(void) { return %s; }", ct, c->name, gexpr(c->e));
      continue;
    }
    emit("static %s sd_c_%s(void) {", ct, c->name);
    indent++;
    emit("static %s v;", ct);
    emit("static int st;");
    emit("if (st == 2) return v;");
    emit("if (st == 1) sd_crash(\"constant %s depends on itself\");", c->name);
    emit("st = 1;");
    emit("v = %s;", gowned(c->e));
    emit("st = 2;");
    emit("return v;");
    indent--;
    emit("}");
  }
  out = &out_fns;
  for (int i = 0; i < fns.n; i++) {
    FnDecl *f = fns.v[i];
    cur_fn = f;
    const char *sig = fn_sig(f);
    bprintf(&out_protos, "%s;\n", sig);
    emit("%s {", sig);
    indent++;
    gblock(&f->body);
    /* The checker proved every path returns (e.g. through an exhaustive
     * switch); tell the C compiler too. */
    if (f->rtypes.n && (!f->body.n || f->body.v[f->body.n - 1]->k != S_RETURN))
      emit("sd_crash(\"internal error: %s reached its end without returning\");", f->name);
    indent--;
    emit("}");
    emit("%s", "");
  }
  cur_fn = NULL;
  emit("int main(int argc, char **argv) { return sd_finish(sd_f_main(sd_start(argc, argv))); }");
  for (int i = 0; i < helper_queue.n; i++) gen_helper(helper_queue.v[i].t, helper_queue.v[i].h);
}

static void write_program(FILE *f) {
  fputs(runtime_src, f);
  fputs("\n/* ---- program ---- */\n", f);
  fputs(bstr(&out_types), f);
  for (int i = 0; i < tuples.n; i++) {
    Type *t = tuples.v[i];
    fputs("typedef struct {", f);
    for (int j = 0; j < t->np; j++) fprintf(f, " %s r%d;", ctype(t->parts[j]), j);
    fprintf(f, " } %s;\n", ctype(t));
  }
  fputs(bstr(&out_protos), f);
  fputs(bstr(&out_helpers), f);
  fputs(bstr(&out_consts), f);
  fputs(bstr(&out_fns), f);
}

/* ======================================================================
 * Driver
 * ====================================================================== */

static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "%s: error: cannot open file\n", path);
    exit(1);
  }
  Buf b = {0};
  char tmpb[65536];
  size_t n;
  while ((n = fread(tmpb, 1, sizeof tmpb, f)) > 0) bputn(&b, tmpb, n);
  fclose(f);
  *len = b.len;
  return b.s ? b.s : calloc(1, 1);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: seed0 file.sd... > out.c\n");
    return 2;
  }
  init_types();
  for (int i = 1; i < argc; i++) {
    size_t len;
    char *src = read_file(argv[i], &len);
    Tokens toks = {0};
    lex(argv[i], src, len, &toks);
    parse_file(argv[i], &toks);
  }
  check_program();
  gen_program();
  write_program(stdout);
  if (fflush(stdout) != 0) {
    fprintf(stderr, "seed0: error writing output\n");
    return 1;
  }
  return 0;
}

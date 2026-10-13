#include <stdio.h>
#include <stdlib.h>
long m_getchar(void) { return getchar(); }
long m_putchar(long c) { putchar((int)c); return c; }
long m_eputchar(long c) { fputc((int)c, stderr); return c; }
long m_alloc(long n) { return (long)calloc(1, (size_t)n); }
long m_load8(long p) { return *(unsigned char *)p; }
long m_store8(long p, long v) { *(unsigned char *)p = (unsigned char)v; return v; }
long m_exit(long c) { exit((int)c); return 0; }
long m_src ;
long m_pos ;
long m_line ;
long m_kind ;
long m_tstart ;
long m_tlen ;
long m_out (long);
long m_eout (long);
long m_eputnum (long);
long m_fail2 (long, long);
long m_fail (long);
long m_peekc (long);
long m_isdigit (long);
long m_isalpha (long);
long m_skipspace (void);
long m_next (void);
long m_is (long);
long m_restart (void);
long m_peekassign (void);
long m_emittok (void);
long m_skip (long);
long m_take (long);
long m_skipident (void);
long m_ident (void);
long m_isbinop (void);
long m_primary (void);
long m_unary (void);
long m_expr (void);
long m_block (void);
long m_stmt (void);
long m_skipblock (void);
long m_declare (void);
long m_func (void);
long m_define (void);
long m_prelude (void);
long m_readall (void);
long m_main (void);
long m_out ( long m_s ) { 
while ( m_load8 ( m_s ) != 0 ) { 
m_putchar ( m_load8 ( m_s ) ) ; 
m_s = m_s + 1 ; 
} 

return 0 ; 
return 0;
} 
long m_eout ( long m_s ) { 
while ( m_load8 ( m_s ) != 0 ) { 
m_eputchar ( m_load8 ( m_s ) ) ; 
m_s = m_s + 1 ; 
} 

return 0 ; 
return 0;
} 
long m_eputnum ( long m_n ) { 
if ( m_n >= 10 ) { 
m_eputnum ( m_n / 10 ) ; 
} 

m_eputchar ( '0' + m_n % 10 ) ; 
return 0 ; 
return 0;
} 
long m_fail2 ( long m_a , long m_b ) { 
m_eout ( (long)"line " ) ; 
m_eputnum ( m_line ) ; 
m_eout ( (long)": " ) ; 
m_eout ( m_a ) ; 
m_eout ( m_b ) ; 
m_eout ( (long)" (found '" ) ; 
long m_i = 0 ; 
while ( m_i < m_tlen ) { 
m_eputchar ( m_load8 ( m_src + m_tstart + m_i ) ) ; 
m_i = m_i + 1 ; 
} 

m_eout ( (long)"')\n" ) ; 
m_exit ( 1 ) ; 
return 0 ; 
return 0;
} 
long m_fail ( long m_msg ) { 
return m_fail2 ( m_msg , (long)"" ) ; 
return 0;
} 
long m_peekc ( long m_i ) { 
return m_load8 ( m_src + m_pos + m_i ) ; 
return 0;
} 
long m_isdigit ( long m_c ) { 
return m_c >= '0' && m_c <= '9' ; 
return 0;
} 
long m_isalpha ( long m_c ) { 
return m_c >= 'a' && m_c <= 'z' || m_c >= 'A' && m_c <= 'Z' || m_c == '_' ; 
return 0;
} 
long m_skipspace ( void ) { 
long m_going = 1 ; 
while ( m_going ) { 
long m_c = m_peekc ( 0 ) ; 
if ( m_c == '\n' ) { 
m_line = m_line + 1 ; 
m_pos = m_pos + 1 ; 
} 
else if ( m_c == ' ' || m_c == '\t' || m_c == '\r' ) { 
m_pos = m_pos + 1 ; 
} 
else if ( m_c == '/' && m_peekc ( 1 ) == '/' ) { 
while ( m_peekc ( 0 ) != '\n' && m_peekc ( 0 ) != 0 ) { 
m_pos = m_pos + 1 ; 
} 

} 
else { 
m_going = 0 ; 
} 



} 

return 0 ; 
return 0;
} 
long m_next ( void ) { 
m_skipspace ( ) ; 
m_tstart = m_pos ; 
long m_c = m_peekc ( 0 ) ; 
if ( m_c == 0 ) { 
m_kind = 0 ; 
} 
else if ( m_isalpha ( m_c ) ) { 
m_kind = 1 ; 
while ( m_isalpha ( m_peekc ( 0 ) ) || m_isdigit ( m_peekc ( 0 ) ) ) { 
m_pos = m_pos + 1 ; 
} 

} 
else if ( m_isdigit ( m_c ) ) { 
m_kind = 2 ; 
while ( m_isdigit ( m_peekc ( 0 ) ) ) { 
m_pos = m_pos + 1 ; 
} 

} 
else if ( m_c == '"' || m_c == '\'' ) { 
m_kind = 3 ; 
if ( m_c == '\'' ) { 
m_kind = 4 ; 
} 

m_pos = m_pos + 1 ; 
while ( m_peekc ( 0 ) != m_c ) { 
if ( m_peekc ( 0 ) == 0 || m_peekc ( 0 ) == '\n' ) { 
m_fail ( (long)"unterminated literal" ) ; 
} 

if ( m_peekc ( 0 ) == '\\' ) { 
m_pos = m_pos + 1 ; 
} 

m_pos = m_pos + 1 ; 
} 

m_pos = m_pos + 1 ; 
} 
else { 
m_kind = 5 ; 
long m_d = m_peekc ( 1 ) ; 
if ( m_c == '=' && m_d == '=' || m_c == '!' && m_d == '=' || m_c == '<' && m_d == '=' || m_c == '>' && m_d == '=' || m_c == '&' && m_d == '&' || m_c == '|' && m_d == '|' ) { 
m_pos = m_pos + 2 ; 
} 
else { 
m_pos = m_pos + 1 ; 
} 

} 




m_tlen = m_pos - m_tstart ; 
return 0 ; 
return 0;
} 
long m_is ( long m_s ) { 
long m_i = 0 ; 
while ( m_i < m_tlen ) { 
if ( m_load8 ( m_s + m_i ) != m_load8 ( m_src + m_tstart + m_i ) ) { 
return 0 ; 
} 

m_i = m_i + 1 ; 
} 

return m_load8 ( m_s + m_tlen ) == 0 ; 
return 0;
} 
long m_restart ( void ) { 
m_pos = 0 ; 
m_line = 1 ; 
m_next ( ) ; 
return 0 ; 
return 0;
} 
long m_peekassign ( void ) { 
long m_p = m_tstart ; 
long m_l = m_line ; 
m_next ( ) ; 
long m_r = m_is ( (long)"=" ) ; 
m_pos = m_p ; 
m_line = m_l ; 
m_next ( ) ; 
return m_r ; 
return 0;
} 
long m_emittok ( void ) { 
long m_i = 0 ; 
while ( m_i < m_tlen ) { 
m_putchar ( m_load8 ( m_src + m_tstart + m_i ) ) ; 
m_i = m_i + 1 ; 
} 

m_putchar ( ' ' ) ; 
return 0 ; 
return 0;
} 
long m_skip ( long m_s ) { 
if ( ! m_is ( m_s ) ) { 
m_fail2 ( (long)"expected " , m_s ) ; 
} 

m_next ( ) ; 
return 0 ; 
return 0;
} 
long m_take ( long m_s ) { 
if ( ! m_is ( m_s ) ) { 
m_fail2 ( (long)"expected " , m_s ) ; 
} 

m_emittok ( ) ; 
m_next ( ) ; 
return 0 ; 
return 0;
} 
long m_skipident ( void ) { 
if ( m_kind != 1 ) { 
m_fail ( (long)"expected identifier" ) ; 
} 

m_next ( ) ; 
return 0 ; 
return 0;
} 
long m_ident ( void ) { 
if ( m_kind != 1 ) { 
m_fail ( (long)"expected identifier" ) ; 
} 

m_out ( (long)"m_" ) ; 
m_emittok ( ) ; 
m_next ( ) ; 
return 0 ; 
return 0;
} 
long m_isbinop ( void ) { 
return m_kind == 5 && ( m_is ( (long)"+" ) || m_is ( (long)"-" ) || m_is ( (long)"*" ) || m_is ( (long)"/" ) || m_is ( (long)"%" ) || m_is ( (long)"<" ) || m_is ( (long)">" ) || m_is ( (long)"<=" ) || m_is ( (long)">=" ) || m_is ( (long)"==" ) || m_is ( (long)"!=" ) || m_is ( (long)"&&" ) || m_is ( (long)"||" ) ) ; 
return 0;
} 
long m_primary ( void ) { 
if ( m_kind == 2 || m_kind == 4 ) { 
m_emittok ( ) ; 
m_next ( ) ; 
} 
else if ( m_kind == 3 ) { 
m_out ( (long)"(long)" ) ; 
m_emittok ( ) ; 
m_next ( ) ; 
} 
else if ( m_kind == 1 ) { 
m_ident ( ) ; 
if ( m_is ( (long)"(" ) ) { 
m_take ( (long)"(" ) ; 
if ( ! m_is ( (long)")" ) ) { 
m_expr ( ) ; 
while ( m_is ( (long)"," ) ) { 
m_take ( (long)"," ) ; 
m_expr ( ) ; 
} 

} 

m_take ( (long)")" ) ; 
} 

} 
else if ( m_is ( (long)"(" ) ) { 
m_take ( (long)"(" ) ; 
m_expr ( ) ; 
m_take ( (long)")" ) ; 
} 
else { 
m_fail ( (long)"expected expression" ) ; 
} 




return 0 ; 
return 0;
} 
long m_unary ( void ) { 
if ( m_is ( (long)"-" ) || m_is ( (long)"!" ) ) { 
m_emittok ( ) ; 
m_next ( ) ; 
m_unary ( ) ; 
} 
else { 
m_primary ( ) ; 
} 

return 0 ; 
return 0;
} 
long m_expr ( void ) { 
m_unary ( ) ; 
while ( m_isbinop ( ) ) { 
m_emittok ( ) ; 
m_next ( ) ; 
m_unary ( ) ; 
} 

return 0 ; 
return 0;
} 
long m_block ( void ) { 
m_take ( (long)"{" ) ; 
m_putchar ( '\n' ) ; 
while ( ! m_is ( (long)"}" ) ) { 
m_stmt ( ) ; 
} 

m_take ( (long)"}" ) ; 
m_putchar ( '\n' ) ; 
return 0 ; 
return 0;
} 
long m_stmt ( void ) { 
if ( m_is ( (long)"var" ) ) { 
m_next ( ) ; 
m_out ( (long)"long " ) ; 
m_ident ( ) ; 
m_take ( (long)"=" ) ; 
m_expr ( ) ; 
m_take ( (long)";" ) ; 
} 
else if ( m_is ( (long)"if" ) ) { 
m_emittok ( ) ; 
m_next ( ) ; 
m_take ( (long)"(" ) ; 
m_expr ( ) ; 
m_take ( (long)")" ) ; 
m_block ( ) ; 
if ( m_is ( (long)"else" ) ) { 
m_emittok ( ) ; 
m_next ( ) ; 
if ( m_is ( (long)"if" ) ) { 
m_stmt ( ) ; 
} 
else { 
m_block ( ) ; 
} 

} 

} 
else if ( m_is ( (long)"while" ) ) { 
m_emittok ( ) ; 
m_next ( ) ; 
m_take ( (long)"(" ) ; 
m_expr ( ) ; 
m_take ( (long)")" ) ; 
m_block ( ) ; 
} 
else if ( m_is ( (long)"return" ) ) { 
m_emittok ( ) ; 
m_next ( ) ; 
m_expr ( ) ; 
m_take ( (long)";" ) ; 
} 
else if ( m_kind == 1 && m_peekassign ( ) ) { 
m_ident ( ) ; 
m_take ( (long)"=" ) ; 
m_expr ( ) ; 
m_take ( (long)";" ) ; 
} 
else { 
m_expr ( ) ; 
m_take ( (long)";" ) ; 
} 





m_putchar ( '\n' ) ; 
return 0 ; 
return 0;
} 
long m_skipblock ( void ) { 
if ( ! m_is ( (long)"{" ) ) { 
m_fail2 ( (long)"expected " , (long)"{" ) ; 
} 

long m_depth = 0 ; 
long m_going = 1 ; 
while ( m_going ) { 
if ( m_kind == 0 ) { 
m_fail ( (long)"unexpected end of file" ) ; 
} 

if ( m_is ( (long)"{" ) ) { 
m_depth = m_depth + 1 ; 
} 

if ( m_is ( (long)"}" ) ) { 
m_depth = m_depth - 1 ; 
} 

m_next ( ) ; 
m_going = m_depth != 0 ; 
} 

return 0 ; 
return 0;
} 
long m_declare ( void ) { 
while ( m_kind != 0 ) { 
if ( m_is ( (long)"var" ) ) { 
m_next ( ) ; 
m_out ( (long)"long " ) ; 
m_ident ( ) ; 
m_skip ( (long)";" ) ; 
m_out ( (long)";\n" ) ; 
} 
else if ( m_is ( (long)"fn" ) ) { 
m_next ( ) ; 
m_out ( (long)"long " ) ; 
m_ident ( ) ; 
m_skip ( (long)"(" ) ; 
if ( m_is ( (long)")" ) ) { 
m_out ( (long)"(void)" ) ; 
} 
else { 
m_out ( (long)"(long" ) ; 
m_skipident ( ) ; 
while ( m_is ( (long)"," ) ) { 
m_next ( ) ; 
m_out ( (long)", long" ) ; 
m_skipident ( ) ; 
} 

m_out ( (long)")" ) ; 
} 

m_skip ( (long)")" ) ; 
m_out ( (long)";\n" ) ; 
m_skipblock ( ) ; 
} 
else { 
m_fail ( (long)"expected 'fn' or 'var' at top level" ) ; 
} 


} 

return 0 ; 
return 0;
} 
long m_func ( void ) { 
m_skip ( (long)"fn" ) ; 
m_out ( (long)"long " ) ; 
m_ident ( ) ; 
m_take ( (long)"(" ) ; 
if ( m_is ( (long)")" ) ) { 
m_out ( (long)"void " ) ; 
} 
else { 
m_out ( (long)"long " ) ; 
m_ident ( ) ; 
while ( m_is ( (long)"," ) ) { 
m_take ( (long)"," ) ; 
m_out ( (long)"long " ) ; 
m_ident ( ) ; 
} 

} 

m_take ( (long)")" ) ; 
m_take ( (long)"{" ) ; 
m_putchar ( '\n' ) ; 
while ( ! m_is ( (long)"}" ) ) { 
m_stmt ( ) ; 
} 

m_out ( (long)"return 0;\n" ) ; 
m_take ( (long)"}" ) ; 
m_putchar ( '\n' ) ; 
return 0 ; 
return 0;
} 
long m_define ( void ) { 
while ( m_kind != 0 ) { 
if ( m_is ( (long)"var" ) ) { 
m_next ( ) ; 
m_next ( ) ; 
m_next ( ) ; 
} 
else { 
m_func ( ) ; 
} 

} 

return 0 ; 
return 0;
} 
long m_prelude ( void ) { 
m_out ( (long)"#include <stdio.h>\n" ) ; 
m_out ( (long)"#include <stdlib.h>\n" ) ; 
m_out ( (long)"long m_getchar(void) { return getchar(); }\n" ) ; 
m_out ( (long)"long m_putchar(long c) { putchar((int)c); return c; }\n" ) ; 
m_out ( (long)"long m_eputchar(long c) { fputc((int)c, stderr); return c; }\n" ) ; 
m_out ( (long)"long m_alloc(long n) { return (long)calloc(1, (size_t)n); }\n" ) ; 
m_out ( (long)"long m_load8(long p) { return *(unsigned char *)p; }\n" ) ; 
m_out ( (long)"long m_store8(long p, long v) { *(unsigned char *)p = (unsigned char)v; return v; }\n" ) ; 
m_out ( (long)"long m_exit(long c) { exit((int)c); return 0; }\n" ) ; 
return 0 ; 
return 0;
} 
long m_readall ( void ) { 
long m_cap = 4000000 ; 
long m_n = 0 ; 
m_src = m_alloc ( m_cap + 1 ) ; 
long m_c = m_getchar ( ) ; 
while ( m_c != - 1 ) { 
if ( m_n == m_cap ) { 
m_fail ( (long)"input too large" ) ; 
} 

m_store8 ( m_src + m_n , m_c ) ; 
m_n = m_n + 1 ; 
m_c = m_getchar ( ) ; 
} 

return 0 ; 
return 0;
} 
long m_main ( void ) { 
m_readall ( ) ; 
m_prelude ( ) ; 
m_restart ( ) ; 
m_declare ( ) ; 
m_restart ( ) ; 
m_define ( ) ; 
m_out ( (long)"int main(void) { return (int)m_main(); }\n" ) ; 
return 0 ; 
return 0;
} 
int main(void) { return (int)m_main(); }

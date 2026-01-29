%{
  #include "pager_filter.bison.h"

  /*Utility function, body is defined at pager_filter.c*/
  unsigned long xtol(const char *str);

  int line_num = 1;
  int ch_num = 0;
%}
%option noyywrap
%option nounput
%option noinput

%%
[ \t]                     ; // Skip whitespaces
\n                        { ++line_num;ch_num=0; } // Skip newline, just count

and|AND|\&\&              { ch_num += strlen(yytext); return AND;             }
or|OR|\|\|                { ch_num += strlen(yytext); return OR;              }
not|NOT|\!                { ch_num += strlen(yytext); return NOT;             }
has|HAS                   { ch_num += strlen(yytext); return HAS;             }
like|LIKE|=~              { ch_num += strlen(yytext); return LIKE;            }
in|IN                     { ch_num += strlen(yytext); return IN;              }
=>                        { ch_num += strlen(yytext); return RARROW;          }

cpu|CPU                   { ch_num += strlen(yytext); return PROP_CPU;        }
trace|TRACE               { ch_num += strlen(yytext); return PROP_TRACE;      }
func|FUNC                 { ch_num += strlen(yytext); return PROP_FUNC;       }
file|FILE                 { ch_num += strlen(yytext); return PROP_FILE;       }
fmt|FMT                   { ch_num += strlen(yytext); return PROP_FMT;        }
sev|SEV|severity|SEVERITY { ch_num += strlen(yytext); return PROP_SEV;        }
sticky|STICKY             { ch_num += strlen(yytext); return PROP_STICKY;     }
sticky_until|STICKY_UNTIL { ch_num += strlen(yytext); return PROP_STICKY_END; }

\@[0-9A-Z_\.]+      { // Token ID
  ch_num += strlen(yytext);
  yylval.sval = strdup(yytext);
  return TOKENID;
}

0x[0-9a-fA-F]+ { // Int literal
  ch_num += strlen(yytext);
  yylval.ival = xtol(yytext + 2);
  return INT;
}

[0-9]+         { // Int literal
  ch_num += strlen(yytext);
  yylval.ival = atol(yytext);
  return INT;
}

\"[^"]*\"         { // String literal
  int len = strlen(yytext);
  ch_num += len;
  yylval.sval = malloc(len - 1); // -2 quates + 1 NULL terminator
  memcpy(yylval.sval, yytext + 1, len - 2);
  yylval.sval[len - 2] = '\0';
  return STRING;
}

[0-9a-zA-Z_\*\.]+     { // Identifier (treated as a string)
  ch_num += strlen(yytext);
  yylval.sval = strdup(yytext);
  return STRING;
}

[\(\)\=\~\{\}\,\<\>\[\]\+]        { // Special symbols
  ch_num += strlen(yytext);
  return yytext[0];
}

. { ch_num += strlen(yytext); }     ;
%%
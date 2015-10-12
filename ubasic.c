/*
 * Copyright (c) 2006, Adam Dunkels
 * All rights reserved.
 *
 * Copyright (c) 2015, Alan Cox
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#define DEBUG 0

#if DEBUG
#define DEBUG_PRINTF(...)  printf(__VA_ARGS__)
#else
#define DEBUG_PRINTF(...)
#endif

#include <stdio.h> /* printf() */
#include <stdlib.h> /* exit() */
#include <stdint.h> /* Types */
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>

#include "ubasic.h"
#include "tokenizer.h"

static char const *program_ptr;

#define MAX_GOSUB_STACK_DEPTH 10
static line_t gosub_stack[MAX_GOSUB_STACK_DEPTH];
static int gosub_stack_ptr;

struct for_state {
  line_t line_after_for;
  var_t for_variable;
  value_t to;
  value_t step;
};

#define MAX_FOR_STACK_DEPTH 4
static struct for_state for_stack[MAX_FOR_STACK_DEPTH];
static int for_stack_ptr;

struct line_index {
  line_t line_number;
  char const *program_text_position;
  struct line_index *next;
};
struct line_index *line_index_head = NULL;
struct line_index *line_index_current = NULL;

#define MAX_VARNUM 26 * 11
static value_t variables[MAX_VARNUM];
#define MAX_STRING 26
static uint8_t *strings[MAX_STRING];
static uint8_t nullstr[1] = { 0 };

static int ended;

static void expr(struct typevalue *val);
static void line_statement(void);
static void statement(void);
static void index_free(void);

peek_func peek_function = NULL;
poke_func poke_function = NULL;

line_t line_num;
static const char *data_position;
static int data_seek;

static unsigned int array_base = 0;

/*---------------------------------------------------------------------------*/
void ubasic_init(const char *program)
{
  int i;
  program_ptr = program;
  for_stack_ptr = gosub_stack_ptr = 0;
  index_free();
  tokenizer_init(program);
  data_position = program_ptr;
  data_seek = 1;
  ended = 0;
  for (i = 0; i < 26; i++)
    strings[i] = nullstr;
}
/*---------------------------------------------------------------------------*/
void ubasic_init_peek_poke(const char *program, peek_func peek, poke_func poke)
{
  peek_function = peek;
  poke_function = poke;
  ubasic_init(program);
}
/*---------------------------------------------------------------------------*/
void ubasic_error(const char *err)
{
  if (line_num)
    fprintf(stderr, "Line %d: ", line_num);
  fprintf(stderr, "%s error.\n", err);
  exit(1);
}
static const char syntax[] = { "Syntax" };
static const char badtype[] = { "Type mismatch" };
static const char divzero[] = { "Division by zero" };
static const char outofmemory[] = { "Out of memory" };

/* Call back from the tokenizer on error */
void ubasic_tokenizer_error(void)
{
  ubasic_error(syntax);
}

/*---------------------------------------------------------------------------*/
static uint8_t accept_tok(uint8_t token)
{
  if(token != tokenizer_token()) {
    DEBUG_PRINTF("Token not what was expected (expected %d, got %d)\n",
                token, tokenizer_token());
    tokenizer_error_print();
    exit(1);
  }
  DEBUG_PRINTF("Expected %d, got it\n", token);
  tokenizer_next();
  /* This saves lots of extra calls - return the new token */
  return tokenizer_token();
}
/*---------------------------------------------------------------------------*/
static uint8_t accept_either(uint8_t tok1, uint8_t tok2)
{
  uint8_t t = tokenizer_token();
  if (t == tok2)
    accept_tok(tok2);
  else
    accept_tok(tok1);
  return t;
}

static void bracketed_expr(struct typevalue *v)
{
  accept_tok(TOKENIZER_LEFTPAREN);
  expr(v);
  accept_tok(TOKENIZER_RIGHTPAREN);
}

/*---------------------------------------------------------------------------*/
static void typecheck_int(struct typevalue *v)
{
  if (v->type != TYPE_INTEGER)
    ubasic_error(badtype);
}
/*---------------------------------------------------------------------------*/
static void typecheck_string(struct typevalue *v)
{
  if (v->type != TYPE_STRING)
    ubasic_error(badtype);
}
/*---------------------------------------------------------------------------*/
static void typecheck_same(struct typevalue *l, struct typevalue *r)
{
  if (l->type != r->type)
    ubasic_error(badtype);
}
/*---------------------------------------------------------------------------*/
/* Temoporary implementation of string workspaces */

static uint8_t stringblob[512];
static uint8_t *nextstr;

static uint8_t *string_temp(int len)
{
  uint8_t *p = nextstr;
  if (len > 255)
    ubasic_error("String too long");
  nextstr += len + 1;
  if (nextstr > stringblob + sizeof(stringblob))
    ubasic_error("Out of temporary space");
  *p = len;
  return p;
}
/*---------------------------------------------------------------------------*/
static void string_temp_free(void)
{
  nextstr = stringblob;
}
/*---------------------------------------------------------------------------*/
static void string_cut(struct typevalue *o, struct typevalue *t, value_t l, value_t n)
{
  uint8_t *p = t->d.p;
  int f = *p;
  /* Strings start at 1 ... */
  
  if (l > f)	/* Nothing to cut */
    o->d.p = string_temp(0);
  else {
    f -= l - 1;
    if (f < n)
      n = f;
    o->d.p = string_temp(n);
    memcpy(o->d.p+1, p + l, n);
  }
  o->type = TYPE_STRING;
}  
/*---------------------------------------------------------------------------*/
static void string_cut_r(struct typevalue *o, struct typevalue *t, value_t r)
{
  int f = *t->d.p;
  f -= r;
  if (f <= 0) {
    o->d.p = string_temp(0);
    o->type = TYPE_STRING;
  } else
    string_cut(o, t, f + 1, r);
}
/*---------------------------------------------------------------------------*/
static value_t string_val(struct typevalue *t)
{
  uint8_t *p = t->d.p;
  uint8_t l = *p++;
  uint8_t neg = 0;
  value_t n = 0;
  if (*p == '-') {
    neg = 1;
    p++;
    l--;
  }
  if (l == 0)
    ubasic_error(badtype);
  while(l) {
    if (!isdigit(*p))
      ubasic_error(badtype);
    n = 10 * n + *p++ - '0';
    l--;
  }
  return neg ? -n : n;
}
/*---------------------------------------------------------------------------*/
static value_t bracketed_intexpr(void)
{
  struct typevalue v;
  bracketed_expr(&v);
  typecheck_int(&v);
  return v.d.i;
}
/*---------------------------------------------------------------------------*/
static void funcexpr(struct typevalue *t, const char *f)
{
  accept_tok(TOKENIZER_LEFTPAREN);
  while(*f) {
    expr(t);
    if (*f != t->type)
      ubasic_error(badtype);
    if (*++f)
      accept_tok(TOKENIZER_COMMA);
    t++;
  }
  accept_tok(TOKENIZER_RIGHTPAREN);
}
/*---------------------------------------------------------------------------*/
static void varfactor(struct typevalue *v)
{
  ubasic_get_variable(tokenizer_variable_num(), v);
  DEBUG_PRINTF("varfactor: obtaining %d from variable %d\n", v->d.i, tokenizer_variable_num());
  accept_either(TOKENIZER_INTVAR, TOKENIZER_STRINGVAR);
}
/*---------------------------------------------------------------------------*/
static void factor(struct typevalue *v)
{
  uint8_t t = tokenizer_token();
  int len;
  struct typevalue arg[3];

  DEBUG_PRINTF("factor: token %d\n", tokenizer_token());
  switch(t) {
  case TOKENIZER_STRING:
    v->type = TYPE_STRING;
    len = tokenizer_string_len();
    v->d.p = string_temp(len);
    memcpy(v->d.p + 1, tokenizer_string(), len);
    DEBUG_PRINTF("factor: string %p\n", v->d.p);
    accept_tok(TOKENIZER_STRING);
    break;
  case TOKENIZER_NUMBER:
    v->d.i = tokenizer_num();
    v->type = TYPE_INTEGER;
    DEBUG_PRINTF("factor: number %d\n", v->d.i);
    accept_tok(TOKENIZER_NUMBER);
    break;
  case TOKENIZER_LEFTPAREN:
    accept_tok(TOKENIZER_LEFTPAREN);
    expr(v);
    accept_tok(TOKENIZER_RIGHTPAREN);
    break;
  case TOKENIZER_INTVAR:
  case TOKENIZER_STRINGVAR:
    varfactor(v);
    break;
  default:
    if (TOKENIZER_NUMEXP(t)) {
      accept_tok(t);
      switch(t) {
      case TOKENIZER_PEEK:
        funcexpr(arg,"I");
        v->d.i = peek_function(arg[0].d.i);
        break;
      case TOKENIZER_ABS:
        funcexpr(arg,"I");
        v->d.i = arg[0].d.i;
        if (v->d.i < 0)
          v->d.i = -v->d.i;
        break;
      case TOKENIZER_INT:
        funcexpr(arg,"I");
        v->d.i = arg[0].d.i;
        break;
      case TOKENIZER_SGN:
        funcexpr(arg,"I");
        v->d.i = arg[0].d.i;
        if (v->d.i > 1 ) v->d.i = 1;
        if (v->d.i < 0) v->d.i = -1;
        break;
      case TOKENIZER_LEN:
        funcexpr(arg,"S");
        v->d.i = *arg[0].d.p;
        break;
      case TOKENIZER_CODE:
        funcexpr(arg,"S");
        if (*arg[0].d.p)
          v->d.i = arg[0].d.p[1];
        else
          v->d.i = 0;
        break;
      case TOKENIZER_VAL:
        funcexpr(arg,"S");
        v->d.i = string_val(&arg[0]);
        break;
      default:
        ubasic_error(syntax);
      }
      v->type = TYPE_INTEGER;
    }
    else if (TOKENIZER_STRINGEXP(t)) {
      accept_tok(t);
      switch(t) {
      case TOKENIZER_LEFTSTR:
        funcexpr(arg, "SI");
        string_cut(v, &arg[0], 1, arg[1].d.i);
        break;
      case TOKENIZER_RIGHTSTR:
        funcexpr(arg, "SI");
        string_cut_r(v, &arg[0], arg[1].d.i);
        break;
      case TOKENIZER_MIDSTR:
        funcexpr(arg, "SII");
        string_cut(v, &arg[0], arg[1].d.i, arg[2].d.i);
        break;
      case TOKENIZER_CHRSTR:
        funcexpr(arg, "I");
        v->d.p = string_temp(2);
        v->d.p[1] = arg[0].d.i;
        v->type = TYPE_STRING;
        break;
      default:
        ubasic_error(syntax);
      }
    }
    else
      ubasic_error(syntax);
  }
}

/*---------------------------------------------------------------------------*/
static void term(struct typevalue *v)
{
  struct typevalue f2;
  int op;

  factor(v);
  op = tokenizer_token();
  DEBUG_PRINTF("term: token %d\n", op);
  while(op == TOKENIZER_ASTR ||
       op == TOKENIZER_SLASH ||
       op == TOKENIZER_MOD) {
    tokenizer_next();
    factor(&f2);
    typecheck_int(v);
    typecheck_int(&f2);
    DEBUG_PRINTF("term: %d %d %d\n", v->d.i, op, f2.d.i);
    switch(op) {
    case TOKENIZER_ASTR:
      v->d.i *= f2.d.i;
      break;
    case TOKENIZER_SLASH:
      if (f2.d.i == 0)
        ubasic_error(divzero);
      v->d.i /= f2.d.i;
      break;
    case TOKENIZER_MOD:
      if (f2.d.i == 0)
        ubasic_error(divzero);
      v->d.i %= f2.d.i;
      break;
    }
    op = tokenizer_token();
  }
  DEBUG_PRINTF("term: %d\n", v->d.i);
}
/*---------------------------------------------------------------------------*/
static void expr(struct typevalue *v)
{
  struct typevalue t2;
  int op;

  term(v);
  op = tokenizer_token();
  DEBUG_PRINTF("expr: token %d\n", op);
  while(op == TOKENIZER_PLUS ||
       op == TOKENIZER_MINUS ||
       op == TOKENIZER_AND ||
       op == TOKENIZER_OR) {
    tokenizer_next();
    term(&t2);
    if (op != TOKENIZER_PLUS)
      typecheck_int(v);
    typecheck_same(v, &t2);
    DEBUG_PRINTF("expr: %d %d %d\n", v->d.i, op, t2.d.i);
    switch(op) {
    case TOKENIZER_PLUS:
      if (v->type == TYPE_INTEGER)
        v->d.i += t2.d.i;
      else {
        uint8_t *p;
        uint8_t l = *v->d.p;
        p = string_temp(l + *t2.d.p);
        memcpy(p + 1, v->d.p + 1, l);
        memcpy(p + l + 1, t2.d.p + 1, *t2.d.p);
        v->d.p = p;
      }
      break;
    case TOKENIZER_MINUS:
      v->d.i -= t2.d.i;
      break;
    case TOKENIZER_AND:
      v->d.i &= t2.d.i;
      break;
    case TOKENIZER_OR:
      v->d.i |= t2.d.i;
      break;
    }
    op = tokenizer_token();
  }
  DEBUG_PRINTF("expr: %d\n", v->d.i);
}
/*---------------------------------------------------------------------------*/
static void relation(struct typevalue *r1)
{
  struct typevalue r2;
  int op;

  expr(r1);
  op = tokenizer_token();
  DEBUG_PRINTF("relation: token %d\n", op);
  /* FIXME: unclear the while is correct here. It's not correct in most
     BASIC to write  A > B > C, rather relations should be two part linked
     with logic */
  while(op == TOKENIZER_LT ||
       op == TOKENIZER_GT ||
       op == TOKENIZER_EQ ||
       op == TOKENIZER_NE ||
       op == TOKENIZER_LE ||
       op == TOKENIZER_GE) {
    tokenizer_next();
    expr(&r2);
    typecheck_same(r1, &r2);
    DEBUG_PRINTF("relation: %d %d %d\n", r1->d.i, op, r2.d.i);
    if (r1->type == TYPE_INTEGER) {
      switch(op) {
      case TOKENIZER_LT:
        r1->d.i = r1->d.i < r2.d.i;
        break;
      case TOKENIZER_GT:
        r1->d.i = r1->d.i > r2.d.i;
        break;
      case TOKENIZER_EQ:
        r1->d.i = r1->d.i == r2.d.i;
        break;
      case TOKENIZER_LE:
        r1->d.i = r1->d.i <= r2.d.i;
        break;
      case TOKENIZER_GE:
        r1->d.i = r1->d.i >= r2.d.i;
        break;
      case TOKENIZER_NE:
        r1->d.i = r1->d.i != r2.d.i;
        break;
      }
    } else {
      int n =*r1->d.p;
      if (*r2.d.p < n)
        n = *r2.d.p;
      n = memcmp(r1->d.p + 1, r2.d.p + 1, n);
      if (n == 0) {
        if (*r1->d.p > *r2.d.p)
          n = 1;
        else if (*r1->d.p < *r2.d.p)
          n = -1;
      }
      switch(op) {
        case TOKENIZER_LT:
          n = (n == -1);
          break;
        case TOKENIZER_GT:
          n = (n == 1);
          break;
        case TOKENIZER_EQ:
          n = (n == 0);
          break;
        case TOKENIZER_LE:
          n = (n != 1);
          break;
        case TOKENIZER_GE:
          n = (n != -1);
          break;
        case TOKENIZER_NE:
          n = (n != 0);
          break;
      }
      r1->d.i = n;
    }
    op = tokenizer_token();
  }
  r1->type = TYPE_INTEGER;
}
/*---------------------------------------------------------------------------*/
static value_t intexpr(void)
{
  struct typevalue t;
  expr(&t);
  typecheck_int(&t);
  return t.d.i;
}
/*---------------------------------------------------------------------------*/
static uint8_t *stringexpr(void)
{
  struct typevalue t;
  expr(&t);
  typecheck_string(&t);
  return t.d.p;
}
/*---------------------------------------------------------------------------*/
static void index_free(void) {
  if(line_index_head != NULL) {
    line_index_current = line_index_head;
    do {
      DEBUG_PRINTF("Freeing index for line %p.\n", (void *)line_index_current);
      line_index_head = line_index_current;
      line_index_current = line_index_current->next;
      free(line_index_head);
    } while (line_index_current != NULL);
    line_index_head = NULL;
  }
}
/*---------------------------------------------------------------------------*/
static char const*index_find(int linenum) {
  #if DEBUG
  int step = 0;
  #endif
  struct line_index *lidx;
  lidx = line_index_head;


  while(lidx != NULL && lidx->line_number != linenum) {
    lidx = lidx->next;

    #if DEBUG
    if(lidx != NULL) {
      DEBUG_PRINTF("index_find: Step %3d. Found index for line %d: %p.\n",
                   step, lidx->line_number,
                   lidx->program_text_position);
    }
    step++;
    #endif
  }
  if(lidx != NULL && lidx->line_number == linenum) {
    DEBUG_PRINTF("index_find: Returning index for line %d.\n", linenum);
    return lidx->program_text_position;
  }
  DEBUG_PRINTF("index_find: Returning NULL.\n");
  return NULL;
}
/*---------------------------------------------------------------------------*/
static void index_add(int linenum, char const* sourcepos) {
  struct line_index *new_lidx;

  if(line_index_head != NULL && index_find(linenum)) {
    return;
  }

  new_lidx = malloc(sizeof(struct line_index));
  new_lidx->line_number = linenum;
  new_lidx->program_text_position = sourcepos;
  new_lidx->next = NULL;

  if(line_index_head != NULL) {
    line_index_current->next = new_lidx;
    line_index_current = line_index_current->next;
  } else {
    line_index_current = new_lidx;
    line_index_head = line_index_current;
  }
  DEBUG_PRINTF("index_add: Adding index for line %d: %p.\n", linenum,
               sourcepos);
}
/*---------------------------------------------------------------------------*/
static void
jump_linenum_slow(int linenum)
{
  tokenizer_init(program_ptr);
  while(tokenizer_num() != linenum) {
    do {
      do {
        tokenizer_next();
      } while(tokenizer_token() != TOKENIZER_CR &&
          tokenizer_token() != TOKENIZER_ENDOFINPUT);
      if(tokenizer_token() == TOKENIZER_CR) {
        tokenizer_next();
      }
    } while(tokenizer_token() != TOKENIZER_NUMBER);
    DEBUG_PRINTF("jump_linenum_slow: Found line %d\n", tokenizer_num());
  }
}
/*---------------------------------------------------------------------------*/
static void
jump_linenum(int linenum)
{
  char const* pos = index_find(linenum);
  if(pos != NULL) {
    DEBUG_PRINTF("jump_linenum: Going to line %d.\n", linenum);
    tokenizer_goto(pos);
  } else {
    /* We'll try to find a yet-unindexed line to jump to. */
    DEBUG_PRINTF("jump_linenum: Calling jump_linenum_slow %d.\n", linenum);
    jump_linenum_slow(linenum);
  }
}
/*---------------------------------------------------------------------------*/
static void go_statement(void)
{
  int linenum;
  uint8_t t;

  accept_tok(TOKENIZER_GO);
  t = accept_either(TOKENIZER_TO, TOKENIZER_SUB);
  if (t == TOKENIZER_TO) {
    linenum = intexpr();
    accept_tok(TOKENIZER_CR);
    jump_linenum(linenum);
    return;
  }
  linenum = intexpr();
  accept_tok(TOKENIZER_CR);
  if(gosub_stack_ptr < MAX_GOSUB_STACK_DEPTH) {
    gosub_stack[gosub_stack_ptr] = tokenizer_num();
    gosub_stack_ptr++;
    jump_linenum(linenum);
  } else {
    DEBUG_PRINTF("gosub_statement: gosub stack exhausted\n");
    ubasic_error("Return without gosub");
  }
}
/*---------------------------------------------------------------------------*/

static int chpos = 0;

static void charout(char c, void *unused)
{
  if (c == '\t') {
    do {
      charout(' ', NULL);
    } while(chpos%8);
    return;
  }
  putchar(c);
  if ((c == 8 || c== 127) && chpos)
    chpos--;
  else if (c == '\r' || c == '\n')
    chpos = 0;
  else
    chpos++;
}

static void charreset(void)
{
  chpos = 0;
}

static void chartab(value_t v)
{
  while(chpos < v)
    charout(' ', NULL);
}

static void charoutstr(uint8_t *p)
{
  int len =*p++;
  while(len--)
    charout(*p++, NULL);
}

static void print_statement(void)
{
  uint8_t nonl;
  uint8_t t;

  accept_tok(TOKENIZER_PRINT);
  do {
    t = tokenizer_token();
    nonl = 0;
    DEBUG_PRINTF("Print loop\n");
    if(t == TOKENIZER_STRING) {
      /* Handle string const specially - length rules */
      tokenizer_string_func(charout, NULL);
      tokenizer_next();
    } else if(TOKENIZER_STRINGEXP(t)) {
      charoutstr(stringexpr());
    } else if(t == TOKENIZER_COMMA) {
      printf("\t");
      nonl = 1;
      tokenizer_next();
    } else if(t == TOKENIZER_SEMICOLON) {
      nonl = 1;
      tokenizer_next();
    } else if(TOKENIZER_NUMEXP(t)) {
      printf("%d", intexpr());
    } else if(t == TOKENIZER_TAB) {
      accept_tok(TOKENIZER_TAB);
      chartab(bracketed_intexpr());
    } else if (t != TOKENIZER_CR) {
      ubasic_error(syntax);
      break;
    }
  } while(t != TOKENIZER_CR &&
      t != TOKENIZER_ENDOFINPUT);
  if (!nonl)
    printf("\n");
  DEBUG_PRINTF("End of print\n");
  tokenizer_next();
}
/*---------------------------------------------------------------------------*/
static void if_statement(void)
{
  struct typevalue r;

  accept_tok(TOKENIZER_IF);

  relation(&r);
  DEBUG_PRINTF("if_statement: relation %d\n", r.d.i);
  accept_tok(TOKENIZER_THEN);
  if(r.d.i) {
    statement();
  } else {
    do {
      tokenizer_next();
    } while(tokenizer_token() != TOKENIZER_ELSE &&
        tokenizer_token() != TOKENIZER_CR &&
        tokenizer_token() != TOKENIZER_ENDOFINPUT);
    if(tokenizer_token() == TOKENIZER_ELSE) {
      tokenizer_next();
      statement();
    } else if(tokenizer_token() == TOKENIZER_CR) {
      tokenizer_next();
    }
  }
}
/*---------------------------------------------------------------------------*/
static void let_statement(void)
{
  var_t var;
  struct typevalue v;

  var = tokenizer_variable_num();

  accept_either(TOKENIZER_INTVAR, TOKENIZER_STRINGVAR);
  accept_tok(TOKENIZER_EQ);
  expr(&v);
  DEBUG_PRINTF("let_statement: assign %d to %d\n", var, v.d.i);
  ubasic_set_variable(var, &v);
  accept_tok(TOKENIZER_CR);

}
/*---------------------------------------------------------------------------*/
static void return_statement(void)
{
  accept_tok(TOKENIZER_RETURN);
  if(gosub_stack_ptr > 0) {
    gosub_stack_ptr--;
    jump_linenum(gosub_stack[gosub_stack_ptr]);
  } else {
    DEBUG_PRINTF("return_statement: non-matching return\n");
  }
}
/*---------------------------------------------------------------------------*/
static void next_statement(void)
{
  int var;
  struct for_state *fs;
  struct typevalue t;

  /* FIXME: support 'NEXT' on its own, also loop down the stack so if you
     GOTO out of a layer of NEXT the right thing occurs */
  accept_tok(TOKENIZER_NEXT);
  var = tokenizer_variable_num();
  accept_tok(TOKENIZER_INTVAR);
  
  /* FIXME: make the for stack just use pointers so it compiles better */
  fs = &for_stack[for_stack_ptr - 1];
  if(for_stack_ptr > 0 &&
     var == fs->for_variable) {
    ubasic_get_variable(var, &t);
    t.d.i += fs->step;
    ubasic_set_variable(var, &t);
    /* NEXT end depends upon sign of STEP */
    if ((fs->step >= 0 && t.d.i <= fs->to) ||
        (fs->step < 0 && t.d.i >= fs->to)) {
      jump_linenum(fs->line_after_for);
    } else {
      for_stack_ptr--;
      accept_tok(TOKENIZER_CR);
    }
  } else
    ubasic_error("Mismatched NEXT");
}
/*---------------------------------------------------------------------------*/
static void for_statement(void)
{
  var_t for_variable;
  value_t to, step = 1;
  struct typevalue t;

  accept_tok(TOKENIZER_FOR);
  for_variable = tokenizer_variable_num();
  accept_tok(TOKENIZER_INTVAR);
  accept_tok(TOKENIZER_EQ);
  expr(&t);
  typecheck_int(&t);
  /* The set also typechecks the variable */
  ubasic_set_variable(for_variable, &t);
  accept_tok(TOKENIZER_TO);
  to = intexpr();
  if (tokenizer_token() == TOKENIZER_STEP) {
    accept_tok(TOKENIZER_STEP);
    step = intexpr();
  }
  accept_tok(TOKENIZER_CR);

  if(for_stack_ptr < MAX_FOR_STACK_DEPTH) {
    struct for_state *fs = &for_stack[for_stack_ptr];
    fs->line_after_for = tokenizer_num();
    fs->for_variable = for_variable;
    fs->to = to;
    fs->step = step;
    DEBUG_PRINTF("for_statement: new for, var %d to %d step %d\n",
                fs->for_variable,
                fs->to,
                fs->step);

    for_stack_ptr++;
  } else {
    DEBUG_PRINTF("for_statement: for stack depth exceeded\n");
  }
}
/*---------------------------------------------------------------------------*/
static void poke_statement(void)
{
  value_t poke_addr;
  value_t value;

  accept_tok(TOKENIZER_POKE);
  poke_addr = intexpr();
  accept_tok(TOKENIZER_COMMA);
  value = intexpr();
  accept_tok(TOKENIZER_CR);

  poke_function(poke_addr, value);
}
/*---------------------------------------------------------------------------*/
static void stop_statement(void)
{
  accept_tok(TOKENIZER_STOP);
  accept_tok(TOKENIZER_CR);
  ended = 1;
}
/*---------------------------------------------------------------------------*/
static void rem_statement(void)
{
  accept_tok(TOKENIZER_REM);
  tokenizer_newline();
}

/*---------------------------------------------------------------------------*/
static void data_statement(void)
{
  uint8_t t;
  accept_tok(TOKENIZER_DATA);
  do {
    t = tokenizer_token();
    /* We could just as easily allow expressions which might be wild... */
    /* Some platforms allow 4,,5  ... we don't yet FIXME */
    if (t == TOKENIZER_STRING || t == TOKENIZER_NUMBER)
      tokenizer_next();
    else
      ubasic_error(syntax);
    t = accept_either(TOKENIZER_CR, TOKENIZER_COMMA);
  } while(t != TOKENIZER_CR);
}

/*---------------------------------------------------------------------------*/
static void randomize_statement(void)
{
  value_t r = 0;
  accept_tok(TOKENIZER_RANDOMIZE);
  /* FIXME: replace all the CR checks with TOKENIZER_EOS() or similar so we
     can deal with ':' */
  if (tokenizer_token() != TOKENIZER_CR)
    r = intexpr();
  if (r)
    srand(getpid()^getuid()^time(NULL));
  else
    srand(r);
  accept_tok(TOKENIZER_CR);
}

/*---------------------------------------------------------------------------*/

static void option_statement(void)
{
  value_t r;
  accept_tok(TOKENIZER_OPTION);
  accept_tok(TOKENIZER_BASE);
  r = intexpr();
  accept_tok(TOKENIZER_CR);
  if (r < 0 || r > 1)
    ubasic_error("Invalid base");
  array_base = r;
}

/*---------------------------------------------------------------------------*/

static void input_statement(void)
{
  struct typevalue r;
  var_t v;
  char buf[129];
  uint8_t t;
  int l;
  
  accept_tok(TOKENIZER_INPUT);

  t = tokenizer_token();
  if (TOKENIZER_STRINGEXP(t)) {
    tokenizer_string_func(charout, NULL);
    tokenizer_next();
    t = tokenizer_token();
    if (t == TOKENIZER_COMMA)
      accept_tok(TOKENIZER_COMMA);
    else
      accept_tok(TOKENIZER_SEMICOLON);	/* accept_tok_pair needed  ? */
  } else {
    charout('?', NULL);
    charout(' ', NULL);
  }

  /* Consider the single var allowed version of INPUT - it's saner for
     strings by far ? */
  do {  
    t = tokenizer_token();
    v = tokenizer_variable_num();
    accept_either(TOKENIZER_INTVAR, TOKENIZER_STRINGVAR);

    if (fgets(buf + 1, 128, stdin) == NULL) {
      fprintf(stderr, "EOF\n");
      exit(1);
    }
    charreset();		/* Newline input so move to left */
    if (t == TOKENIZER_INTVAR) {
      r.type = TYPE_INTEGER;	/* For now */
      r.d.i = atoi(buf + 1);	/* FIXME: error checking */
    } else {
      /* Turn a C string into a BASIC one */
      r.type = TYPE_STRING;
      l = strlen(buf + 1);
      if (buf[l] == '\n')
        l--;
      *((uint8_t *)buf) = l;
      r.d.p = (uint8_t *)buf;
    }
    ubasic_set_variable(v, &r);
    t = tokenizer_token();
    if (t != TOKENIZER_CR)
      t = accept_either(TOKENIZER_COMMA, TOKENIZER_SEMICOLON);
  } while(t != TOKENIZER_CR);
  accept_tok(TOKENIZER_CR);
}

/*---------------------------------------------------------------------------*/
void restore_statement(void)
{
  int linenum = 0;
  uint8_t t;
  t = accept_tok(TOKENIZER_RESTORE);
  if (t != TOKENIZER_CR)
    linenum = intexpr();
  accept_tok(TOKENIZER_CR);
  if (linenum) {
    tokenizer_push();
    jump_linenum(linenum);
    data_position = tokenizer_pos();
    tokenizer_pop();
  } else
    data_position = program_ptr;
  data_seek = 1;
}

/*---------------------------------------------------------------------------*/
static void statement(void)
{
  int token;

  string_temp_free();

  token = tokenizer_token();

  switch(token) {
  case TOKENIZER_PRINT:
    print_statement();
    break;
  case TOKENIZER_IF:
    if_statement();
    break;
  case TOKENIZER_GO:
    go_statement();
    break;
  case TOKENIZER_RETURN:
    return_statement();
    break;
  case TOKENIZER_FOR:
    for_statement();
    break;
  case TOKENIZER_POKE:
    poke_statement();
    break;
  case TOKENIZER_NEXT:
    next_statement();
    break;
  case TOKENIZER_STOP:
    stop_statement();
    break;
  case TOKENIZER_REM:
    rem_statement();
    break;
  case TOKENIZER_DATA:
    data_statement();
    break;
  case TOKENIZER_RANDOMIZE:
    randomize_statement();
    break;
  case TOKENIZER_OPTION:
    option_statement();
    break;
  case TOKENIZER_INPUT:
    input_statement();
    break;
  case TOKENIZER_RESTORE:
    restore_statement();
    break;
  case TOKENIZER_LET:
    accept_tok(TOKENIZER_LET);
    /* Fall through. */
  case TOKENIZER_STRINGVAR:
  case TOKENIZER_INTVAR:
    let_statement();
    break;
  default:
    DEBUG_PRINTF("ubasic.c: statement(): not implemented %d\n", token);
    ubasic_error(syntax);
  }
}
/*---------------------------------------------------------------------------*/
static void line_statement(void)
{
  line_num = tokenizer_num();
  DEBUG_PRINTF("----------- Line number %d ---------\n", line_num);
  index_add(line_num, tokenizer_pos());
  accept_tok(TOKENIZER_NUMBER);
  statement();
  return;
}
/*---------------------------------------------------------------------------*/
void ubasic_run(void)
{
  if(tokenizer_finished()) {
    DEBUG_PRINTF("uBASIC program finished\n");
    return;
  }

  line_statement();
}
/*---------------------------------------------------------------------------*/
int ubasic_finished(void)
{
  return ended || tokenizer_finished();
}
/*---------------------------------------------------------------------------*/

/* This helper will change once we try and stamp out malloc but will do for
   the moment */
static uint8_t *string_save(uint8_t *p)
{
  uint8_t *b = malloc(*p + 1);
  if (b == NULL)
    ubasic_error(outofmemory);
  memcpy(b, p, *p + 1);
  return b;
}

void ubasic_set_variable(int varnum, struct typevalue *value)
{
  if (varnum & STRINGFLAG) {
    typecheck_string(value);
    varnum &= ~STRINGFLAG;
    if (strings[varnum] != nullstr)
      free(strings[varnum]);
    strings[varnum] = string_save(value->d.p);
  } else {
    typecheck_int(value);
    if(varnum >= 0 && varnum <= MAX_VARNUM)
      variables[varnum] = value->d.i;
    else {
      ubasic_error("badsw");
      exit(1);
    }
  }
}
/*---------------------------------------------------------------------------*/
void ubasic_get_variable(int varnum, struct typevalue *value)
{
  if (varnum & STRINGFLAG) {
    value->d.p = strings[varnum & ~STRINGFLAG];
    value->type = TYPE_STRING;
  } else if(varnum >= 0 && varnum <= MAX_VARNUM) {
    value->d.i = variables[varnum];
    value->type = TYPE_INTEGER;
  } else
    ubasic_error("badv");
}
/*---------------------------------------------------------------------------*/

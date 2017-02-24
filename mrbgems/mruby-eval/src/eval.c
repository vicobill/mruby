#include <mruby.h>
#include <mruby/class.h>
#include <mruby/compile.h>
#include <mruby/irep.h>
#include <mruby/proc.h>
#include <mruby/opcode.h>
#include <mruby/variable.h>

mrb_value mrb_exec_irep(mrb_state *mrb, mrb_value self, struct RProc *p);
mrb_value mrb_obj_instance_eval(mrb_state *mrb, mrb_value self);

static struct mrb_irep *
get_closure_irep(mrb_state *mrb, int level, struct RProc *scope)
{
  struct mrb_context *c = mrb->c;
  struct REnv *e = scope->env;
  struct RProc *proc;
  if (level == 0) {
    proc = scope;
    if (MRB_PROC_CFUNC_P(proc)) {
      return NULL;
    }
    return proc->body.irep;
  }

  while (--level) {
    e = (struct REnv*)e->c;
    if (!e) return NULL;
  }

  if (!e) return NULL;
  if (!MRB_ENV_STACK_SHARED_P(e)) return NULL;

  proc = c->cibase[e->cioff].proc;

  if (!proc || MRB_PROC_CFUNC_P(proc)) {
    return NULL;
  }
  return proc->body.irep;
}

static inline mrb_code
search_variable(mrb_state *mrb, mrb_sym vsym, int bnest, struct RProc *scope)
{
  mrb_irep *virep;
  int level;
  int pos;

  for (level = 0; (virep = get_closure_irep(mrb, level, scope)); level++) {
    if (!virep || virep->lv == NULL) {
      continue;
    }
    for (pos = 0; pos < virep->nlocals - 1; pos++) {
      if (vsym == virep->lv[pos].name) {
        return (MKARG_B(pos + 1) | MKARG_C(level + bnest));
      }
    }
  }

  return 0;
}

static mrb_bool
potential_upvar_p(struct mrb_locals *lv, uint16_t v, int argc, uint16_t nlocals)
{
  if (v >= nlocals) return FALSE;
  /* skip arguments  */
  if (v < argc+1) return FALSE;
  return TRUE;
}

static void
patch_irep(mrb_state *mrb, mrb_irep *irep, int bnest, struct RProc *scope)
{
  size_t i;
  mrb_code c;
  int argc = 0;

  for (i = 0; i < irep->ilen; i++) {
    c = irep->iseq[i];
    switch(GET_OPCODE(c)){
    case OP_ENTER:
      {
        mrb_aspec ax = GETARG_Ax(c);
        /* extra 1 means a slot for block */
        argc = MRB_ASPEC_REQ(ax)+MRB_ASPEC_OPT(ax)+MRB_ASPEC_REST(ax)+MRB_ASPEC_POST(ax)+1;
      }
      break;

    case OP_EPUSH:
      patch_irep(mrb, irep->reps[GETARG_Bx(c)], bnest + 1, scope);
      break;

    case OP_LAMBDA:
      {
        int arg_c = GETARG_c(c);
        if (arg_c & OP_L_CAPTURE) {
          patch_irep(mrb, irep->reps[GETARG_b(c)], bnest + 1, scope);
        }
      }
      break;

    case OP_SEND:
      if (GETARG_C(c) != 0) {
        break;
      }
      {
        mrb_code arg = search_variable(mrb, irep->syms[GETARG_B(c)], bnest, scope);
        if (arg != 0) {
          /* must replace */
          irep->iseq[i] = MKOPCODE(OP_GETUPVAR) | MKARG_A(GETARG_A(c)) | arg;
        }
      }
      break;

    case OP_MOVE:
      /* src part */
      if (potential_upvar_p(irep->lv, GETARG_B(c), argc, irep->nlocals)) {
        mrb_code arg = search_variable(mrb, irep->lv[GETARG_B(c) - 1].name, bnest, scope);
        if (arg != 0) {
          /* must replace */
          irep->iseq[i] = MKOPCODE(OP_GETUPVAR) | MKARG_A(GETARG_A(c)) | arg;
        }
      }
      /* dst part */
      if (potential_upvar_p(irep->lv, GETARG_A(c), argc, irep->nlocals)) {
        mrb_code arg = search_variable(mrb, irep->lv[GETARG_A(c) - 1].name, bnest, scope);
        if (arg != 0) {
          /* must replace */
          irep->iseq[i] = MKOPCODE(OP_SETUPVAR) | MKARG_A(GETARG_B(c)) | arg;
        }
      }
      break;

    case OP_STOP:
      irep->iseq[i] = MKOP_AB(OP_RETURN, irep->nlocals, OP_R_NORMAL);
      break;
    }
  }
}

void mrb_codedump_all(mrb_state*, struct RProc*);

static struct RProc*
create_proc_from_string(mrb_state *mrb, char *s, int len, mrb_value binding, const char *file, mrb_int line)
{
  mrbc_context *cxt;
  struct mrb_parser_state *p;
  struct RProc *proc, *scope;
  struct REnv *e;
  struct mrb_context *c = mrb->c;
  mrb_bool is_binding = FALSE;

  if (!mrb_nil_p(binding)) {
    if (!mrb_class_defined(mrb, "Binding")
        || !mrb_obj_is_kind_of(mrb, binding, mrb_class_get(mrb, "Binding"))) {
      mrb_raisef(mrb, E_TYPE_ERROR, "wrong argument type %S (expected binding)",
          mrb_obj_value(mrb_obj_class(mrb, binding)));
    }
    scope = mrb_proc_ptr(mrb_iv_get(mrb, binding, mrb_intern_lit(mrb, "proc")));
    is_binding = TRUE;
  }
  else {
    scope = c->ci[-1].proc;
  }

  cxt = mrbc_context_new(mrb);
  cxt->lineno = line;

  if (!file) {
    file = "(eval)";
  }
  mrbc_filename(mrb, cxt, file);
  cxt->capture_errors = TRUE;
  cxt->no_optimize = TRUE;

  p = mrb_parse_nstring(mrb, s, len, cxt);

  /* only occur when memory ran out */
  if (!p) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "Failed to create parser state.");
  }

  if (0 < p->nerr) {
    /* parse error */
    char buf[256];
    int n;
    n = snprintf(buf, sizeof(buf), "line %d: %s\n", p->error_buffer[0].lineno, p->error_buffer[0].message);
    mrb_parser_free(p);
    mrbc_context_free(mrb, cxt);
    mrb_exc_raise(mrb, mrb_exc_new(mrb, E_SYNTAX_ERROR, buf, n));
  }

  proc = mrb_generate_code(mrb, p);
  if (proc == NULL) {
    /* codegen error */
    mrb_parser_free(p);
    mrbc_context_free(mrb, cxt);
    mrb_raise(mrb, E_SCRIPT_ERROR, "codegen error");
  }
  if (scope->target_class) {
    proc->target_class = scope->target_class;
  }
  e = scope->env;
  if (!e) e = c->ci[-1].env;
  e = (struct REnv*)mrb_obj_alloc(mrb, MRB_TT_ENV, (struct RClass*)e);
  e->mid = c->ci[-1].mid;
  e->cioff = c->ci - c->cibase - 1;
  e->stack = c->ci->stackent;
  MRB_SET_ENV_STACK_LEN(e, scope->body.irep->nlocals);
  c->ci->target_class = proc->target_class;
  c->ci->env = 0;
  proc->env = e;
  patch_irep(mrb, proc->body.irep, 0, scope);

  mrb_parser_free(p);
  mrbc_context_free(mrb, cxt);

  return proc;
}

static mrb_value
f_eval(mrb_state *mrb, mrb_value self)
{
  char *s;
  mrb_int len;
  mrb_value binding = mrb_nil_value();
  char *file = NULL;
  mrb_int line = 1;
  struct RProc *proc;

  mrb_get_args(mrb, "s|ozi", &s, &len, &binding, &file, &line);

  proc = create_proc_from_string(mrb, s, len, binding, file, line);
  if (!mrb_nil_p(binding)) {
    self = mrb_iv_get(mrb, binding, mrb_intern_lit(mrb, "recv"));
  }
  mrb_assert(!MRB_PROC_CFUNC_P(proc));
  return mrb_exec_irep(mrb, self, proc);
}

#define CI_ACC_SKIP    -1

static mrb_value
f_instance_eval(mrb_state *mrb, mrb_value self)
{
  struct mrb_context *c = mrb->c;
  mrb_value b;
  mrb_int argc; mrb_value *argv;

  mrb_get_args(mrb, "*&", &argv, &argc, &b);

  if (mrb_nil_p(b)) {
    char *s;
    mrb_int len;
    char *file = NULL;
    mrb_int line = 1;
    mrb_value cv;
    struct RProc *proc;

    mrb_get_args(mrb, "s|zi", &s, &len, &file, &line);
    c->ci->acc = CI_ACC_SKIP;
    cv = mrb_singleton_class(mrb, self);
    c->ci->target_class = mrb_class_ptr(cv);
    proc = create_proc_from_string(mrb, s, len, mrb_nil_value(), file, line);
    mrb->c->ci->env = NULL;
    mrb_assert(!MRB_PROC_CFUNC_P(proc));
    return mrb_exec_irep(mrb, self, proc);
  }
  else {
    mrb_get_args(mrb, "&", &b);
    return mrb_obj_instance_eval(mrb, self);
  }
}

void
mrb_mruby_eval_gem_init(mrb_state* mrb)
{
  mrb_define_module_function(mrb, mrb->kernel_module, "eval", f_eval, MRB_ARGS_ARG(1, 3));
  mrb_define_method(mrb, mrb->kernel_module, "instance_eval", f_instance_eval, MRB_ARGS_ARG(1, 2));
}

void
mrb_mruby_eval_gem_final(mrb_state* mrb)
{
}

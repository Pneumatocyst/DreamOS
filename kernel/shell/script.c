/* Shell scripts: variables, arithmetic, if / while / for, and script files.
 *
 * The shell runs text in two steps:
 *
 *   1. split it into statements: one per line, and at every ; outside quotes
 *      ("if x; then echo hi; fi" is four statements: "if x", "then", "echo hi", "fi")
 *   2. walk through them. Keywords (if, while, for...) steer the walk; every other
 *      statement gets its $variables filled in and is handed to the ordinary command
 *      runner in shell.c (which still does pipes, > files and &&).
 *
 * Every command ends with a status: 0 means it worked, anything else that it didn't.
 * if and while look at that status, && looks at it, and $? shows it. */
#include "shell/shell.h"
#include "drivers/console.h"
#include "fs/ramfs.h"
#include "mm/heap.h"
#include "arch/cpu.h"
#include "sched/task.h"

#define MAX_VARS   64
#define NAME_MAX   24
#define VALUE_MAX  200
#define MAX_STMTS  1024
#define MAX_DEPTH  8           /* scripts running scripts */
#define LINE_LEN   512

/* ---- variables ---------------------------------------------------------------------- */

static struct { char name[NAME_MAX], value[VALUE_MAX]; } vars[MAX_VARS];

static bool name_char(char c, bool first) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (!first && c >= '0' && c <= '9');
}

static int find_var(const char *name) {
    for (int i = 0; i < MAX_VARS; i++) if (vars[i].name[0] && !strcmp(vars[i].name, name)) return i;
    return -1;
}

void sh_setvar(const char *name, const char *value) {
    int i = find_var(name);
    for (int j = 0; i < 0 && j < MAX_VARS; j++) if (!vars[j].name[0]) i = j;
    if (i < 0) { kerrorf(C_ERR "shell" C_DIM ": too many variables (%d at most)\n", MAX_VARS); return; }
    strlcpy(vars[i].name, name, NAME_MAX);
    strlcpy(vars[i].value, value, VALUE_MAX);
}

/* ---- the status of the last command -------------------------------------------------- */

static int status;                 /* $? */
static int fail_code;              /* set during a command by sh_fail_with: why it failed */

void sh_fail_with(int code) { if (!fail_code) fail_code = code ? code : 1; }
int  sh_failed(void) { return fail_code; }
void sh_clear_failure(void) { fail_code = 0; }
int  sh_status(void) { return status; }

/* ---- scripts' arguments, and how deep we are ------------------------------------------ */

struct frame { int argc; char *argv[16]; };
static struct frame frames[MAX_DEPTH];
static int depth;                  /* 0 = typing at the prompt */
static enum { FLOW_NONE, FLOW_BREAK, FLOW_CONTINUE, FLOW_EXIT } flow;
static int loops;                  /* how many loops we're inside (break needs one) */

bool sh_in_script(void) { return depth > 0; }
void sh_script_exit(int code) { status = code; flow = FLOW_EXIT; }
void sh_interrupt(void) { status = 130; flow = FLOW_EXIT; }

static uint32_t rnd(void) {
    static uint32_t s;
    if (!s) s = (uint32_t)pit_ticks() * 2654435761u | 1;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

/* What $name is: a script argument, a special one, or an ordinary variable. */
static const char *lookup(const char *name, char *tmp, size_t n) {
    struct frame *f = depth ? &frames[depth - 1] : NULL;
    if (!strcmp(name, "?")) { snprintf(tmp, n, "%d", status); return tmp; }
    if (!strcmp(name, "#")) { snprintf(tmp, n, "%d", f ? f->argc - 1 : 0); return tmp; }
    if (!strcmp(name, "@") || !strcmp(name, "*")) {
        size_t len = 0;
        tmp[0] = 0;
        for (int i = 1; f && i < f->argc; i++) len += snprintf(tmp + len, n - len, "%s%s", i > 1 ? " " : "", f->argv[i]);
        return tmp;
    }
    if (name[0] >= '0' && name[0] <= '9' && !name[1]) {
        int i = name[0] - '0';
        return f && i < f->argc ? f->argv[i] : "";
    }
    if (!strcmp(name, "RANDOM")) { snprintf(tmp, n, "%u", rnd() % 32768); return tmp; }
    if (!strcmp(name, "PWD")) { fs_path(sh_cwd(), tmp, n); return tmp; }
    if (!strcmp(name, "HOSTNAME")) return sh_hostname();
    int i = find_var(name);
    if (i >= 0) return vars[i].value;
    if (!strcmp(name, "HOME")) return "/root";
    return "";
}

/* ---- arithmetic: $(( ... )) ------------------------------------------------------------
 * A little recursive-descent calculator. Each function handles one level of
 * precedence and calls the next one up for its parts:
 *     compare  <  sum (+ -)  <  product (* / %)  <  unit (number, name, -x, (x))  */

static const char *ap;             /* where we are in the expression */
static bool arith_error;

static void skip(void) { while (*ap == ' ' || *ap == '\t') ap++; }
static int64_t compare(void);

static int64_t unit(void) {
    skip();
    if (*ap == '(') { ap++; int64_t v = compare(); skip(); if (*ap == ')') ap++; else arith_error = true; return v; }
    if (*ap == '-') { ap++; return -unit(); }
    if (*ap == '+') { ap++; return unit(); }
    if (*ap >= '0' && *ap <= '9') {
        int64_t v = 0;
        while (*ap >= '0' && *ap <= '9') v = v * 10 + (*ap++ - '0');
        return v;
    }
    if (name_char(*ap, true)) {                 /* a bare variable name, like i in $((i + 1)) */
        char name[NAME_MAX], tmp[64];
        int k = 0;
        while (name_char(*ap, false) && k < NAME_MAX - 1) name[k++] = *ap++;
        name[k] = 0;
        const char *v = lookup(name, tmp, sizeof tmp);
        int64_t n = 0;
        bool neg = *v == '-';
        if (neg) v++;
        while (*v >= '0' && *v <= '9') n = n * 10 + (*v++ - '0');
        return neg ? -n : n;
    }
    arith_error = true;
    return 0;
}

static int64_t product(void) {
    int64_t v = unit();
    for (;;) {
        skip();
        char o = *ap;
        if (o != '*' && o != '/' && o != '%') return v;
        ap++;
        int64_t r = unit();
        if (o == '*') v *= r;
        else if (!r) { arith_error = true; return 0; }   /* dividing by zero */
        else v = o == '/' ? v / r : v % r;
    }
}

static int64_t sum(void) {
    int64_t v = product();
    for (;;) {
        skip();
        if (*ap == '+') { ap++; v += product(); }
        else if (*ap == '-') { ap++; v -= product(); }
        else return v;
    }
}

static int64_t compare(void) {
    int64_t v = sum();
    skip();
    if (ap[0] == '=' && ap[1] == '=') { ap += 2; return v == sum(); }
    if (ap[0] == '!' && ap[1] == '=') { ap += 2; return v != sum(); }
    if (ap[0] == '<' && ap[1] == '=') { ap += 2; return v <= sum(); }
    if (ap[0] == '>' && ap[1] == '=') { ap += 2; return v >= sum(); }
    if (ap[0] == '<') { ap++; return v < sum(); }
    if (ap[0] == '>') { ap++; return v > sum(); }
    return v;
}

static bool arith(const char *expr, int64_t *out) {
    ap = expr;
    arith_error = false;
    *out = compare();
    skip();
    return !arith_error && !*ap;
}

/* ---- $(command): a command's output, in place ------------------------------------------- */

static void sh_exec_inner(const char *text);
static char *cap;                 /* where captured output goes (one $(...) at a time, innermost) */
static size_t cap_len, cap_max;
static bool cap_esc;

static void capture(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (cap_esc) { cap_esc = false; continue; }      /* colour codes aren't text */
        if (s[i] == '\033') { cap_esc = true; continue; }
        if (cap_len + 1 < cap_max) cap[cap_len++] = s[i];
    }
}

/* Run cmd with its output going into out (newlines become spaces, the last ones go). */
static void substitute(const char *cmd, char *out, size_t max) {
    char *saved = cap;                                   /* $( $(...) ) works: save the outer one */
    size_t saved_len = cap_len, saved_max = cap_max;
    void (*saved_hook)(const char *, size_t) = kout_current();
    cap = out; cap_len = 0; cap_max = max; cap_esc = false;
    kout_redirect(capture);
    int saved_status = status;
    sh_exec_inner(cmd);
    kout_redirect(saved_hook);
    while (cap_len && (out[cap_len - 1] == '\n' || out[cap_len - 1] == ' ')) cap_len--;
    out[cap_len] = 0;
    for (size_t i = 0; i < cap_len; i++) if (out[i] == '\n') out[i] = ' ';
    cap = saved; cap_len = saved_len; cap_max = saved_max;
    (void)saved_status;
}

/* ---- filling in $variables ------------------------------------------------------------- */

/* Copy `in` to `out`, replacing $name, ${name}, $? ... and $((sums)). Nothing is
   replaced inside 'single quotes', and \$ is a plain dollar sign. The quotes
   themselves are kept: the command runner removes them when it splits words. */
static void expand(const char *in, char *out, size_t n) {
    size_t o = 0;
    bool single = false, dbl = false;       /* inside '...' / inside "..." */
    char tmp[LINE_LEN];
#define PUT(c) do { if (o + 1 < n) out[o++] = (c); } while (0)
    for (const char *s = in; *s; s++) {
        if (*s == '\'' && !dbl) { single = !single; PUT(*s); continue; }   /* "it's" is just text */
        if (*s == '"' && !single) dbl = !dbl;
        if (single || *s != '$') {
            if (*s == '\\' && s[1] == '$' && !single) { PUT('$'); s++; continue; }
            PUT(*s);
            continue;
        }
        const char *val = NULL;
        if (s[1] == '(' && s[2] != '(') {                        /* $(command) */
            const char *start = s + 2, *e = start;
            int par = 1;
            char q = 0;
            for (; *e; e++) {
                if (q) { if (*e == q) q = 0; continue; }
                if (*e == '"' || *e == '\'') { q = *e; continue; }
                if (*e == '(') par++;
                if (*e == ')' && --par == 0) break;
            }
            if (!*e) { PUT('$'); continue; }
            char inner[LINE_LEN];
            size_t len = e - start < (long)sizeof inner - 1 ? (size_t)(e - start) : sizeof inner - 1;
            memcpy(inner, start, len);
            inner[len] = 0;
            substitute(inner, tmp, sizeof tmp);
            val = tmp;
            s = e;
        } else if (s[1] == '(' && s[2] == '(') {                        /* $(( arithmetic )) */
            const char *start = s + 3, *e = start;
            int par = 0;
            while (*e && !(par == 0 && e[0] == ')' && e[1] == ')')) { if (*e == '(') par++; if (*e == ')') par--; e++; }
            if (!*e) { PUT('$'); continue; }
            char inner[LINE_LEN], expanded[LINE_LEN];
            size_t len = e - start < (long)sizeof inner - 1 ? (size_t)(e - start) : sizeof inner - 1;
            memcpy(inner, start, len);
            inner[len] = 0;
            expand(inner, expanded, sizeof expanded);           /* $x inside works too */
            int64_t v;
            if (!arith(expanded, &v)) { kerrorf(C_ERR "shell" C_DIM ": can't work out " C_FG "%s\n", inner); v = 0; }
            snprintf(tmp, sizeof tmp, "%ld", v);
            val = tmp;
            s = e + 1;
        } else {
            char name[NAME_MAX];
            int k = 0;
            const char *p = s + 1;
            if (*p == '{') {                                     /* ${name} */
                p++;
                while (*p && *p != '}' && k < NAME_MAX - 1) name[k++] = *p++;
                if (*p == '}') p++;
            } else if (*p == '?' || *p == '#' || *p == '@' || *p == '*' || (*p >= '0' && *p <= '9')) {
                name[k++] = *p++;                                /* one-character specials */
            } else {
                while (name_char(*p, k == 0) && k < NAME_MAX - 1) name[k++] = *p++;
            }
            name[k] = 0;
            if (!k) { PUT('$'); continue; }                      /* a lone $ */
            val = lookup(name, tmp, sizeof tmp);
            s = p - 1;
        }
        for (; *val; val++) PUT(*val);
    }
    out[o] = 0;
#undef PUT
}

/* ---- running one statement ---------------------------------------------------------------- */

static void skip_space(const char **s) { while (**s == ' ' || **s == '\t') (*s)++; }

/* name=value (the whole statement): set a variable instead of running a command. */
static bool try_assign(const char *s) {
    const char *p = s;
    if (!name_char(*p, true)) return false;
    while (name_char(*p, false)) p++;
    if (*p != '=') return false;
    char name[NAME_MAX], value[VALUE_MAX];
    size_t nl = p - s < NAME_MAX - 1 ? (size_t)(p - s) : NAME_MAX - 1;
    memcpy(name, s, nl);
    name[nl] = 0;
    /* the value: quotes group it and are removed; an unquoted space ends it */
    size_t v = 0;
    char q = 0;
    for (p++; *p; p++) {
        if (q) { if (*p == q) { q = 0; continue; } }
        else if (*p == '"' || *p == '\'') { q = *p; continue; }
        else if (*p == ' ' || *p == '\t') break;
        if (v < VALUE_MAX - 1) value[v++] = *p;
    }
    value[v] = 0;
    skip_space(&p);
    if (*p) return false;                          /* "x=1 echo hi": not supported, run as a command */
    sh_setvar(name, value);
    return true;
}

/* Expand and run a plain statement. Returns its status. */
static int run_statement(const char *text) {
    char line[LINE_LEN];
    expand(text, line, sizeof line);
    const char *s = line;
    skip_space(&s);
    if (!*s) return status;
    if (try_assign(s)) { status = 0; return 0; }
    int code = sh_run_line(line);
    if (flow != FLOW_EXIT) status = code;          /* `exit 3` already set the status */
    return status;
}

/* ---- splitting text into statements ---------------------------------------------------------- */

struct program { char *buf; char **st; int n; };

static bool word_is(const char *s, const char *w) {
    size_t n = strlen(w);
    return !strncmp(s, w, n) && (s[n] == 0 || s[n] == ' ' || s[n] == '\t');
}

static void add(struct program *p, char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    if (!*s || p->n >= MAX_STMTS) return;
    /* "then echo hi", "do x=1", "else ..." -> the keyword, then the rest on its own */
    static const char *const kw[] = { "then", "do", "else" };
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(kw[i]);
        if (word_is(s, kw[i]) && s[l]) {
            s[l] = 0;
            p->st[p->n++] = s;
            add(p, s + l + 1);
            return;
        }
    }
    p->st[p->n++] = s;
}

static bool parse(const char *text, struct program *p) {
    size_t len = strlen(text);
    p->buf = kmalloc(len + 1);
    p->st = kmalloc(sizeof(char *) * MAX_STMTS);
    p->n = 0;
    if (!p->buf || !p->st) { kfree(p->buf); kfree(p->st); return false; }
    memcpy(p->buf, text, len + 1);
    char *start = p->buf, q = 0;
    bool word_start = true;
    for (char *c = p->buf; ; c++) {
        if (q) { if (*c == q) q = 0; else if (!*c) { add(p, start); break; } word_start = false; continue; }
        if (*c == '"' || *c == '\'') { q = *c; word_start = false; continue; }
        if (*c == '#' && word_start) {                   /* a comment: skip to the end of the line */
            *c = 0;
            add(p, start);
            c++;
            while (*c && *c != '\n') c++;
            if (!*c) break;
            start = c + 1;
            word_start = true;
            continue;
        }
        if (*c == '\n' || (*c == ';' && c[1] != ';') || !*c) {
            bool end = !*c;
            *c = 0;
            add(p, start);
            if (end) break;
            start = c + 1;
            word_start = true;
            continue;
        }
        word_start = *c == ' ' || *c == '\t';
    }
    return true;
}

static void free_program(struct program *p) { kfree(p->buf); kfree(p->st); }

/* ---- walking through the statements ----------------------------------------------------------- */

static int opener(const char *s) { return word_is(s, "if") || word_is(s, "while") || word_is(s, "until") || word_is(s, "for"); }
static int closer(const char *s) { return word_is(s, "fi") || word_is(s, "done"); }

/* the statement that closes the block opened at i (its fi or done), or -1 */
static int block_end(struct program *p, int i, int end) {
    int d = 0;
    for (int j = i; j < end; j++) {
        if (opener(p->st[j])) d++;
        else if (closer(p->st[j]) && --d == 0) return j;
    }
    return -1;
}

/* the next statement at this nesting level that is one of the given keywords */
static int next_at_level(struct program *p, int from, int end, const char *const *words, int nwords) {
    int d = 0;
    for (int j = from; j < end; j++) {
        if (d == 0) for (int w = 0; w < nwords; w++) if (word_is(p->st[j], words[w])) return j;
        if (opener(p->st[j])) d++;
        else if (closer(p->st[j])) d--;
    }
    return -1;
}

static bool stopped(void) {
    if (flow == FLOW_EXIT) return true;
    int k = sh_poll_key();                         /* ctrl+c stops a running script or loop */
    if (k == 3) { kerrorf(C_DIM "^C\n"); flow = FLOW_EXIT; status = 130; return true; }
    return false;
}

static void run_range(struct program *p, int a, int b);

/* the condition of if/while: the rest of the keyword's statement, then anything before
   `then` / `do`. Its status decides. */
static int condition(struct program *p, int at, const char *kw, int body) {
    const char *rest = p->st[at] + strlen(kw);
    skip_space(&rest);
    run_statement(rest);
    for (int j = at + 1; j < body && flow == FLOW_NONE; j++) run_statement(p->st[j]);
    return status;
}

static int syntax(const char *what, const char *s) {
    kerrorf(C_ERR "shell" C_DIM ": %s near " C_FG "%s\n", what, s);
    flow = FLOW_EXIT;
    status = 2;
    return -1;
}

static int do_if(struct program *p, int i, int end) {
    int fi = block_end(p, i, end);
    if (fi < 0) return syntax("this if has no fi", p->st[i]);
    static const char *const marks[] = { "then", "elif", "else", "fi" };
    int at = i;
    const char *kw = "if";
    for (;;) {
        int then = next_at_level(p, at + 1, fi + 1, marks, 1);
        if (then < 0) return syntax("expected then", p->st[at]);
        int ok = condition(p, at, kw, then);
        if (flow != FLOW_NONE) return fi;
        int next = next_at_level(p, then + 1, fi + 1, marks + 1, 3);   /* elif, else or fi */
        if (ok == 0) { run_range(p, then + 1, next); return fi; }
        if (word_is(p->st[next], "elif")) { at = next; kw = "elif"; continue; }
        if (word_is(p->st[next], "else")) run_range(p, next + 1, fi);
        return fi;
    }
}

static int do_while(struct program *p, int i, int end) {
    int done = block_end(p, i, end);
    if (done < 0) return syntax("this loop has no done", p->st[i]);
    static const char *const marks[] = { "do" };
    int body = next_at_level(p, i + 1, done, marks, 1);
    if (body < 0) return syntax("expected do", p->st[i]);
    bool until = word_is(p->st[i], "until");
    loops++;
    while (!stopped()) {
        int ok = condition(p, i, until ? "until" : "while", body);
        if (flow != FLOW_NONE || (until ? ok == 0 : ok != 0)) break;
        run_range(p, body + 1, done);
        if (flow == FLOW_BREAK) { flow = FLOW_NONE; break; }
        if (flow == FLOW_CONTINUE) flow = FLOW_NONE;
        task_yield();                              /* a busy loop shouldn't starve the others */
    }
    loops--;
    status = flow == FLOW_EXIT ? status : 0;
    return done;
}

static int do_for(struct program *p, int i, int end) {
    int done = block_end(p, i, end);
    if (done < 0) return syntax("this loop has no done", p->st[i]);
    static const char *const marks[] = { "do" };
    int body = next_at_level(p, i + 1, done, marks, 1);
    if (body < 0) return syntax("expected do", p->st[i]);
    /* for NAME in WORDS   (without "in": the script's arguments) */
    const char *s = p->st[i] + 3;
    skip_space(&s);
    char name[NAME_MAX];
    int k = 0;
    while (name_char(*s, k == 0) && k < NAME_MAX - 1) name[k++] = *s++;
    name[k] = 0;
    if (!k) return syntax("for needs a name", p->st[i]);
    skip_space(&s);
    char list[LINE_LEN];
    if (word_is(s, "in")) expand(s + 2, list, sizeof list);
    else expand("$@", list, sizeof list);
    /* split into words (quotes keep words together) */
    char *words[64];
    int nw = 0;
    char *c = list;
    while (*c && nw < 64) {
        while (*c == ' ' || *c == '\t') c++;
        if (!*c) break;
        char q = 0, *w = c, *o = c;
        while (*c && (q || (*c != ' ' && *c != '\t'))) {
            if (q && *c == q) { q = 0; c++; continue; }
            if (!q && (*c == '"' || *c == '\'')) { q = *c++; continue; }
            *o++ = *c++;
        }
        if (*c) c++;
        *o = 0;
        words[nw++] = w;
    }
    loops++;
    for (int w = 0; w < nw && !stopped(); w++) {
        sh_setvar(name, words[w]);
        run_range(p, body + 1, done);
        if (flow == FLOW_BREAK) { flow = FLOW_NONE; break; }
        if (flow == FLOW_CONTINUE) flow = FLOW_NONE;
    }
    loops--;
    status = flow == FLOW_EXIT ? status : 0;
    return done;
}

static void run_range(struct program *p, int a, int b) {
    for (int i = a; i < b && flow == FLOW_NONE; i++) {
        const char *s = p->st[i];
        int end;
        if (word_is(s, "if")) end = do_if(p, i, b);
        else if (word_is(s, "while") || word_is(s, "until")) end = do_while(p, i, b);
        else if (word_is(s, "for")) end = do_for(p, i, b);
        else if (word_is(s, "break") || word_is(s, "continue")) {
            if (!loops) { syntax("not in a loop", s); return; }
            flow = word_is(s, "break") ? FLOW_BREAK : FLOW_CONTINUE;
            return;
        } else if (word_is(s, "then") || word_is(s, "do") || word_is(s, "else") || word_is(s, "elif") ||
                   closer(s)) {
            syntax("unexpected", s);
            return;
        } else { run_statement(s); continue; }
        if (end < 0) return;
        i = end;
    }
}

/* ---- the entry points ------------------------------------------------------------------------- */

static void sh_exec_inner(const char *text) {
    struct program p;
    if (!parse(text, &p)) return;
    run_range(&p, 0, p.n);
    free_program(&p);
}

/* Run some text: one line typed at the prompt, or a whole script. */
void sh_exec(const char *text) {
    struct program p;
    if (!parse(text, &p)) { kerrorf(C_ERR "shell" C_DIM ": out of memory\n"); return; }
    run_range(&p, 0, p.n);
    free_program(&p);
    if (!depth) flow = FLOW_NONE;                  /* at the prompt, nothing carries over */
}

/* Run a script file with arguments ($0 is its name). false if it couldn't be read. */
bool sh_run_script(const char *path, int argc, char **argv) {
    if (depth == MAX_DEPTH) { kerrorf(C_ERR "shell" C_DIM ": scripts nested too deep\n"); return true; }
    int n = fs_lookup(sh_cwd(), path);
    if (n < 0 || fs_get(n)->dir) return false;
    size_t size = fs_get(n)->size;
    char *text = kmalloc(size + 1);
    if (!text) { kerrorf(C_ERR "shell" C_DIM ": out of memory\n"); return true; }
    memcpy(text, fs_data(n), size);              /* a copy: the script might change its own file */
    text[size] = 0;

    struct frame *f = &frames[depth];
    f->argc = argc < 16 ? argc : 16;
    for (int i = 0; i < f->argc; i++) {
        size_t l = strlen(argv[i]) + 1;
        f->argv[i] = kmalloc(l);
        if (f->argv[i]) memcpy(f->argv[i], argv[i], l);
        else { f->argc = i; break; }               /* out of memory: fewer arguments */
    }
    depth++;
    int saved_loops = loops;
    loops = 0;
    sh_exec(text);
    loops = saved_loops;
    depth--;
    if (flow == FLOW_EXIT && depth == 0) flow = FLOW_NONE;   /* exit ends this script only */
    if (flow == FLOW_EXIT && depth > 0 && status != 130) flow = FLOW_NONE;   /* ...unless it was ctrl+c */
    for (int i = 0; i < f->argc; i++) kfree(f->argv[i]);
    kfree(text);
    if (status) sh_fail_with(status);              /* the script's status is the command's */
    return true;
}

bool sh_is_script(const char *data, size_t size) {
    return size >= 2 && data[0] == '#' && data[1] == '!';
}

/* ---- built-in commands for scripts -------------------------------------------------------------- */

SH_CMD(true)  { (void)argc; (void)argv; }
SH_CMD(false) { (void)argc; (void)argv; sh_fail_with(1); }

static bool number(const char *s, int64_t *v) {
    bool neg = *s == '-';
    if (neg) s++;
    if (!*s) return false;
    int64_t n = 0;
    for (; *s; s++) { if (*s < '0' || *s > '9') return false; n = n * 10 + (*s - '0'); }
    *v = neg ? -n : n;
    return true;
}

/* test (also spelled [ ... ]): check something; fail quietly if it isn't so.
     -f file  -d dir  -e path  -z string  -n string
     a = b   a != b   n -eq m  -ne  -lt  -le  -gt  -ge   ! (not) */
SH_CMD(test) {
    if (!strcmp(argv[0], "[")) {
        if (strcmp(argv[argc - 1], "]")) { kerrorf(C_ERR "[" C_DIM ": missing ]\n"); return; }
        argc--;
    }
    bool negate = argc > 1 && !strcmp(argv[1], "!");
    char **a = argv + 1 + negate;
    int n = argc - 1 - negate;
    bool ok = false;
    int64_t x, y;
    if (n == 0) ok = false;
    else if (n == 1) ok = a[0][0] != 0;                            /* [ "$x" ]: not empty */
    else if (n == 2) {
        int node = fs_lookup(sh_cwd(), a[1]);
        if (!strcmp(a[0], "-z")) ok = !a[1][0];
        else if (!strcmp(a[0], "-n")) ok = a[1][0];
        else if (!strcmp(a[0], "-e")) ok = node >= 0;
        else if (!strcmp(a[0], "-f")) ok = node >= 0 && !fs_get(node)->dir;
        else if (!strcmp(a[0], "-d")) ok = node >= 0 && fs_get(node)->dir;
        else { kerrorf(C_ERR "test" C_DIM ": don't know " C_FG "%s\n", a[0]); return; }
    } else if (n == 3) {
        const char *op = a[1];
        if (!strcmp(op, "=") || !strcmp(op, "==")) ok = !strcmp(a[0], a[2]);
        else if (!strcmp(op, "!=")) ok = strcmp(a[0], a[2]) != 0;
        else if (op[0] == '-' && number(a[0], &x) && number(a[2], &y)) {
            if (!strcmp(op, "-eq")) ok = x == y;
            else if (!strcmp(op, "-ne")) ok = x != y;
            else if (!strcmp(op, "-lt")) ok = x < y;
            else if (!strcmp(op, "-le")) ok = x <= y;
            else if (!strcmp(op, "-gt")) ok = x > y;
            else if (!strcmp(op, "-ge")) ok = x >= y;
            else { kerrorf(C_ERR "test" C_DIM ": don't know " C_FG "%s\n", op); return; }
        } else { kerrorf(C_ERR "test" C_DIM ": %s needs numbers or a known comparison\n", op); return; }
    } else { kerrorf(C_ERR "test" C_DIM ": too many words. try quoting: " C_FG "[ \"$x\" = yes ]\n"); return; }
    if (ok == negate) sh_fail_with(1);
}

/* read NAME...: a line from the keyboard into variables (the last one gets the rest). */
SH_CMD(read) {
    char line[VALUE_MAX];
    int len = 0;
    for (;;) {
        int k = sh_wait_key();
        if (k == '\n') { con_putc('\n'); break; }
        if (k == 3) { kerrorf(C_DIM "^C\n"); flow = FLOW_EXIT; status = 130; return; }
        if (k == '\b') { if (len) { len--; con_putc('\b'); } continue; }
        if (k >= 32 && k < 127 && len < VALUE_MAX - 1) { line[len++] = (char)k; con_putc((char)k); }
    }
    line[len] = 0;
    char *s = line;
    for (int i = 1; i < argc; i++) {
        while (*s == ' ') s++;
        char *w = s;
        if (i < argc - 1) { while (*s && *s != ' ') s++; if (*s) *s++ = 0; }
        sh_setvar(argv[i], w);
    }
    if (argc < 2) sh_setvar("REPLY", line);
}

/* shift: drop $1, so $2 becomes $1 (for scripts that take a word, then "the rest") */
SH_CMD(shift) {
    (void)argc; (void)argv;
    if (!depth) return;
    struct frame *f = &frames[depth - 1];
    if (f->argc < 2) return;
    kfree(f->argv[1]);
    memmove(&f->argv[1], &f->argv[2], sizeof f->argv[0] * (f->argc - 2));
    f->argc--;
}

SH_CMD(set) {
    (void)argc; (void)argv;
    for (int i = 0; i < MAX_VARS; i++)
        if (vars[i].name[0]) kprintf(C_ACCENT "%s" C_DIM "=" C_FG "%s\n", vars[i].name, vars[i].value);
}

SH_CMD(unset) {
    for (int i = 1; i < argc; i++) { int v = find_var(argv[i]); if (v >= 0) vars[v].name[0] = 0; }
}

/* sh FILE [args] / source FILE [args]: run a script */
SH_CMD(sh) {
    if (argc < 2) { kprintf("usage: %s <script> [arguments]\n", argv[0]); return; }
    if (!sh_run_script(argv[1], argc - 1, argv + 1))
        kerrorf(C_ERR "%s" C_DIM ": no such script: " C_FG "%s\n", argv[0], argv[1]);
}

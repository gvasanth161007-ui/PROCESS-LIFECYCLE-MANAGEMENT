/* Process Lifecycle Management System - backend in pure C (POSIX sockets, no libraries). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

enum { NEW, READY, RUNNING, WAITING, SUSPENDED, TERMINATED };
static const char *SN[] = {"NEW", "READY", "RUNNING", "WAITING", "SUSPENDED", "TERMINATED"};
/* Valid state transitions (PRD section 11). Everything else is rejected. */
static const int T[6][6] = {[NEW][READY] = 1, [READY][RUNNING] = 1, [RUNNING][WAITING] = 1,
    [WAITING][READY] = 1, [RUNNING][SUSPENDED] = 1, [READY][SUSPENDED] = 1,
    [SUSPENDED][READY] = 1, [RUNNING][TERMINATED] = 1};

typedef struct { int pid, state, priority, burst, remaining, memory, arrival, start, term,
                 parent, pc, wait_left, waiting, ready_since; char name[40]; } Proc; /* = PCB */
typedef struct { int pid, prev, next, ts; char reason[48]; } Ev;
#define MP 256
#define ME 4096
static Proc P[MP]; static int np, nextpid = 1;
static Ev E[ME]; static int ne;
static int clk, busy, autosched = 1, dirty;
static char errm[160];
static char out[1 << 19]; static int ol;

static void J(const char *f, ...) {
    va_list a; va_start(a, f);
    ol += vsnprintf(out + ol, sizeof out - ol, f, a); va_end(a);
    if (ol >= (int)sizeof out) ol = sizeof out - 1;
}
static void save(void) {
    FILE *f = fopen("plms.dat", "wb"); if (!f) return;
    int h[6] = {np, nextpid, clk, busy, autosched, ne};
    fwrite(h, sizeof h, 1, f); fwrite(P, sizeof(Proc), np, f); fwrite(E, sizeof(Ev), ne, f); fclose(f);
}
static void load(void) {
    FILE *f = fopen("plms.dat", "rb"); if (!f) return; int h[6];
    if (fread(h, sizeof h, 1, f) == 1 && h[0] >= 0 && h[0] <= MP && h[5] >= 0 && h[5] <= ME) {
        np = h[0]; nextpid = h[1]; clk = h[2]; busy = h[3]; autosched = h[4]; ne = h[5];
        if (fread(P, sizeof(Proc), np, f) != (size_t)np || fread(E, sizeof(Ev), ne, f) != (size_t)ne) np = ne = 0;
    }
    fclose(f);
}
static Proc *find(int id) { for (int i = 0; i < np; i++) if (P[i].pid == id) return &P[i]; return NULL; }
static Proc *running(void) { for (int i = 0; i < np; i++) if (P[i].state == RUNNING) return &P[i]; return NULL; }

static void ev(int pid, int pv, int nx, const char *why) {
    if (ne == ME) { memmove(E, E + ME / 2, (ME / 2) * sizeof(Ev)); ne = ME / 2; }
    Ev *e = &E[ne++]; e->pid = pid; e->prev = pv; e->next = nx; e->ts = clk;
    snprintf(e->reason, sizeof e->reason, "%s", why); dirty = 1;
}
/* Every state change goes through here: updates PCB timestamps and records history. */
static void move(Proc *p, int to, const char *why) {
    int pv = p->state;
    if (pv == READY) p->waiting += clk - p->ready_since;
    p->state = to;
    if (to == READY) p->ready_since = clk;
    if (to == RUNNING && p->start < 0) p->start = clk;
    if (to == WAITING) p->wait_left = 4;
    if (to == TERMINATED) { p->term = clk; if (p->remaining < 0) p->remaining = 0; }
    ev(p->pid, pv, to, why);
}
/* Returns 1 ok, 0 rejected (errm set), -1 unknown action. */
static int act(Proc *p, const char *a) {
    static const struct { const char *n; int to; const char *why; } A[] = {
        {"admit", READY, "Admitted to Ready Queue"}, {"run", RUNNING, "CPU assigned"},
        {"wait", WAITING, "Resource required"}, {"suspend", SUSPENDED, "Suspended by user"},
        {"resume", READY, "Resumed by user"}, {"terminate", TERMINATED, "Terminated by user"}};
    for (unsigned i = 0; i < sizeof A / sizeof *A; i++) {
        if (strcmp(a, A[i].n)) continue;
        if (!T[p->state][A[i].to]) {
            if (!strcmp(a, "resume")) snprintf(errm, sizeof errm, "Cannot resume process. Process is not SUSPENDED.");
            else snprintf(errm, sizeof errm, "Cannot %s this process. Process is currently %s.", a, SN[p->state]);
            return 0;
        }
        Proc *r = running();
        if (A[i].to == RUNNING && r) { snprintf(errm, sizeof errm, "Cannot run this process. CPU is busy with P%03d.", r->pid); return 0; }
        move(p, A[i].to, A[i].why); return 1;
    }
    return -1;
}
/* One tick of the simulated OS clock. */
static void tick(void) {
    clk++; Proc *r = running();
    if (r) { busy++; r->pc++; if (--r->remaining <= 0) move(r, TERMINATED, "Burst completed"); dirty = 1; }
    for (int i = 0; i < np; i++) if (P[i].state == WAITING && --P[i].wait_left <= 0) move(&P[i], READY, "Resource available");
    if (!autosched) return;
    for (int i = 0; i < np; i++) if (P[i].state == NEW) move(&P[i], READY, "Admitted by scheduler");
    if (!running()) { /* priority scheduling: lowest number first, FCFS on ties */
        Proc *b = NULL;
        for (int i = 0; i < np; i++) if (P[i].state == READY && (!b || P[i].priority < b->priority ||
            (P[i].priority == b->priority && P[i].ready_since < b->ready_since))) b = &P[i];
        if (b) move(b, RUNNING, "Scheduler dispatched");
    }
}
/* ---- JSON output ---- */
static void pj(Proc *p) {
    int w = p->waiting + (p->state == READY ? clk - p->ready_since : 0);
    J("{\"pid\":\"P%03d\",\"name\":\"%s\",\"state\":\"%s\",\"priority\":%d,\"cpuBurst\":%d,\"remainingTime\":%d,"
      "\"memory\":%d,\"arrivalTime\":%d,\"startTime\":%d,\"terminationTime\":%d,\"parentPid\":",
      p->pid, p->name, SN[p->state], p->priority, p->burst, p->remaining, p->memory, p->arrival, p->start, p->term);
    if (p->parent) J("\"P%03d\"", p->parent); else J("null");
    J(",\"programCounter\":%d,\"readySince\":%d,\"waitingTime\":%d,\"turnaroundTime\":%d,\"responseTime\":%d}",
      p->pc, p->ready_since, w, p->state == TERMINATED ? p->term - p->arrival : -1, p->start >= 0 ? p->start - p->arrival : -1);
}
static void ej(Ev *e) {
    J("{\"pid\":\"P%03d\",\"previousState\":", e->pid);
    if (e->prev >= 0) J("\"%s\"", SN[e->prev]); else J("null");
    J(",\"newState\":\"%s\",\"timestamp\":%d,\"reason\":\"%s\"}", SN[e->next], e->ts, e->reason);
}
static void lj(int pid, int max) { /* pid==0: newest-first log; else oldest-first history */
    int first = 1, cnt = 0; J("[");
    if (pid) { for (int i = 0; i < ne; i++) if (E[i].pid == pid) { if (!first) J(","); first = 0; ej(&E[i]); } }
    else for (int i = ne - 1; i >= 0 && cnt < max; i--, cnt++) { if (!first) J(","); first = 0; ej(&E[i]); }
    J("]");
}
static void sj(void) {
    int c[6] = {0}, n = 0; double w = 0, t = 0, r = 0;
    for (int i = 0; i < np; i++) { Proc *p = &P[i]; c[p->state]++;
        if (p->state == TERMINATED) { n++; w += p->waiting; t += p->term - p->arrival; r += p->start - p->arrival; } }
    J("{\"total\":%d,\"counts\":{", np);
    for (int s = 0; s < 6; s++) J("%s\"%s\":%d", s ? "," : "", SN[s], c[s]);
    J("},\"cpuUtilization\":%.1f,\"avgWaitingTime\":%.2f,\"avgTurnaroundTime\":%.2f,\"avgResponseTime\":%.2f}",
      clk ? 100.0 * busy / clk : 0.0, n ? w / n : 0, n ? t / n : 0, n ? r / n : 0);
}
/* ---- tiny JSON input ---- */
static int jnum(const char *b, const char *k, int d) {
    char key[32]; snprintf(key, sizeof key, "\"%s\"", k);
    const char *p = strstr(b, key); if (!p) return d;
    p = strchr(p + strlen(key), ':'); if (!p) return d; p++;
    while (*p == ' ' || *p == '"' || *p == 'P') p++;
    return (isdigit((unsigned char)*p) || *p == '-') ? atoi(p) : d;
}
static int jstr(const char *b, const char *k, char *o, int n) { /* sanitises: letters, digits, space - _ . only */
    char key[32]; snprintf(key, sizeof key, "\"%s\"", k);
    const char *p = strstr(b, key); if (!p) return 0;
    p = strchr(p + strlen(key), ':'); if (!p) return 0; p++;
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    int i = 0; for (p++; *p && *p != '"' && i < n - 1; p++) if (isalnum((unsigned char)*p) || strchr(" -_.", *p)) o[i++] = *p;
    o[i] = 0; return 1;
}
/* ---- HTTP ---- */
static void wr(int fd, const char *b, int n) { while (n > 0) { int w = write(fd, b, n); if (w <= 0) return; b += w; n -= w; } }
static void reply(int fd, int code, const char *ct) {
    const char *t = code == 200 ? "OK" : code == 201 ? "Created" : code == 204 ? "No Content" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : "Conflict";
    char h[400]; int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type\r\n"
        "Access-Control-Allow-Methods: GET,POST,PUT,DELETE\r\nConnection: close\r\n\r\n", code, t, ct, ol);
    wr(fd, h, n); wr(fd, out, ol);
}
static void fail(int fd, int code, const char *m) { ol = 0; J("{\"error\":\"%s\"}", m); reply(fd, code, "application/json"); }
#define OK(c) reply(fd, c, "application/json")

static void handle(int fd) {
    char rq[8192]; int n = 0; struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (n < (int)sizeof rq - 1) {
        int r = recv(fd, rq + n, sizeof rq - 1 - n, 0); if (r <= 0) break; n += r; rq[n] = 0;
        char *h = strstr(rq, "\r\n\r\n");
        if (h) { char *cl = strcasestr(rq, "content-length:"); if (n >= (h - rq) + 4 + (cl ? atoi(cl + 15) : 0)) break; }
    }
    rq[n] = 0;
    char m[8], path[256]; if (sscanf(rq, "%7s %255s", m, path) != 2) return;
    char *q = strchr(path, '?'); if (q) *q = 0;
    char *body = strstr(rq, "\r\n\r\n"); body = body ? body + 4 : "";
    int get = !strcmp(m, "GET"), post = !strcmp(m, "POST"); ol = 0;
    if (!strcmp(m, "OPTIONS")) { OK(204); return; }
    if (get && !strcmp(path, "/")) {
        FILE *f = fopen("public/index.html", "rb"); if (!f) { fail(fd, 404, "public/index.html not found"); return; }
        ol = fread(out, 1, sizeof out - 1, f); fclose(f); reply(fd, 200, "text/html; charset=utf-8"); return;
    }
    if (get && !strcmp(path, "/api/state")) {
        Proc *r = running(); J("{\"clock\":%d,\"auto\":%s,\"cpu\":", clk, autosched ? "true" : "false");
        if (r) pj(r); else J("null");
        J(",\"processes\":["); for (int i = 0; i < np; i++) { if (i) J(","); pj(&P[i]); }
        J("],\"stats\":"); sj(); J(",\"logs\":"); lj(0, 40); J("}"); OK(200); return;
    }
    if (get && !strcmp(path, "/api/statistics")) { sj(); OK(200); return; }
    if (get && !strcmp(path, "/api/logs")) { lj(0, 100); OK(200); return; }
    if (post && !strcmp(path, "/api/scheduler")) { autosched = strstr(body, "true") != NULL; dirty = 1; J("{\"auto\":%s}", autosched ? "true" : "false"); OK(200); return; }
    if (post && !strcmp(path, "/api/reset")) { np = ne = clk = busy = 0; nextpid = 1; dirty = 1; J("{\"reset\":true}"); OK(200); return; }
    if (strncmp(path, "/api/processes", 14)) { fail(fd, 404, "Unknown endpoint."); return; }
    const char *s = path + 14;
    if (!*s) {
        if (get) { J("["); for (int i = 0; i < np; i++) { if (i) J(","); pj(&P[i]); } J("]"); OK(200); return; }
        if (!post) { fail(fd, 400, "Method not supported."); return; }
        char nm[40];
        if (!jstr(body, "name", nm, sizeof nm) || !nm[0]) { fail(fd, 400, "Cannot create process. Required fields are missing."); return; }
        int pr = jnum(body, "priority", -1), bu = jnum(body, "cpuBurst", -1), me = jnum(body, "memory", -1), pa = jnum(body, "parentPid", 0);
        if (pr < 1 || pr > 10 || bu < 1 || bu > 1000 || me < 1 || me > 65536) { fail(fd, 400, "Cannot create process. Priority must be 1-10, CPU burst 1-1000 ms, memory 1-65536 MB."); return; }
        if (np >= MP) { fail(fd, 409, "Process table is full."); return; }
        Proc *p = &P[np++]; memset(p, 0, sizeof *p);
        p->pid = nextpid++; snprintf(p->name, sizeof p->name, "%s", nm); p->priority = pr; p->burst = p->remaining = bu;
        p->memory = me; p->arrival = clk; p->start = p->term = p->ready_since = -1; p->parent = (pa > 0 && find(pa)) ? pa : 0;
        ev(p->pid, -1, NEW, "Process created"); pj(p); OK(201); return;
    }
    int id = 0; char a[16] = "";
    if (*s != '/' || sscanf(s, "/P%d/%15s", &id, a) < 1) { fail(fd, 404, "Unknown endpoint."); return; }
    Proc *p = find(id); if (!p) { fail(fd, 404, "Process not found."); return; }
    if (get && !a[0]) { pj(p); OK(200); return; }
    if (get && !strcmp(a, "history")) { lj(id, 0); OK(200); return; }
    if (!strcmp(m, "PUT") && !a[0]) {
        if (p->state == TERMINATED) { fail(fd, 409, "Cannot edit process. Process is currently TERMINATED."); return; }
        char nm[40]; int pr = jnum(body, "priority", p->priority);
        if (pr < 1 || pr > 10) { fail(fd, 400, "Priority must be 1-10."); return; }
        if (jstr(body, "name", nm, sizeof nm) && nm[0]) snprintf(p->name, sizeof p->name, "%s", nm);
        p->priority = pr; dirty = 1; pj(p); OK(200); return;
    }
    if (!strcmp(m, "DELETE") && !a[0]) {
        int i = p - P; memmove(P + i, P + i + 1, (np - i - 1) * sizeof(Proc)); np--; dirty = 1;
        J("{\"deleted\":\"P%03d\"}", id); OK(200); return;
    }
    if (post && a[0]) {
        int r = act(p, a);
        if (r < 0) fail(fd, 404, "Unknown action."); else if (!r) fail(fd, 409, errm); else { pj(p); OK(200); }
        return;
    }
    fail(fd, 400, "Method not supported.");
}
int main(int argc, char **argv) {
    const char *envp = getenv("PORT"); /* hosting platforms set PORT */
    int port = argc > 1 ? atoi(argv[1]) : envp ? atoi(envp) : 8080; signal(SIGPIPE, SIG_IGN); load();
    int ls = socket(AF_INET, SOCK_STREAM, 0), on = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in ad = {.sin_family = AF_INET, .sin_port = htons(port)}; ad.sin_addr.s_addr = inet_addr(argc > 2 ? argv[2] : envp ? "0.0.0.0" : "127.0.0.1"); /* optional bind address, e.g. 0.0.0.0 */
    if (bind(ls, (struct sockaddr *)&ad, sizeof ad) || listen(ls, 16)) { perror("bind/listen"); return 1; }
    printf("Process Lifecycle Management System running at http://localhost:%d\n", port); fflush(stdout);
    time_t last = time(0);
    for (;;) {
        struct pollfd pf = {ls, POLLIN, 0}; poll(&pf, 1, 200);
        for (time_t now = time(0); last < now; last++) tick();
        if (pf.revents & POLLIN) { int c = accept(ls, NULL, NULL); if (c >= 0) { handle(c); close(c); } }
        if (dirty) { save(); dirty = 0; }
    }
}

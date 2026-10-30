/* verify_waitfor.c — част от verify.c (виж verify_proto.h). */
#include "baga.h"
#include "verify_impl.h"

/* ======================= M19: wait-for acyclicity =======================
 * Structural liveness fragment (docs/thesis-open-problems.md §1.4).
 * A cycle is REFUTED when a thread waits on a recv/join that can only be
 * satisfied by an action the waiter itself is blocking. Matched sequential
 * send/recv, join-after-send, and recv-after-a-send-first worker are PROVEN.
 * if/while bodies, nested go, recv2/select, packed args, and non-constant
 * loop bounds are no-claim (honest incompleteness) — never a false PROVEN.
 *
 * M24: send-blocking on a full bounded buffer (baga_chan_send waits on
 * not_full, src/baga_par_rt.c). With a constant capacity (chan_new literal)
 * a parent send is checked against outstanding = n_send - n_recv so far
 * minus recv credits (recv-first workers already spawned, go_bg consumers
 * included): outstanding - credits >= cap with no complex worker on the
 * channel is a kind-3 REFUTED; otherwise "свободен слот" is PROVEN. A join
 * of a send-only worker is REFUTED when wf_n_send > cap + parent recvs +
 * other consumer credits — the worker can never finish. Competing producers
 * (parent sends before the join on that channel, go_bg send-first) keep the
 * join check honest no-claim; symbolic capacity keeps both checks silent.
 *
 * M25: counted loops in workers (known-N fan-in). A `for i in lo..hi` with
 * constant bounds is scanned once and its counts scaled by k = hi - lo
 * (hi exclusive); loop-body aliases do not leak. Producer coverage is now
 * exact — wf_producer_capacity sums wf_n_send over classified send-first
 * workers — so a parent recv past parent sends + producer capacity is
 * REFUTED (previously no-claim past one), and multi-recv workers are honest
 * credits instead of complex. while/if bodies, non-constant ranges, and
 * nested go stay no-claim.
 *
 * M26: the other blocking forms. recv2 blocks exactly like recv and gets
 * the same check (it had none — a silent deadlock). select2_wait is
 * satisfied when either channel will eventually hold an item
 * (wf_chan_eventual: buffered + producer capacity - recv credits; a closed
 * channel keeps only its buffered items), REFUTED when neither will; the
 * winning side is unknown, so both channels are taxed (n_recv++) and marked
 * wf_unknown/wf_sel_unknown — later claims that need the exact drain stay
 * silent, REFUTED directions stay sound. recv on a closed channel never
 * blocks (returns 0) — no claims. select2, try_recv and the timeout forms
 * never block — no claims; in workers they stay complex (optional
 * consumption).
 *
 * M27: if/else merge in the worker scan. Both branches are scanned
 * separately; the merged counts are guaranteed minimums (the worker
 * performs at least the min of each side), which is exactly what credits,
 * producer capacity, and the over-send check need for their PROVEN
 * directions. A body whose branch counts differ becomes wf_imprecise: the
 * minimums still prove, but every REFUTED direction on the channel sees
 * unknown (they need exactness). The first blocking op is classified only
 * when both branches agree; a branch that may not block (or blocks
 * differently) before any op was seen makes the scan complex. while/match
 * stay complex — they need induction. Also closes an M26 gap: the select
 * PROVEN now requires !unk — an unknown consumer could eat the item.
 *
 * M28: a worker with `while true` and no break/return in the loop body on
 * its straight-line path never completes (the checker agrees such a body
 * diverges — no return required after it). Joining it waits forever:
 * kind-3 REFUTED "join на worker без изход". Inside a branch the loop may
 * never run — no claim; go_bg of such a worker is fine (nothing joins
 * it).
 *
 * M29: nested go with a structured join. go(child, tracked-channel) inside
 * a worker is classified recursively (depth <= 4). join(h) folds the
 * child: guaranteed counts, imprecise flag, noreturn (the parent blocks
 * forever on a child that never completes), and — if the parent has not
 * blocked yet — the child's first blocking op. detach(h) and never-joined
 * children fold credit-only (child_cons): their guaranteed consumption
 * counts, but the parent's completion does not wait for them, so no send
 * fold, noreturn, or first-op. A child in an if branch or a loop body may
 * run zero or k times — complex; untracked channels, packed args, unknown
 * fns, and double joins stay complex. go_bg consumption is now the full
 * guaranteed count (n_recv + child_cons), fixing the old 1-per-worker
 * undercount.
 *
 * M30: server loops. A while-true WITH an exit runs its body at least
 * once; the guaranteed prefix (statements up to the first may-exit) is
 * counted and classifies the first blocking op. The loop may spin
 * forever: wf_maybe_loop — join-completion PROVENs and the over-send
 * check are gated off, the cycle REFUTED stays sound (the guaranteed
 * first recv blocks, the parent waits, nobody sends), and every
 * REFUTED direction on the channel sees unknown (consumption/production
 * is unbounded). Statements after the loop, after an if with a spinning
 * branch, and inside a for body with such a loop are not guaranteed —
 * the walk stops or goes complex. */


/* M29: a child worker spawned inside a scanned body. hvar is the handle
 * variable it was bound to (empty for a bare go); folded marks that its
 * counts were merged into the parent (at a join, or by the leftover pass). */

static void wf_scan_free(WfScan *s) {
    for (int i = 0; i < s->nkids; i++) wf_scan_free(&s->kids[i].sc);
    free(s->kids);
    s->kids = NULL; s->nkids = s->kidcap = 0;
}

/* M29: children never joined (or detached) still run — their guaranteed
 * consumption is credit, but the parent's completion does not wait for
 * them: no send fold, no noreturn, no first-op inheritance. An unjoined
 * complex child makes the channel behaviour unknown. */
static void wf_fold_leftover(WfScan *s) {
    for (int i = 0; i < s->nkids; i++) {
        WfChild *k = &s->kids[i];
        if (k->folded) continue;
        k->folded = 1;
        if (k->sc.complex) { s->complex = 1; continue; }
        s->child_cons += k->sc.n_recv;
        if (k->sc.imprecise) s->imprecise = 1;
    }
}

static int wf_name_is(const char **names, int nn, const char *s) {
    for (int i = 0; i < nn; i++) if (strcmp(names[i], s) == 0) return 1;
    return 0;
}

static void wf_scan_node(Node *n, const char **names, int *nn, int nmax, WfScan *s);

/* M28: does a tree contain a return or a break anywhere? Conservative — a
 * return inside a nested lambda counts too (missed detection, never a
 * false noreturn claim). */
static int wf_has_exit(Node *n) {
    if (!n) return 0;
    switch (n->kind) {
    case NODE_RETURN: case NODE_BREAK: return 1;
    case NODE_BLOCK:
        for (int i = 0; i < n->stmts.len; i++) if (wf_has_exit(n->stmts.data[i])) return 1;
        return 0;
    case NODE_LET:     return wf_has_exit(n->let_init);
    case NODE_EXPR_STMT: return wf_has_exit(n->expr);
    case NODE_ASSIGN:  return wf_has_exit(n->assign_val);
    case NODE_IF:      return wf_has_exit(n->then_br) || wf_has_exit(n->else_br);
    case NODE_WHILE:   return wf_has_exit(n->while_body);
    case NODE_FOR:     return wf_has_exit(n->for_body);
    case NODE_BINARY:  return wf_has_exit(n->left) || wf_has_exit(n->right);
    case NODE_UNARY:   return wf_has_exit(n->operand);
    case NODE_CALL:
        for (int i = 0; i < n->args.len; i++) if (wf_has_exit(n->args.data[i])) return 1;
        return 0;
    default: return 0;
    }
}

/* M30: scan the guaranteed prefix of a loop body — the statements up to
 * (and including) the first one that may exit; whatever follows is not
 * guaranteed on the first iteration. */
static void wf_scan_prefix(Node *body, const char **names, int *nn, int nmax, WfScan *s) {
    if (!body) return;
    if (body->kind == NODE_BLOCK) {
        for (int i = 0; i < body->stmts.len; i++) {
            Node *st = body->stmts.data[i];
            wf_scan_node(st, names, nn, nmax, s);
            if (s->complex || s->maybe_loop) return;
            if (wf_has_exit(st)) return;
        }
        return;
    }
    wf_scan_node(body, names, nn, nmax, s);
}

static void wf_scan_call(Node *call, const char **names, int nn, WfScan *s) {
    if (!call || call->kind != NODE_CALL) return;
    if (!is_par_builtin_call(call)) { s->complex = 1; return; }  /* hidden send/recv */
    const char *nm = call->callee->name;
    if (strcmp(nm, "go") == 0 || strcmp(nm, "go_bg") == 0) {
        /* M29: classify the child worker on the tracked channel (bounded
         * recursion). Untracked channel, packed args, unknown fn, or depth
         * exhausted — honest complex. */
        if (call->args.len < 2 || call->args.data[0]->kind != NODE_IDENT ||
            call->args.data[1]->kind != NODE_IDENT ||
            !wf_name_is(names, nn, call->args.data[1]->name) ||
            s->depth >= 4) { s->complex = 1; return; }
        Node *cfn = find_fn(g_prog, call->args.data[0]->name);
        if (!cfn || !cfn->fn_body || cfn->params.len < 1) { s->complex = 1; return; }
        WfScan cs; memset(&cs, 0, sizeof cs);
        cs.depth = s->depth + 1;
        const char *cnms[16]; int cnn = 0;
        cnms[cnn++] = cfn->params.data[0]->param_name;
        wf_scan_node(cfn->fn_body, cnms, &cnn, 16, &cs);
        wf_fold_leftover(&cs);
        if (s->nkids == s->kidcap) {
            s->kidcap = s->kidcap ? s->kidcap * 2 : 4;
            s->kids = realloc(s->kids, (size_t)s->kidcap * sizeof(WfChild));
        }
        WfChild *k = &s->kids[s->nkids++];
        k->hvar[0] = 0; k->folded = 0; k->detached = 0;
        k->sc = cs;   /* moved: the parent owns cs.kids */
        return;
    }
    if (strcmp(nm, "join") == 0 || strcmp(nm, "detach") == 0) {
        /* M29: fold a classified child. join waits for the child, so its
         * guaranteed counts, termination, and first blocking op become the
         * parent's; detach only marks it leftover (credit-only fold). */
        if (call->args.len < 1 || call->args.data[0]->kind != NODE_IDENT) { s->complex = 1; return; }
        WfChild *k = NULL;
        for (int i = 0; i < s->nkids; i++)
            if (s->kids[i].hvar[0] && strcmp(s->kids[i].hvar, call->args.data[0]->name) == 0) { k = &s->kids[i]; break; }
        if (!k || k->folded) { s->complex = 1; return; }
        if (nm[0] == 'd') { k->detached = 1; return; }
        k->folded = 1;
        WfScan *c = &k->sc;
        if (c->noreturn) { s->noreturn = 1; s->complex = 1; return; }
        if (c->complex) { s->complex = 1; return; }
        s->n_send += c->n_send;
        s->n_recv += c->n_recv;
        if (c->imprecise) s->imprecise = 1;
        if (c->maybe_loop) s->maybe_loop = 1;   /* M30: the join may block forever */
        if (!s->saw_op && c->saw_op) {
            s->saw_op = 1;
            s->recv_first = c->recv_first;
            s->send_first = c->send_first;
        }
        return;
    }
    int on_p = call->args.len >= 1 && call->args.data[0]->kind == NODE_IDENT &&
               wf_name_is(names, nn, call->args.data[0]->name);
    if (!on_p) {
        /* M26: a blocking op on a channel we do not track hides a wait-for
         * edge — complex. Non-blocking forms (try_recv/select2/timeouts)
         * stay irrelevant. */
        if (strcmp(nm, "chan_recv") == 0 || strcmp(nm, "chan_send") == 0 ||
            strcmp(nm, "chan_recv2") == 0 || strcmp(nm, "chan_select2_wait") == 0) s->complex = 1;
        return;
    }
    if (strcmp(nm, "chan_recv") == 0 || strcmp(nm, "chan_recv2") == 0) {
        s->n_recv++;   /* M26: recv2 blocks (and consumes) exactly like recv */
        if (!s->saw_op) { s->saw_op = 1; s->recv_first = 1; }
    } else if (strcmp(nm, "chan_send") == 0) {
        s->n_send++;
        if (!s->saw_op) { s->saw_op = 1; s->send_first = 1; }
    } else if (strcmp(nm, "chan_close") == 0 || strcmp(nm, "chan_new") == 0) {
        /* non-blocking */
    } else {
        s->complex = 1;   /* try_recv / select* / recv_timeout: optional
                           * consumption breaks credit/join accounting */
    }
}

static void wf_scan_node(Node *n, const char **names, int *nn, int nmax, WfScan *s) {
    if (!n || s->complex) return;
    switch (n->kind) {
    case NODE_BLOCK:
        for (int i = 0; i < n->stmts.len; i++) {
            wf_scan_node(n->stmts.data[i], names, nn, nmax, s);
            if (s->complex) return;
            if (s->maybe_loop) return;   /* M30: the rest is not guaranteed */
        }
        return;
    case NODE_LET: {
        int before = s->nkids;   /* M29: name a child by its handle var */
        wf_scan_node(n->let_init, names, nn, nmax, s);
        if (s->nkids > before)
            snprintf(s->kids[s->nkids - 1].hvar, sizeof s->kids[0].hvar, "%s", n->let_name);
        if (n->let_init && n->let_init->kind == NODE_IDENT &&
            wf_name_is(names, *nn, n->let_init->name)) {
            if (*nn < nmax) names[(*nn)++] = n->let_name;
            else s->complex = 1;
        }
        return;
    }
    case NODE_EXPR_STMT: wf_scan_node(n->expr, names, nn, nmax, s); return;
    case NODE_RETURN:    wf_scan_node(n->ret_val, names, nn, nmax, s); return;
    case NODE_ASSIGN:    wf_scan_node(n->assign_val, names, nn, nmax, s); return;
    case NODE_CALL:      wf_scan_call(n, names, *nn, s); return;
    case NODE_BINARY:
        wf_scan_node(n->left, names, nn, nmax, s);
        wf_scan_node(n->right, names, nn, nmax, s);
        return;
    case NODE_UNARY:     wf_scan_node(n->operand, names, nn, nmax, s); return;
    case NODE_IF: {
        /* M27: an if whose branches stay in the fragment merges with
         * guaranteed-min counts — the worker performs at least the min of
         * each side. The first blocking op is classified only when BOTH
         * branches agree on it; if one side may not block (or they block
         * differently) before any op was seen, the first op is a guess —
         * complex. Aliases born in a branch stay in the branch. */
        WfScan ba, bb; memset(&ba, 0, sizeof ba); memset(&bb, 0, sizeof bb);
        int saved_nn = *nn;
        wf_scan_node(n->cond, names, nn, nmax, s);   /* runs before either branch */
        if (s->complex) return;
        wf_scan_node(n->then_br, names, nn, nmax, &ba);
        *nn = saved_nn;
        wf_scan_node(n->else_br, names, nn, nmax, &bb);   /* NULL = empty else */
        *nn = saved_nn;
        if (ba.nkids || bb.nkids) {   /* M29: a child in a branch may not run */
            wf_scan_free(&ba); wf_scan_free(&bb);
            s->complex = 1;
            return;
        }
        if (ba.complex || bb.complex) { s->complex = 1; return; }
        if (ba.maybe_loop || bb.maybe_loop) s->maybe_loop = 1;   /* M30: a
            branch may spin forever — statements after the if are not
            guaranteed */
        s->n_send += ba.n_send < bb.n_send ? ba.n_send : bb.n_send;
        s->n_recv += ba.n_recv < bb.n_recv ? ba.n_recv : bb.n_recv;
        if (ba.n_send != bb.n_send || ba.n_recv != bb.n_recv) s->imprecise = 1;
        if (ba.imprecise || bb.imprecise) s->imprecise = 1;
        if (!s->saw_op) {
            if (ba.saw_op && bb.saw_op &&
                ba.recv_first == bb.recv_first && ba.send_first == bb.send_first) {
                s->saw_op = 1;
                s->recv_first = ba.recv_first;
                s->send_first = ba.send_first;
            } else if (ba.saw_op || bb.saw_op) {
                s->complex = 1;
            }
        }
        return;
    }
    case NODE_WHILE: {
        /* M28: `while true` with no break/return in its body never exits
         * (the checker agrees — no return is required after it). On the
         * straight-line path that means the worker never completes; the
         * prefix counts stand, everything after is unreachable. */
        if (n->while_cond && n->while_cond->kind == NODE_BOOL_LIT &&
            n->while_cond->bool_val) {
            if (!wf_has_exit(n->while_body)) {
                s->noreturn = 1;
                s->complex = 1;
                return;
            }
            /* M30: a server loop — while true WITH an exit. The body runs
             * at least once; its guaranteed prefix (up to the first
             * statement that may exit) runs on the first iteration. Those
             * ops are counted and classify the first blocking op; the loop
             * may spin forever, so completion and everything after the
             * loop are not guaranteed (maybe_loop). */
            WfScan bs; memset(&bs, 0, sizeof bs);
            bs.depth = s->depth;
            wf_scan_prefix(n->while_body, names, nn, nmax, &bs);
            if (bs.complex || bs.nkids) { wf_scan_free(&bs); s->complex = 1; return; }
            if (!bs.saw_op) { wf_scan_free(&bs); s->complex = 1; return; }
            s->n_send += bs.n_send;
            s->n_recv += bs.n_recv;
            if (bs.imprecise) s->imprecise = 1;
            if (!s->saw_op) {
                s->saw_op = 1;
                s->recv_first = bs.recv_first;
                s->send_first = bs.send_first;
            }
            s->maybe_loop = 1;
            wf_scan_free(&bs);
            return;
        }
        s->complex = 1;
        return;
    }
    case NODE_MATCH:
        s->complex = 1;
        return;
    case NODE_FOR: {
        /* M25: counted loop over a constant range (hi exclusive, codegen
         * emits x < hi). Scan the body once, scale its counts by k — the
         * known-N discipline of §1.4. Aliases born in the body stay in the
         * body (loop scope); an empty range executes nothing. */
        if (!n->for_iter || n->for_iter->kind != NODE_RANGE ||
            !n->for_iter->range_lo || !n->for_iter->range_hi ||
            n->for_iter->range_lo->kind != NODE_INT_LIT ||
            n->for_iter->range_hi->kind != NODE_INT_LIT) { s->complex = 1; return; }
        long long k = (long long)n->for_iter->range_hi->int_val -
                      (long long)n->for_iter->range_lo->int_val;
        if (k <= 0) return;
        WfScan bs; memset(&bs, 0, sizeof bs);
        int saved_nn = *nn;
        wf_scan_node(n->for_body, names, nn, nmax, &bs);
        *nn = saved_nn;
        if (bs.nkids) {   /* M29: a child spawned k times — no single fold */
            wf_scan_free(&bs);
            s->complex = 1;
            return;
        }
        if (bs.complex || bs.maybe_loop) {   /* M30: a body that may spin
            forever breaks the fixed-k completion argument */
            wf_scan_free(&bs);
            s->complex = 1;
            return;
        }
        if (k > 1000000 ||
            (long long)(bs.n_send + bs.n_recv) * k > 1000000000LL) { s->complex = 1; return; }
        s->n_send += (int)(bs.n_send * k);
        s->n_recv += (int)(bs.n_recv * k);
        if (bs.imprecise) s->imprecise = 1;
        if (!s->saw_op && bs.saw_op) {
            s->saw_op = 1;
            s->recv_first = bs.recv_first;
            s->send_first = bs.send_first;
        }
        return;
    }
    default: return;
    }
}

/* M25: exact producer capacity — the total sends of classified send-first
 * workers on `ch` (join handles carry wf_n_send; n_bg_prod is the summed
 * send count of go_bg send-first workers). Complex workers keep the set
 * unknown — never a false REFUTED. */
static int wf_producer_capacity(State *st, HandleEntry *ch, int *unknown) {
    if (!ch) return 0;
    if (ch->wf_unknown) *unknown = 1;
    int cap = ch->n_bg_prod;
    for (int i = 0; i < st->handles.n; i++) {
        HandleEntry *h = &st->handles.h[i];
        if (h->kind != HK_JOIN || !h->chan_arg) continue;
        if (strcmp(h->chan_arg, ch->var) != 0) continue;
        if (h->wf_complex) *unknown = 1;
        else {
            if (h->wf_imprecise) *unknown = 1;   /* M27: minimums only — the
                REFUTED directions need exactness; mins still prove */
            if (h->wf_maybe_loop && h->wf_send) *unknown = 1;   /* M30: it may
                produce unbounded — a recv might still be satisfied. A loop
                CONSUMER only helps a recv REFUTED, so it stays sound. */
            if (h->wf_send) cap += h->wf_n_send;
        }
    }
    return cap;
}

static void wf_check_recv(State *st, Obligations *ob, HandleEntry *ch) {
    if (!ch) return;
    if (ch->state != 0) { ch->n_recv++; return; }   /* M26: closed — never
        blocks (returns 0 or a buffered item), but the consumption counts */
    ch->n_recv++;
    int unk = 0;
    int capw = wf_producer_capacity(st, ch, &unk);
    int unmatched = ch->n_recv - ch->n_send;
    if (unmatched <= 0) {
        push_protocol_ok(st, ob, "wait-for: последователни send/recv");
    } else if (unmatched <= capw) {
        push_protocol_ok(st, ob, "wait-for: recv след producer");
    } else if (!unk) {
        push_protocol_violation(st, ob, "wait-for цикъл: recv без send");
    }
}

/* M24: parent send on a constant-capacity channel. A recv credit is a
 * recv-first worker already spawned (join handle or go_bg) — it is blocked
 * on recv, so it will consume one item. Blocked forever iff the items in
 * flight before this send, minus every credit, still fill the buffer and no
 * complex worker could be a hidden consumer. */
static void wf_check_send(State *st, Obligations *ob, HandleEntry *ch) {
    if (!ch || ch->wf_cap < 0) return;
    if (ch->state != 0) return;   /* closed: send never blocks (returns -1) */
    int credits = ch->n_bg_cons, unknown = ch->wf_unknown;
    for (int i = 0; i < st->handles.n; i++) {
        HandleEntry *h = &st->handles.h[i];
        if (h->kind != HK_JOIN || !h->chan_arg) continue;
        if (strcmp(h->chan_arg, ch->var) != 0) continue;
        if (h->wf_complex) unknown = 1;
        else {
            if (h->wf_imprecise) unknown = 1;   /* M27: it may consume more */
            if (h->wf_maybe_loop && h->wf_recv) unknown = 1;   /* M30: it may
                drain unbounded — blocked-forever is not certain. A loop
                producer cannot free a slot, so it stays sound. */
            if (h->wf_recv) credits += h->wf_n_recv;
            credits += h->wf_child_cons;   /* M29: unjoined children consume too */
        }
    }
    if (ch->n_send - ch->n_recv - credits >= ch->wf_cap) {
        if (!unknown)
            push_protocol_violation(st, ob, "wait-for цикъл: send върху пълен буфер без consumer");
    } else if (!ch->wf_sel_unknown) {
        push_protocol_ok(st, ob, "wait-for: send — свободен слот");
    }
}

static void wf_check_join(State *st, Obligations *ob, HandleEntry *jh) {
    if (!jh) return;
    if (jh->wf_noreturn) {
        /* M28: the worker never completes — the join waits forever */
        push_protocol_violation(st, ob, "wait-for: join на worker без изход");
        return;
    }
    if (!jh->chan_arg || jh->wf_complex) return;
    HandleEntry *ch = handles_find(&st->handles, jh->chan_arg);
    if (!ch || ch->kind != HK_CHAN) return;
    if (jh->wf_recv) {
        int unk = 0;
        int capw = wf_producer_capacity(st, ch, &unk);
        if (ch->n_send > 0) {
            if (!jh->wf_maybe_loop)   /* M30: completion not guaranteed */
                push_protocol_ok(st, ob, "wait-for: join след send");
        } else if (capw > 0) {
            if (!jh->wf_maybe_loop)
                push_protocol_ok(st, ob, "wait-for: join, друг producer");
        } else if (!unk) {
            /* sound even for a server loop: its guaranteed first recv
             * blocks, the parent waits on the join, nobody sends */
            push_protocol_violation(st, ob, "wait-for цикъл: join преди send");
        }
    }
    /* M24: a send-only worker must fit — its sends complete iff
     * wf_n_send <= cap + parent recvs so far + other consumer credits.
     * Competing producers on the channel make room accounting dishonest:
     * no-claim. */
    if (jh->wf_send && jh->wf_n_recv == 0 && !jh->wf_maybe_loop &&
        ch->wf_cap >= 0 &&
        ch->state == 0 && ch->n_send == 0 && ch->n_bg_prod == 0) {
        int credits = ch->n_bg_cons, unknown = ch->wf_unknown, rivals = 0;
        for (int i = 0; i < st->handles.n; i++) {
            HandleEntry *h = &st->handles.h[i];
            if (h->kind != HK_JOIN || !h->chan_arg) continue;
            if (strcmp(h->chan_arg, ch->var) != 0) continue;
            if (h == jh) { credits += h->wf_child_cons; continue; }   /* M29: its own
                unjoined consumer frees slots for its sends */
            if (h->wf_complex) unknown = 1;
            else {
                if (h->wf_imprecise) unknown = 1;   /* M27: it may consume more */
                if (h->wf_maybe_loop && h->wf_recv) unknown = 1;   /* M30: it
                    may free slots unbounded — over-send is not certain */
                if (h->wf_recv) credits += h->wf_n_recv;
                else if (h->wf_send) rivals = 1;
            }
        }
        if (rivals) return;
        int room = ch->wf_cap + ch->n_recv + credits;
        if (jh->wf_n_send > room) {
            if (!unknown)
                push_protocol_violation(st, ob, "wait-for цикъл: worker send върху пълен буфер");
        } else if (!ch->wf_sel_unknown && !jh->wf_imprecise) {
            push_protocol_ok(st, ob, "wait-for: worker send се побира в буфера");
        }
    }
}

/* M26: resolve a call argument to its channel handle (NULL = not a known
 * channel ghost var — no claims). */
static HandleEntry *wf_resolve_chan(State *st, Node *arg) {
    Sym cv = se_from_ast(arg, &st->env, &st->vlen, &st->reads);
    HandleEntry *e = NULL;
    if (!cv.nonlinear && cv.lin.n == 1 && cv.lin.c.num == 0 &&
        cv.lin.terms[0].coeff.num == cv.lin.terms[0].coeff.den)
        e = handles_find(&st->handles, cv.lin.terms[0].var);
    if (e && e->kind != HK_CHAN) e = NULL;
    lin_free(&cv.lin);
    return e;
}

/* M26: items that will eventually be available on `ch` for a fresh taker:
 * buffered (parent sends - parent recvs) plus worker producer capacity
 * (zero once closed — sends on a closed channel return -1), minus recv
 * credits. Never overclaims: unknown workers propagate via `unknown`. */
static int wf_chan_eventual(State *st, HandleEntry *ch, int *unknown) {
    if (!ch) return 0;
    int credits = ch->n_bg_cons, unk = 0;
    for (int i = 0; i < st->handles.n; i++) {
        HandleEntry *h = &st->handles.h[i];
        if (h->kind != HK_JOIN || !h->chan_arg) continue;
        if (strcmp(h->chan_arg, ch->var) != 0) continue;
        if (h->wf_complex) unk = 1;
        else {
            if (h->wf_imprecise) unk = 1;   /* M27: it may consume more */
            if (h->wf_maybe_loop && h->wf_send) unk = 1;   /* M30: it may feed
                the channel unbounded — starvation is not certain. A loop
                consumer only helps starvation, so it stays sound. */
            if (h->wf_recv) credits += h->wf_n_recv;
            credits += h->wf_child_cons;   /* M29: unjoined children consume too */
        }
    }
    if (unk) *unknown = 1;
    int buffered = ch->n_send - ch->n_recv;
    int capw = ch->state == 1 ? 0 : wf_producer_capacity(st, ch, unknown);
    return buffered + capw - credits;
}

/* M26: select2_wait blocks until either channel yields an item (or both are
 * closed — it returns (3,0) at once). Satisfied when either side will
 * eventually hold an item; REFUTED when neither will and nothing is
 * unknown. The consumed side is unknown, so the caller taxes both channels
 * (n_recv++) and marks them wf_unknown / wf_sel_unknown afterwards. */
static void wf_check_select(State *st, Obligations *ob, HandleEntry *a, HandleEntry *b) {
    int unk = 0;
    int ea = wf_chan_eventual(st, a, &unk);
    int eb = wf_chan_eventual(st, b, &unk);
    if (!a || !b) unk = 1;   /* unknown channel expression: never refute */
    int both_closed = a && b && a->state == 1 && b->state == 1;
    if (both_closed || (!unk && (ea > 0 || eb > 0))) {
        push_protocol_ok(st, ob, "wait-for: select — има producer");
    } else if (!unk) {
        push_protocol_violation(st, ob, "wait-for цикъл: select без producer");
    }
}

/* M14: symbolic semantics of the !Par builtins (statement level only).
 *
 * go(f, x): f must be a pure verifiable (i64)->i64 user fn (gated earlier).
 * Fork-join determinism: the spawned computation of a pure worker is the
 * function call itself, so the worker's contract applies via the M5
 * assume–guarantee machinery — the join handle carries the symbolic result.
 * join(h)/detach(h): the runtime handle protocol (spawn → join | detach;
 * join-after-detach is fatal, src/baga_par_rt.c) becomes a static protocol:
 * a second consume on the same handle is a kind-3 violation (REFUTED).
 * Channels: open/closed ghost state; send returns 0 (ok) or -1 (closed) —
 * the interval -1 <= s <= 0 is exact; on a known-closed channel s == -1.
 * recv payload is unconstrained (any payload, or 0 on closed+empty).
 * M19 wait-for: join-before-send with a recv-first worker, and recv with no
 * matching send/producer, are kind-3 REFUTED; matched sequential send/recv
 * and join-after-send are PROVEN. Returns the symbolic value of the call. */
/* M17: a (status, value) pair result for the pair-returning channel APIs.
 * status: fresh __c var with [0, hi] range on the path. value: fresh __c var
 * with M16 content axioms instantiated — from the single channel, or for
 * select2* only the axioms BOTH channels share (same cmp, structurally equal
 * rhs): the value may come from either, so only common facts are honest. */
static Sym make_status_pair(State *st, Node *c0, Node *c1, int hi) {
    char sv[64], vv[64], pv[64];
    snprintf(sv, sizeof sv, "__c%d", g_call_ctr++);
    snprintf(vv, sizeof vv, "__c%d", g_call_ctr++);
    path_push_ge(&st->path, sv, 0);
    path_push_le(&st->path, sv, hi);
    char kbuf0[64], kbuf1[64];
    const char *key0 = resolve_key(c0, st, kbuf0, sizeof kbuf0);
    const char *key1 = c1 ? resolve_key(c1, st, kbuf1, sizeof kbuf1) : NULL;
    for (int i = 0; i < st->ax.n; i++) {
        ElemAxiom *a = &st->ax.a[i];
        if (a->is_sorted || !key0 || strcmp(a->vec, key0) != 0) continue;
        if (!c1) {
            cl_push(&st->path, axiom_instantiate(a, vv));
        } else if (key1) {
            for (int j = 0; j < st->ax.n; j++) {
                ElemAxiom *b = &st->ax.a[j];
                if (b->is_sorted || strcmp(b->vec, key1) != 0 || b->cmp != a->cmp) continue;
                if (lin_eq(&a->rhs, &b->rhs)) { cl_push(&st->path, axiom_instantiate(a, vv)); break; }
            }
        }
    }
    snprintf(pv, sizeof pv, "__q%d", g_prod_ctr++);
    reads_push_pair(&st->reads, pv, lin_var(sv), lin_var(vv));
    return sym_lin(lin_var(pv));
}

static const char *world_move_scan(State *st, Obligations *ob, Node *arg, int delegate_ok,
                                  int can_join, const char *skip);   /* W1.5…W2.7 */
static const char *world_handoff(State *st, Obligations *ob, HandleEntry *we,
                                const char *op, int joinable, int delegate_ok,
                                const char *chan);   /* W2.4/W2.6; W2.12: chan != NULL при chan_send */

Sym eval_par_call(Node *call, State *st, Obligations *ob) {
    const char *nm = call->callee->name;
    g_recv_world_kind = 0;   /* W2.8: само този recv може да го сложи */
    g_recv_world_chan[0] = 0;   /* W2.12 */

    /* M17: pair-returning channel APIs */
    if (strncmp(nm, "chan_select2", 12) == 0 || strcmp(nm, "chan_recv2") == 0 ||
        strcmp(nm, "chan_try_recv") == 0 || strcmp(nm, "chan_recv_timeout") == 0) {
        /* M26: wait-for on the blocking variants. recv2 blocks exactly like
         * recv; select2_wait blocks until either channel yields. The plain
         * select2 / try_recv / *_timeout forms never block — no claims. */
        if (strcmp(nm, "chan_recv2") == 0) {
            wf_check_recv(st, ob, wf_resolve_chan(st, call->args.data[0]));
        } else if (strcmp(nm, "chan_select2_wait") == 0) {
            HandleEntry *ea = wf_resolve_chan(st, call->args.data[0]);
            HandleEntry *eb = wf_resolve_chan(st, call->args.data[1]);
            wf_check_select(st, ob, ea, eb);
            /* the winning side is unknown: tax both (conservative for the
             * PROVEN arithmetic) and silence later refutations/claims that
             * depend on which side was drained */
            if (ea) { ea->n_recv++; ea->wf_unknown = 1; ea->wf_sel_unknown = 1; }
            if (eb) { eb->n_recv++; eb->wf_unknown = 1; eb->wf_sel_unknown = 1; }
        }
        int hi = 1;
        if (strcmp(nm, "chan_recv2") != 0)
            hi = (strncmp(nm, "chan_select2", 12) == 0) ? 3 : 2;
        Node *c1 = (strncmp(nm, "chan_select2", 12) == 0) ? call->args.data[1] : NULL;
        return make_status_pair(st, call->args.data[0], c1, hi);
    }

    if (strcmp(nm, "go") == 0 || strcmp(nm, "go_bg") == 0) {
        /* W1.5: go(worker, arg) — свят в аргумента (директно или в cell2
         * пакет) = прехвърляне на собствеността (като chan_send). W2.1:
         * работник със spec вече НЕ е sound silence — `requires:
         * world_frozen(ctx)` го регистрира като HK_WORLD(1) в неговото тяло
         * (мутатор там е ОБРОЧЕНО), а тук discharge-ва изискването срещу
         * състоянието на аргумента. Затова move-скенът е СЛЕД discharge-а:
         * иначе преместването би променило състоянието преди проверката. */
        Node *w = call->args.data[0];
        const char *wname = w->name;
        Node *wfn = find_fn(g_prog, wname);
        Sym res;
        if (find_spec(g_prog, wname)) {
            /* synthesize `wname(arg)` and reuse M5: discharge requires
             * (kind-2 obligations), assume ensures when justified */
            Node fake; memset(&fake, 0, sizeof fake);
            fake.kind = NODE_CALL; fake.callee = w;
            NodeVec fa; fa.data = &call->args.data[1]; fa.len = 1; fa.cap = 1;
            fake.args = fa;
            res = eval_user_call(&fake, st, ob, g_caller_name);
        } else {
            /* spec-less worker: nothing to discharge or assume (honest) */
            /* W2.7: работник без spec няма договор — проследен свят не може да
             * му се предаде (освен ЗАЕМАН, където W2.6 правилата вече спорят). */
            if (call->args.data[1] && call->args.data[1]->kind == NODE_IDENT) {
                HandleEntry *we0 = handles_find(&st->handles, call->args.data[1]->name);
                if (we0 && we0->kind == HK_WORLD && !we0->borrowed) {
                    char nolabel[300];
                    snprintf(nolabel, sizeof nolabel,
                        "предаване на свят '%s' на работник '%s' без spec — декларирай "
                        "requires: world_frozen/world_mutable/world_owned", we0->var, wname);
                    push_protocol_violation(st, ob, nolabel);
                }
            }
            Sym a = se_from_ast(call->args.data[1], &st->env, &st->vlen, &st->reads);
            lin_free(&a.lin);
            /* M16 drop rule: a spec-less worker may send anything on the
             * channel argument — the caller's axioms on it are dropped */
            char kbuf[64];
            const char *key = resolve_key(call->args.data[1], st, kbuf, sizeof kbuf);
            if (key) ax_drop_key(&st->ax, key);
            char rv[64]; snprintf(rv, sizeof rv, "__c%d", g_call_ctr++);
            res = sym_lin(lin_var(rv));
        }
        /* W2.6/W2.7: договорът на работника (param 0) решава три неща —
         * дали ЗАЕМАН свят може да се делегира надолу (само world_frozen), и
         * дали собствеността вече е прехвърлена от discharge-а (world_owned:
         * тогава scan-ът не трябва да я третира като второ предаване). */
        Node *wspec = find_spec(g_prog, wname);
        int wcont = spec_world_contract(wspec, wfn, 0);
        int deleg_ok = (wcont == 1);
        const char *skip = NULL;
        if (wcont == 3 && call->args.data[1] && call->args.data[1]->kind == NODE_IDENT)
            skip = call->args.data[1]->name;
        const char *shared_w = world_move_scan(st, ob, call->args.data[1], deleg_ok,
                                              strcmp(nm, "go_bg") != 0, skip);
        /* M19: classify the worker's first blocking op on the channel arg */
        HandleEntry *ch = NULL;
        {
            Sym carg = se_from_ast(call->args.data[1], &st->env, &st->vlen, &st->reads);
            const char *cv = !carg.nonlinear ? lin_handle_var(&carg.lin) : NULL;
            if (cv) {
                ch = handles_find(&st->handles, cv);
                if (ch && ch->kind != HK_CHAN) ch = NULL;
            }
            lin_free(&carg.lin);
        }
        WfScan sc; memset(&sc, 0, sizeof sc);
        if (wfn && wfn->fn_body && wfn->params.len >= 1) {
            const char *nms[16]; int nn = 0;
            nms[nn++] = wfn->params.data[0]->param_name;
            wf_scan_node(wfn->fn_body, nms, &nn, 16, &sc);
            wf_fold_leftover(&sc);   /* M29: unjoined/detached children */
        } else if (wfn) {
            sc.complex = 1;
        }
        if (strcmp(nm, "go_bg") == 0) {   /* detached at birth: no handle */
            if (ch) {
                if (sc.complex || sc.imprecise) ch->wf_unknown = 1;
                else {
                    if (sc.send_first) ch->n_bg_prod += sc.n_send;   /* M25: summed sends */
                    ch->n_bg_cons += sc.n_recv + sc.child_cons;   /* M29: all guaranteed consumption */
                    if (sc.maybe_loop) ch->wf_unknown = 1;   /* M30: unbounded behavior */
                }
            }
            wf_scan_free(&sc);
            lin_free(&res.lin);
            return sym_lin(lin_const(rat_int(0)));
        }
        char hv[64]; snprintf(hv, sizeof hv, "__h%d", g_handle_ctr++);
        if (res.nonlinear) { Lin empty; lin_init(&empty); handles_set(&st->handles, hv, HK_JOIN, 0, &empty); }
        else handles_set(&st->handles, hv, HK_JOIN, 0, &res.lin);   /* moves res.lin */
        {
            HandleEntry *jh = handles_find(&st->handles, hv);
            if (jh) {
                jh->worker = strdup(wname);
                if (ch) jh->chan_arg = strdup(ch->var);
                if (shared_w) jh->world_arg = strdup(shared_w);   /* W2.4: joinable share */
                jh->wf_recv = sc.recv_first && !sc.complex;
                jh->wf_send = sc.send_first && !sc.complex;
                jh->wf_complex = sc.complex;
                jh->wf_n_send = sc.complex ? 0 : sc.n_send;
                jh->wf_n_recv = sc.complex ? 0 : sc.n_recv;
                jh->wf_child_cons = sc.complex ? 0 : sc.child_cons;
                jh->wf_imprecise = sc.complex ? 0 : sc.imprecise;
                jh->wf_noreturn = sc.noreturn;
                jh->wf_maybe_loop = sc.complex ? 0 : sc.maybe_loop;
            }
            if (ch && sc.complex) ch->wf_unknown = 1;
        }
        wf_scan_free(&sc);
        return sym_lin(lin_var(hv));
    }

    if (strcmp(nm, "join") == 0 || strcmp(nm, "detach") == 0) {
        int is_join = (nm[0] == 'j');
        Sym hv = se_from_ast(call->args.data[0], &st->env, &st->vlen, &st->reads);
        HandleEntry *e = NULL;
        if (!hv.nonlinear && hv.lin.n == 1 && hv.lin.c.num == 0 &&
            hv.lin.terms[0].coeff.num == hv.lin.terms[0].coeff.den)
            e = handles_find(&st->handles, hv.lin.terms[0].var);
        if (!e || e->kind != HK_JOIN) {
            /* unknown handle (e.g. an i64 parameter): no protocol claims —
             * join returns an unconstrained value, detach 0 (sound) */
            lin_free(&hv.lin);
            if (!is_join) return sym_lin(lin_const(rat_int(0)));
            char rv[64]; snprintf(rv, sizeof rv, "__c%d", g_call_ctr++);
            return sym_lin(lin_var(rv));
        }
        if (e->state != 0) {
            char label[300];
            snprintf(label, sizeof label, "%s след %s по същия handle",
                     is_join ? "join" : "detach",
                     e->state == 1 ? "join" : "detach");
            push_protocol_violation(st, ob, label);
        } else {
            e->state = is_join ? 1 : 2;
            if (is_join) wf_check_join(st, ob, e);
            /* W2.4: читателят е завършил — освободи една споделба. detach НЕ
             * декрементира (работникът може още да чете), значи free на
             * споделен свят остава обречено — честно, не тихо прието. */
            if (is_join && e->world_arg) {
                HandleEntry *sw = handles_find(&st->handles, e->world_arg);
                if (sw && sw->kind == HK_WORLD && sw->n_shared > 0) sw->n_shared--;
            }
        }
        if (!is_join) { lin_free(&hv.lin); return sym_lin(lin_const(rat_int(0))); }
        lin_free(&hv.lin);
        return sym_lin(lin_clone(&e->result));
    }

    if (strcmp(nm, "chan_new") == 0) {
        Sym cap = se_from_ast(call->args.data[0], &st->env, &st->vlen, &st->reads);
        int ccap = -1;   /* M24: constant capacity only; symbolic = unknown */
        if (!cap.nonlinear && cap.lin.n == 0 && cap.lin.c.den == 1) {
            ccap = (int)cap.lin.c.num;
            if (ccap < 1) ccap = 1;   /* runtime clamps: baga_chan_new M1 */
        }
        lin_free(&cap.lin);
        char hv[64]; snprintf(hv, sizeof hv, "__ch%d", g_handle_ctr++);
        handles_set(&st->handles, hv, HK_CHAN, 0, NULL);
        HandleEntry *e = handles_find(&st->handles, hv);
        if (e) e->wf_cap = ccap;
        return sym_lin(lin_var(hv));
    }

    /* chan_send / chan_recv / chan_close */
    Sym cv = se_from_ast(call->args.data[0], &st->env, &st->vlen, &st->reads);
    HandleEntry *e = NULL;
    if (!cv.nonlinear && cv.lin.n == 1 && cv.lin.c.num == 0 &&
        cv.lin.terms[0].coeff.num == cv.lin.terms[0].coeff.den)
        e = handles_find(&st->handles, cv.lin.terms[0].var);
    if (e && e->kind != HK_CHAN) e = NULL;   /* not a channel: no claims */
    lin_free(&cv.lin);

    if (strcmp(nm, "chan_close") == 0) {
        if (e) e->state = 1;   /* idempotent */
        return sym_lin(lin_const(rat_int(0)));
    }
    if (strcmp(nm, "chan_recv") == 0) {
        char rv[64]; snprintf(rv, sizeof rv, "__c%d", g_call_ctr++);
        /* M16: a received payload provably satisfies every content axiom
         * anchored on this channel */
        char kbuf[64];
        const char *key = resolve_key(call->args.data[0], st, kbuf, sizeof kbuf);
        if (key)
            for (int i = 0; i < st->ax.n; i++) {
                if (st->ax.a[i].is_sorted || strcmp(st->ax.a[i].vec, key) != 0) continue;
                if (st->ax.a[i].world_kind) {
                    /* W2.8: договорът от канала се предава на let-името, под
                     * което ще се регистрира HK_WORLD (виж g_recv_world_kind в
                     * symexec_stmts) — ghost-ите са по ИМЕ на променливата, а
                     * recv връща вътрешна __cN стойност. */
                    g_recv_world_kind = st->ax.a[i].world_kind;
                    snprintf(g_recv_world_chan, sizeof g_recv_world_chan, "%s", key);   /* W2.12 */
                    continue;
                }
                cl_push(&st->path, axiom_instantiate(&st->ax.a[i], rv));
            }
        wf_check_recv(st, ob, e);   /* M19: wait-for */
        return sym_lin(lin_var(rv));   /* otherwise unconstrained payload */
    }
    /* chan_send */
    if (e) { wf_check_send(st, ob, e); e->n_send++; }   /* M24: full-buffer */
    Sym v = se_from_ast(call->args.data[1], &st->env, &st->vlen, &st->reads);
    /* M16: the payload must provably satisfy every content axiom anchored on
     * this channel; an axiom that cannot be discharged is dropped (M3 rule —
     * downstream receives then honestly get nothing) */
    {
        char kbuf[64];
        const char *key = resolve_key(call->args.data[0], st, kbuf, sizeof kbuf);
        if (key && st->ax.n > 0) {
            AxiomList kept; ax_init(&kept);
            for (int i = 0; i < st->ax.n; i++) {
                ElemAxiom *a = &st->ax.a[i];
                if (strcmp(a->vec, key) == 0 && !a->is_sorted) {
                    /* W2.8: каналният договор за свят оцелява само ако
                     * payload-ът НАИСТИНА е такъв свят; иначе отпада и
                     * получателят честно не получава нищо (M16 drop rule). */
                    int survives = a->world_kind
                        ? world_axiom_holds(st, call->args.data[1], a->world_kind)
                        : axiom_holds_for_value(a, &v, st);
                    if (survives)
                        ax_push(&kept, a->vec, a->cmp, lin_clone(&a->rhs), a->is_sorted, a->world_kind);
                    /* else: dropped (sound) */
                } else {
                    ax_push(&kept, a->vec, a->cmp, lin_clone(&a->rhs), a->is_sorted, a->world_kind);
                }
            }
            ax_free(&st->ax);
            st->ax = kept;
        }
    }
    lin_free(&v.lin);
    /* W0/W2.4: изпращане на свят през канал. Mutable = прехвърляне на
     * собствеността (move, Pony iso) — всяка по-нататъшна локална употреба е
     * use-after-move. Frozen = споделена read-only capability: локалните
     * четения остават легални, но приемникът не може да се join-ва, затова
     * споделбата е перманентна и `world_free` след това е ОБРОЧЕНО.
     * W2.8: ръчката минава СЛЕД аксиомния филтър — `world_owned(c[*])` се
     * проверява върху състоянието ПРЕДИ предаването (иначе move-ът вече е
     * направил света state 3 и договорът отпада, макар да е спазен). */
    if (call->args.len >= 2 && call->args.data[1] && call->args.data[1]->kind == NODE_IDENT) {
        HandleEntry *we = handles_find(&st->handles, call->args.data[1]->name);
        if (we && we->kind == HK_WORLD)
            world_handoff(st, ob, we, "chan_send", 0, 0, e ? e->var : NULL);   /* W2.6/W2.12 */
    }
    char rv[64]; snprintf(rv, sizeof rv, "__c%d", g_call_ctr++);
    if (e && e->state == 1) {
        /* known closed: send definitely fails → s == -1 */
        Lin s = lin_var(rv);
        cl_push(&st->path, mk_cons(lin_clone(&s), C_LE, rat_int(-1)));   /* s <= -1 */
        Lin neg = lin_neg(&s);
        cl_push(&st->path, mk_cons(neg, C_LE, rat_int(1)));              /* -s <= 1 */
        lin_free(&s);
    } else {
        path_push_ge(&st->path, rv, -1);   /* s >= -1 */
        path_push_le(&st->path, rv, 0);    /* s <= 0  */
    }
    return sym_lin(lin_var(rv));
}

/* MEM-2: drop(x) — consume the alloc→drop protocol state of an owned buffer
 * (Vec/Map/bytes/fn). Keyed by the SOURCE variable name, not by a Lin term:
 * these values are opaque (nonlinear) in the scalar machinery. A second drop
 * on a live path is a kind-3 protocol violation (REFUTED with the path
 * witness); on a dead path it is vacuously PROVEN, which is correct. An
 * untracked value (e.g. a fn-typed local, which is never registered) makes
 * no claims — the checker has already validated the type. */
void eval_drop_call(Node *call, State *st, Obligations *ob) {
    Node *a = call->args.data[0];
    HandleEntry *e = handles_find(&st->handles, a->name);
    if (!e || e->kind != HK_DROP) return;   /* untracked: no claims */
    if (e->state != 0) {
        char label[300];
        snprintf(label, sizeof label, "повторен drop на '%s'", a->name);
        push_protocol_violation(st, ob, label);
    } else {
        e->state = 1;
    }
}

/* W2.4: предаване на свят на друг изпълнител. ЗАМРАЗЕН свят е споделена
 * read-only capability (Pony val / Verona frozen region): n_shared++, а
 * локалните четения остават легални — свободата да имаш N читателя е цялата
 * идея на freeze. MUTABLE свят е изключителна собственост (Pony iso): move, и
 * второ предаване е ОБРОЧЕНО (иначе двама писатели върху един регион).
 * Връща името на света при joinable споделба (go с гол ident) — join ще
 * декрементира; за go_bg/канал/пакет връща NULL (читателят не се проследява,
 * значи free остава обречено — честно, не тихо). */
static const char *world_handoff(State *st, Obligations *ob, HandleEntry *we,
                                const char *op, int joinable, int delegate_ok,
                                const char *chan) {
    char label[300];
    if (we->state == 2) {
        snprintf(label, sizeof label, "%s на освободен свят '%s'", op, we->var);
        push_protocol_violation(st, ob, label);
        return NULL;
    }
    if (we->state == 3) {
        snprintf(label, sizeof label, "%s на вече преместен свят '%s'", op, we->var);
        push_protocol_violation(st, ob, label);
        return NULL;
    }
    /* W2.7: договор world_mutable е за запис по време на повикването —
     * предаването на света нататък е извън него (собствеността е у извикващия).
     * За world_owned предаването е легално: собствеността вече е тук. */
    if (we->wcontract == 2) {
        snprintf(label, sizeof label,
            "%s на свят с договор world_mutable — собствеността е у извикващия", op);
        push_protocol_violation(st, ob, label);
        return NULL;
    }
    /* W2.5: свързан свят не се предава — owner-ът държи capability и разчита
     * целта да е жива, а приемникът (без spec или с друг договор) може да я
     * освободи или препредаде. Проверката е ПРЕДИ заемането (W2.6): дори
     * договор за четене + join не помага, защото препредаването нататък излиза
     * извън контрола на owner-а. */
    {
        const char *owner = wlink_owner_of(&st->handles, we->var);
        if (owner) {
            snprintf(label, sizeof label,
                "%s на свързан свят '%s' — capability държи '%s'", op, we->var, owner);
            push_protocol_violation(st, ob, label);
            return NULL;
        }
    }
    if (we->state == 1) {
        /* W2.4/W2.6: заеманото право на четене се ДЕЛЕГИРА само на работник с
         * договор за четене (`requires: world_frozen`) — без него отсрещната
         * страна е sound silence и може да освободи или препредаде света.
         * Делегирането се отчита ТРАНЗИТИВНО: n_shared++ тук, а join-ът на
         * под-работника го връща; ако делегиращият свърши без join, изходната
         * проверка (drain_returns) го оборва — така животът на внука е вложен
         * в живота на родителската споделба и owner-ът може да free-не след
         * своя join. Канал/go_bg/пакет не са joinable ⇒ не се делегират. */
        if (we->borrowed) {
            if (!joinable) {
                snprintf(label, sizeof label,
                    "%s на заеман свят '%s' — не може да се join-ва (канал, go_bg или пакет), "
                    "значи делегирането не е транзитивно отчетено", op, we->var);
                push_protocol_violation(st, ob, label);
                return NULL;
            }
            if (!delegate_ok) {
                snprintf(label, sizeof label,
                    "%s на заеман свят '%s' — получателят няма договор за четене "
                    "(requires: world_frozen), значи може да го освободи или препредаде",
                    op, we->var);
                push_protocol_violation(st, ob, label);
                return NULL;
            }
            we->n_shared++;
            if (chan) chshare_add(we, chan, 1);   /* W2.12: споделба по канала */
            return we->var;
        }
        we->n_shared++;
        if (chan) chshare_add(we, chan, 1);   /* W2.12 */
        return joinable ? we->var : NULL;
    }
    we->state = 3;
    return NULL;
}

/* W1.5: обхожда израз за HK_WORLD идентификатори (рекурсия по аргументите —
 * покрива cell2 пакети и вложени извиквания) и прилага world_handoff върху
 * всеки. Joinable е само голо ident-предаване; delegate_ok важи само за него
 * (в пакет не знаем кой параметър получава света). */
static const char *world_move_scan_rec(Node *e, State *st, Obligations *ob,
                                       int joinable, int delegate_ok, const char *skip) {
    if (!e) return NULL;
    switch (e->kind) {
    case NODE_IDENT: {
        if (skip && strcmp(e->name, skip) == 0) return NULL;   /* W2.7: owned — вече прехвърлен */
        HandleEntry *h = handles_find(&st->handles, e->name);
        if (!h || h->kind != HK_WORLD) return NULL;
        return world_handoff(st, ob, h, "предаване", joinable, delegate_ok, NULL);
    }
    case NODE_CALL:
        for (int i = 0; i < e->args.len; i++) world_move_scan_rec(e->args.data[i], st, ob, 0, 0, NULL);
        return NULL;
    case NODE_BINARY:
        world_move_scan_rec(e->left, st, ob, 0, 0, NULL);
        world_move_scan_rec(e->right, st, ob, 0, 0, NULL);
        return NULL;
    default: return NULL;
    }
}
static const char *world_move_scan(State *st, Obligations *ob, Node *arg,
                                  int delegate_ok, int can_join, const char *skip) {
    /* само ГОЛ ident е joinable: при cell2 пакет не знаем кой свят ще бъде
     * прочетен от работника, значи декремент при join не е възможен; go_bg е
     * detached at birth и изобщо няма handle за join. */
    return world_move_scan_rec(arg, st, ob,
                              can_join && arg && arg->kind == NODE_IDENT, delegate_ok, skip);
}

/* W0: world_* преходи на HK_WORLD ghost състоянието (като eval_drop_call,
 * но с четири състояния): 0 mutable → 1 frozen → 2 freed; 3 = moved
 * (собствеността е прехвърлена през chan_send). Нарушение на жив път →
 * ОБРОЧЕНО с контрапример; на мъртъв път — vacuously PROVEN. Непроследен
 * handle (потребителски shadow, чужд ident) не прави твърдения. */
void eval_world_call(Node *call, State *st, Obligations *ob) {
    if (!is_world_op_call(call)) return;
    const char *op = call->callee->name;
    const char *wn = call->args.data[0]->name;
    HandleEntry *e = handles_find(&st->handles, wn);
    if (!e || e->kind != HK_WORLD) return;
    char label[300];
    const char *viol = NULL;
    /* W2.7: договорът world_mutable дава изключителен ЗАПИС в региона по време
     * на повикването, но собствеността остава у извикващия — freeze, free и
     * връзките променят правата трайно (извън обхвата на договора), затова са
     * нарушения. За world_frozen това вече го покрива borrowed-правилото, а
     * world_owned позволява всичко. */
    if (e->wcontract == 2 &&
        (strcmp(op, "world_freeze") == 0 || strcmp(op, "world_free") == 0 ||
         strcmp(op, "world_link") == 0 || strcmp(op, "world_unlink") == 0)) {
        snprintf(label, sizeof label,
            "%s върху свят с договор world_mutable — собствеността е у извикващия", op);
        push_protocol_violation(st, ob, label);
        return;
    }
    if (strcmp(op, "world_freeze") == 0) {
        if (e->state == 0) {
            e->state = 1;
        } else if (e->state == 1) {
            snprintf(label, sizeof label, "повторен freeze на '%s'", wn); viol = label;
        } else if (e->state == 2) {
            snprintf(label, sizeof label, "freeze след world_free на '%s'", wn); viol = label;
        } else {
            snprintf(label, sizeof label, "freeze след move на '%s'", wn); viol = label;
        }
    } else if (strcmp(op, "world_free") == 0) {
        if (e->state == 0 || e->state == 1) {
            /* W2.12: каналните споделби изтичат, ако каналите са затворени и
             * проследими и всички носители са join-нати — тогава никой не може
             * нито да чете, нито да получи света по-късно. */
            chshare_expire(&st->handles, e);
            const char *owner = wlink_owner_of(&st->handles, e->var);   /* W2.5 */
            /* W2.4: получател с `requires: world_frozen` държи ЗАЕМАН свят —
             * собствеността е у извикващия, значи free тук би съблякъл региона
             * под останалите читатели (и под самия извикващ). */
            if (e->borrowed) {
                snprintf(label, sizeof label,
                    "world_free на заеман свят '%s' — собствеността е у извикващия",
                    e->var);
                viol = label;
            } else if (e->n_shared > 0) {
                if (e->n_chs > 0) {
                    /* W2.12: оцелели са канални споделби — кажи за кой канал и
                     * защо не изтича (жив/откачен носител или непроследим). */
                    snprintf(label, sizeof label,
                        "world_free на споделен свят '%s' — канал '%s' още го държи "
                        "(join на всички носители, chan_close и никакъв go_bg/пакет/връщане "
                        "преди free)",
                        e->var, e->chs[0]);
                } else {
                    snprintf(label, sizeof label,
                        "world_free на споделен свят '%s' с %d незавършен(и) читател(и) — join преди free",
                        e->var, e->n_shared);
                }
                viol = label;
            } else if (owner) {
                /* W2.5: целта на връзката трябва да я надживее — иначе owner-ът
                 * остава с висящ handle (world_linked/wvec_* → use-after-free). */
                snprintf(label, sizeof label,
                    "world_free на свързан свят '%s' — capability държи '%s' (unlink преди free)",
                    e->var, owner);
                viol = label;
            } else {
                e->state = 2;
                wlink_clear(e);   /* връзките на освободения owner умират с него */
            }
        } else if (e->state == 2) {
            snprintf(label, sizeof label, "повторен world_free на '%s'", wn); viol = label;
        } else {
            snprintf(label, sizeof label, "world_free след move на '%s'", wn); viol = label;
        }
    } else if (strcmp(op, "world_link") == 0 || strcmp(op, "world_unlink") == 0) {
        /* W2.5: capability връзка owner → target. Owner-ът трябва да е жив и
         * МУТАБЕЛЕН (графът се строи преди freeze — така таблицата не се пипа
         * от две нишки); target-ът — жив (не free, не move). Цикъл е ОБРОЧЕНО:
         * прави и двата свята неосвободими (leak, не UB — затова при изчерпан
         * бюджет за обхождане мълчим, никога не измисляме). */
        Node *ta = call->args.len > 1 ? call->args.data[1] : NULL;
        HandleEntry *t = (ta && ta->kind == NODE_IDENT) ? handles_find(&st->handles, ta->name) : NULL;
        if (t && t->kind != HK_WORLD) t = NULL;
        if (strcmp(op, "world_unlink") == 0) {
            if (e->state != 0) {
                snprintf(label, sizeof label,
                    "world_unlink върху %s свят '%s' — графът се строи преди freeze",
                    e->state == 1 ? "замразен" : e->state == 2 ? "освободен" : "преместен", wn);
                viol = label;
            } else if (t) {
                wlink_del(e, t->var);
            }
        } else if (e->state != 0) {
            snprintf(label, sizeof label,
                "world_link върху %s свят '%s' — графът се строи преди freeze",
                e->state == 1 ? "замразен" : e->state == 2 ? "освободен" : "преместен", wn);
            viol = label;
        } else if (!t) {
            /* неследен target (параметър или израз) — честно мълчание */
        } else if (t->state == 2 || t->state == 3) {
            snprintf(label, sizeof label, "world_link към %s свят '%s'",
                     t->state == 2 ? "освободен" : "преместен", t->var);
            viol = label;
        } else if (t == e || wlink_reaches(&st->handles, t->var, e->var)) {
            snprintf(label, sizeof label,
                "цикъл между светове: '%s' и '%s' — графът трябва да е ацикличен",
                e->var, t->var);
            viol = label;
        } else {
            wlink_add(e, t->var);
        }
    } else if (is_world_scratch_call(call)) {
        /* W2.3: вътрешна променяемост — кешът е прикачен към handle-а, не към
         * региона, затова ЗАМРАЗЕН свят го допуска (dump-ът и четенията са
         * непроменени). Нарушение е само след free/move: там handle-ът вече не
         * е твой. */
        if (e->state == 2) {
            snprintf(label, sizeof label, "%s след world_free на '%s'", op, wn); viol = label;
        } else if (e->state == 3) {
            snprintf(label, sizeof label, "%s след move на '%s'", op, wn); viol = label;
        }
    } else {
        /* world_alloc / wvec_push — check-only мутатори: не сменят
         * състоянието, само изискват mutable (затова са легални и в цикли,
         * за разлика от freeze/free). */
        if (e->state == 1) {
            snprintf(label, sizeof label, "mutate-after-freeze: %s върху замразен '%s'", op, wn); viol = label;
        } else if (e->state == 2) {
            snprintf(label, sizeof label, "%s след world_free на '%s'", op, wn); viol = label;
        } else if (e->state == 3) {
            snprintf(label, sizeof label, "%s след move на '%s'", op, wn); viol = label;
        }
    }
    if (viol) push_protocol_violation(st, ob, viol);
}

/* MEM-2: flag ident USES of an already-dropped buffer anywhere in a
 * statement-level expression (call args, operands). `seen` dedups per
 * statement, so one statement yields at most one violation per variable.
 * W0: същият проход покрива и светове в state 2 (freed) / 3 (moved);
 * world_* op-викове се пропускат — техните нарушения ги лови
 * eval_world_call (иначе едно нещо дава два етикета). */
static void check_drop_uses_rec(Node *e, State *st, Obligations *ob, const char **seen, int *nseen) {
    if (!e) return;
    switch (e->kind) {
    case NODE_IDENT: {
        HandleEntry *h = handles_find(&st->handles, e->name);
        if (!h) return;
        const char *what = NULL;
        if (h->kind == HK_DROP && h->state == 1) {
            what = "използване след drop на '%s'";
        } else if (h->kind == HK_WORLD && h->state == 2) {
            what = "използване след world_free на '%s'";
        } else if (h->kind == HK_WORLD && h->state == 3) {
            what = "използване след move на '%s'";
        }
        if (!what) return;
        for (int k = 0; k < *nseen; k++) if (strcmp(seen[k], e->name) == 0) return;
        if (*nseen < 16) seen[(*nseen)++] = e->name;
        char label[300];
        snprintf(label, sizeof label, what, e->name);
        push_protocol_violation(st, ob, label);
        return;
    }
    case NODE_CALL:
        if (is_drop_call(e)) return;   /* drop consumes the buffer; not a "use" */
        if (is_world_op_call(e)) return;   /* W0: eval_world_call ги проверява */
        for (int i = 0; i < e->args.len; i++) check_drop_uses_rec(e->args.data[i], st, ob, seen, nseen);
        return;
    case NODE_BINARY:
        check_drop_uses_rec(e->left, st, ob, seen, nseen);
        check_drop_uses_rec(e->right, st, ob, seen, nseen);
        return;
    case NODE_UNARY:
        check_drop_uses_rec(e->operand, st, ob, seen, nseen);
        return;
    default: return;   /* INDEX/FIELD/... are outside the fragment (gated) */
    }
}
void check_drop_uses(Node *e, State *st, Obligations *ob) {
    const char *seen[16]; int nseen = 0;
    check_drop_uses_rec(e, st, ob, seen, &nseen);
}


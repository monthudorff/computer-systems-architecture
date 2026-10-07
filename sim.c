/*
 * sim.c — последовательная имитация работы ресторана. Время идёт в минутах.
 * Календарь хранит будущие события (окончания действий). Основной цикл берёт
 * ближайшее событие, переводит часы, обрабатывает его и даёт координатору,
 * поварам и официантам сделать всё, что стало возможным. Один поток.
 */
#include "restaurant.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- Очереди заказов (односвязные списки) ---------- */

typedef struct {
    Order *head, *tail;
    int n;
} List;

static void push(List *l, Order *o)
{
    o->next = NULL;
    if (l->tail)
        l->tail->next = o;
    else
        l->head = o;
    l->tail = o;
    l->n++;
}

static void unlink_order(List *l, Order *o)
{
    Order *prev = NULL, *p;
    for (p = l->head; p && p != o; p = p->next)
        prev = p;
    if (!p)
        return;
    if (prev)
        prev->next = o->next;
    else
        l->head = o->next;
    if (l->tail == o)
        l->tail = prev;
    o->next = NULL;
    l->n--;
}

/* Заказ, который обслуживается первым: при правиле vip — первый VIP, иначе первый по порядку */
static Order *best(List *l)
{
    Order *p;
    if (P.vip_first)
        for (p = l->head; p; p = p->next)
            if (p->vip)
                return p;
    return l->head;
}

/* ---------- Состояние модели ---------- */

/* Официант: принимает заказ или несёт готовый заказ (что именно — видно по событию) */
typedef struct {
    int busy;
    Order *order;
} Waiter;

/* Повар: выполняет не более одной операции на одной единице оборудования */
typedef struct {
    int busy, since, busy_time, count;
    int unit;                         /* номер занятой единицы оборудования */
    Op op;
} Cook;

/* События календаря */
enum { EV_ARRIVE, EV_TAKEN, EV_DELIVERED, EV_COOKED };
typedef struct {
    int t, type, who;                 /* who — номер официанта или повара */
} Event;

static int now;                                   /* модельное время, мин */
static Event events[2 * MAX_STAFF + 1];           /* календарь: 1 приход + официанты + повара */
static int nevents;
static Waiter waiters[MAX_STAFF];
static Cook cooks[MAX_STAFF];
static int unit_user[EQ_TYPES][MAX_UNITS];        /* кто занял оборудование: номер повара, 0 — свободно */

static List hall;      /* посетители, ждущие официанта */
static List tray;      /* заказы, переданные на кухню и ждущие координатора */
static List cooking;   /* заказы, принятые в работу */
static List pickup;    /* собранные заказы в зоне выдачи */
static Op queue[MAX_QUEUE];                       /* очередь операций кухни */
static int qlen;
static int pass_used;                             /* блюд в зоне выдачи */
static int pass_reserved;                         /* мест, забронированных за заказами в работе */

/* Статистика */
static int arrived, served, cancelled, live;      /* live — посетителей в ресторане */
static long total_time, vip_time;
static int max_time, vip_served;
static long checks, violations;

static void schedule(int t, int type, int who)
{
    Event e = { t, type, who };
    events[nevents++] = e;
}

/* Достать ближайшее событие (из равных по времени — запланированное раньше) */
static int next_event(Event *e)
{
    int i, k = 0;
    if (nevents == 0)
        return 0;
    for (i = 1; i < nevents; i++)
        if (events[i].t < events[k].t)
            k = i;
    *e = events[k];
    memmove(&events[k], &events[k + 1], (size_t)(nevents - k - 1) * sizeof *events);
    nevents--;
    return 1;
}

/* Проверка инварианта: при нарушении — сообщение и учёт в статистике */
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        violations++;
        say(now, "НАРУШЕНИЕ", 0, "%s\n", what);
    }
}

static const Recipe *recipe(const Order *o, int d)
{
    return &P.menu[o->dish[d]];
}

static const Step *op_step(const Op *op)
{
    return &recipe(op->order, op->dish)->steps[op->order->done_steps[op->dish]];
}

/* ---------- Посетители ---------- */

/* Пришёл посетитель: он сразу выбирает блюда (случайно) и ждёт официанта */
static void visitor_arrives(void)
{
    Order *o = calloc(1, sizeof *o);
    int d;

    if (!o) {
        out("Недостаточно памяти\n");
        exit(3);
    }
    o->id = ++arrived;
    o->arrive = now;
    o->vip = rand() % 100 < P.vip_percent;
    o->ndishes = P.order_min + rand() % (P.order_max - P.order_min + 1);
    for (d = 0; d < o->ndishes; d++)
        o->dish[d] = rand() % P.nmenu;
    push(&hall, o);
    live++;
    say(now, "Посетитель", o->id, "пришёл%s и ждёт официанта (в зале ждут: %d)\n",
        o->vip ? " (VIP)" : "", hall.n);

    if (P.visitors == 0 || arrived < P.visitors)
        schedule(now + P.arrive_min + rand() % (P.arrive_max - P.arrive_min + 1), EV_ARRIVE, 0);
    else
        say(now, "Ресторан", 0, "пришли все %d посетителей, приём заказов закрывается\n", P.visitors);
}

/* ---------- Официанты ---------- */

static void waiter_free(int w)
{
    waiters[w].busy = 0;
    waiters[w].order = NULL;
}

/* Свободные официанты сначала разносят готовые заказы, потом принимают новые */
static int waiters_work(void)
{
    int w, acted = 0;
    for (w = 0; w < P.waiters; w++) {
        Waiter *wt = &waiters[w];
        Order *o;
        if (wt->busy)
            continue;
        if ((o = best(&pickup)) != NULL) {
            unlink_order(&pickup, o);
            pass_used -= o->ndishes;          /* блюда забрали — места в зоне выдачи свободны */
            pass_reserved -= o->ndishes;
            say(now, "Официант", w + 1, "забрал заказ #%d из зоны выдачи\n", o->id);
            schedule(now + P.deliver_time, EV_DELIVERED, w);
        } else if ((o = best(&hall)) != NULL) {
            unlink_order(&hall, o);
            say(now, "Официант", w + 1, "подошёл к посетителю #%d\n", o->id);
            schedule(now + P.take_time, EV_TAKEN, w);
        } else {
            continue;
        }
        wt->busy = 1;
        wt->order = o;
        acted = 1;
    }
    return acted;
}

/* Официант принял заказ и передал его на кухню (в лоток координатора) */
static void order_taken(int w)
{
    Order *o = waiters[w].order;
    char list[200] = "";
    size_t len = 0;
    int d;

    for (d = 0; d < o->ndishes && len < sizeof list; d++)
        len += (size_t)snprintf(list + len, sizeof list - len, "%s%s", d ? ", " : "", recipe(o, d)->name);
    say(now, "Официант", w + 1, "принял заказ #%d (%s) и передал его на кухню\n", o->id, list);
    push(&tray, o);
    waiter_free(w);
}

/* Официант донёс заказ: посетитель получает его и уходит */
static void order_delivered(int w)
{
    Order *o = waiters[w].order;
    int t = now - o->arrive;

    /* Инвариант: заказ выдаётся только целиком. Своему посетителю — по построению:
     * заказ хранится в одной структуре со своим посетителем */
    check(o->ready == o->ndishes, "заказ выдан до готовности всех блюд");
    say(now, "Официант", w + 1, "выдал заказ #%d посетителю #%d\n", o->id, o->id);
    say(now, "Посетитель", o->id, "получил заказ и ушёл (пробыл в ресторане %d мин)\n", t);
    served++;
    total_time += t;
    if (t > max_time)
        max_time = t;
    if (o->vip) {
        vip_served++;
        vip_time += t;
    }
    live--;
    free(o);
    waiter_free(w);
}

/* ---------- Координатор кухни ---------- */

/* Можно ли вообще выполнить заказ; NULL — можно, иначе причина */
static const char *impossible(const Order *o)
{
    static char why[120];
    int d, s;
    if (P.cooks == 0)
        return "на кухне нет поваров";
    if (o->ndishes > P.pass_cap)
        return "блюд больше, чем мест в зоне выдачи";
    for (d = 0; d < o->ndishes; d++)
        for (s = 0; s < recipe(o, d)->nsteps; s++)
            if (P.equip[recipe(o, d)->steps[s].eq] == 0) {
                snprintf(why, sizeof why, "для блюда «%s» нужно оборудование «%s», а его нет",
                         recipe(o, d)->name, eq_name[recipe(o, d)->steps[s].eq]);
                return why;
            }
    return NULL;
}

static int coordinator(void)
{
    int acted = 0, round, d, n;
    Order *o;

    /* 1. Невыполнимые заказы отменяются. Остальные принимаются в работу, только если в зоне
     *    выдачи хватит мест на все блюда: места бронируются заранее, иначе зона может
     *    заполниться частями разных заказов и ни один нельзя будет собрать (тупик). */
    while ((o = best(&tray)) != NULL) {
        const char *why = impossible(o);
        if (why) {
            unlink_order(&tray, o);
            say(now, "Координатор", 0, "заказ #%d невыполним (%s) — отменён\n", o->id, why);
            say(now, "Посетитель", o->id, "ушёл без заказа\n");
            cancelled++;
            live--;
            free(o);
            acted = 1;
            continue;
        }
        if (pass_reserved + o->ndishes > P.pass_cap)
            break;                            /* ждём, пока освободятся места */
        unlink_order(&tray, o);
        pass_reserved += o->ndishes;
        push(&cooking, o);
        for (n = 0, d = 0; d < o->ndishes; d++)
            n += recipe(o, d)->nsteps;
        say(now, "Координатор", 0, "заказ #%d принят в работу, создано операций: %d\n", o->id, n);
        acted = 1;
    }

    /* 2. Следующая операция каждого блюда ставится в очередь кухни, пока есть место
     *    (операции блюда идут по порядку). При правиле vip сначала — заказы VIP. */
    for (round = P.vip_first ? 0 : 1; round < 2; round++)
        for (o = cooking.head; o; o = o->next) {
            if (P.vip_first && o->vip != (round == 0))
                continue;
            for (d = 0; d < o->ndishes && qlen < P.queue_cap; d++) {
                const Step *st;
                if (o->in_work[d] || o->done_steps[d] == recipe(o, d)->nsteps)
                    continue;
                check(qlen < P.queue_cap, "очередь операций переполнена");
                queue[qlen].order = o;
                queue[qlen].dish = d;
                st = op_step(&queue[qlen]);
                qlen++;
                o->in_work[d] = 1;
                say(now, "Координатор", 0, "«%s» (%s) для блюда «%s» заказа #%d → очередь [%d/%d]\n",
                    st->name, eq_name[st->eq], recipe(o, d)->name, o->id, qlen, P.queue_cap);
                acted = 1;
            }
        }
    return acted;
}

/* ---------- Повара и оборудование ---------- */

static int free_unit(int eq)
{
    int u;
    for (u = 0; u < P.equip[eq]; u++)
        if (unit_user[eq][u] == 0)
            return u;
    return -1;
}

/* Свободный повар берёт из очереди доступную операцию — ту, для которой
 * свободно оборудование (при правиле vip сначала операции VIP-заказов) */
static int cooks_work(void)
{
    int c, i, acted = 0;
    for (c = 0; c < P.cooks; c++) {
        Cook *ck = &cooks[c];
        const Step *st;
        int pick = -1;
        if (ck->busy)
            continue;
        for (i = 0; i < qlen; i++) {
            if (free_unit(op_step(&queue[i])->eq) < 0)
                continue;
            if (pick < 0 || (P.vip_first && queue[i].order->vip && !queue[pick].order->vip))
                pick = i;
        }
        if (pick < 0)
            continue;
        ck->op = queue[pick];
        memmove(&queue[pick], &queue[pick + 1], (size_t)(qlen - pick - 1) * sizeof *queue);
        qlen--;

        st = op_step(&ck->op);
        ck->unit = free_unit(st->eq);
        /* Инварианты: оборудование свободно; операция — следующая по порядку в блюде */
        check(unit_user[st->eq][ck->unit] == 0, "оборудование уже занято другим поваром");
        check(ck->op.order->in_work[ck->op.dish], "операция начата не по порядку");
        unit_user[st->eq][ck->unit] = c + 1;
        ck->busy = 1;
        ck->since = now;
        say(now, "Повар", c + 1, "взял «%s» для блюда «%s» заказа #%d и получил оборудование: %s %d\n",
            st->name, recipe(ck->op.order, ck->op.dish)->name, ck->op.order->id, eq_name[st->eq], ck->unit + 1);
        say(now, "Повар", c + 1, "начал «%s» (%d мин)\n", st->name, st->minutes);
        schedule(now + st->minutes, EV_COOKED, c);
        acted = 1;
    }
    return acted;
}

/* Повар закончил операцию: освобождает оборудование; если это была последняя
 * операция блюда — блюдо готово; если готовы все блюда — заказ собран */
static void op_cooked(int c)
{
    Cook *ck = &cooks[c];
    Order *o = ck->op.order;
    int d = ck->op.dish;
    const Step *st = op_step(&ck->op);
    const Recipe *r = recipe(o, d);

    unit_user[st->eq][ck->unit] = 0;
    ck->busy = 0;
    ck->busy_time += now - ck->since;
    ck->count++;
    say(now, "Повар", c + 1, "закончил «%s» для блюда «%s» заказа #%d и освободил: %s %d\n",
        st->name, r->name, o->id, eq_name[st->eq], ck->unit + 1);
    o->done_steps[d]++;
    o->in_work[d] = 0;
    if (o->done_steps[d] < r->nsteps)
        return;

    /* Инвариант: блюдо готово только после всех своих операций */
    check(o->done_steps[d] == r->nsteps, "блюдо готово раньше, чем выполнены все операции");
    o->ready++;
    pass_used++;
    check(pass_used <= P.pass_cap, "зона выдачи переполнена");
    say(now, "Координатор", 0, "блюдо «%s» заказа #%d готово → зона выдачи [%d/%d]\n",
        r->name, o->id, pass_used, P.pass_cap);
    if (o->ready == o->ndishes) {
        unlink_order(&cooking, o);
        push(&pickup, o);
        say(now, "Координатор", 0, "заказ #%d собран (блюд: %d) и ждёт официанта\n", o->id, o->ndishes);
    }
}

/* ---------- Основной цикл ---------- */

/* Пауза для наглядности: минута модели длится delay_ms миллисекунд.
 * Сигнал прерывает nanosleep — тогда пауза заканчивается досрочно */
static void pause_for(int minutes)
{
    long ms = (long)minutes * P.delay_ms;
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }, rest;
    while (ms > 0 && !stop_signal && nanosleep(&ts, &rest) == -1 && errno == EINTR)
        ts = rest;
}

static void print_params(void)
{
    out("=== Кухня ресторана ===\nПосетителей: %d%s; официантов: %d; поваров: %d; блюд в меню: %d\n",
        P.visitors, P.visitors ? "" : " (без ограничения, остановка — Ctrl+C)", P.waiters, P.cooks, P.nmenu);
    out("Плит %d, печей %d, грилей %d, столов %d; очередь операций %d, зона выдачи %d; приоритет %s; seed %u\n",
        P.equip[STOVE], P.equip[OVEN], P.equip[GRILL], P.equip[TABLE], P.queue_cap, P.pass_cap,
        P.vip_first ? "vip" : "fifo", P.seed);
}

static void print_stats(const char *reason)
{
    int c, total = now > 0 ? now : 1;
    out("\n=== Итоги ===\n");
    out("Завершение: %s\n", reason);
    out("Модельное время: %d мин\n", now);
    out("Посетителей пришло: %d, обслужено: %d, заказов отменено: %d, не обслужено: %d\n",
        arrived, served, cancelled, live);
    if (served)
        out("Время в ресторане: среднее %ld мин, наибольшее %d мин\n", total_time / served, max_time);
    if (vip_served && served > vip_served)
        out("  VIP: %ld мин, остальные: %ld мин\n", vip_time / vip_served,
            (total_time - vip_time) / (served - vip_served));
    for (c = 0; c < P.cooks; c++)
        out("Повар %d: операций %d, занят %d%% времени\n", c + 1, cooks[c].count,
            (cooks[c].busy_time + (cooks[c].busy ? now - cooks[c].since : 0)) * 100 / total);
    out("Проверок инвариантов: %ld, нарушений: %ld\n", checks, violations);
}

int run_simulation(void)
{
    Event e;
    const char *reason;
    int code = 0, changed;

    print_params();
    say(0, "Ресторан", 0, "открыт\n");
    schedule(0, EV_ARRIVE, 0);

    while (!stop_signal && next_event(&e)) {
        if (e.t > now) {
            pause_for(e.t - now);
            if (stop_signal)
                break;
            now = e.t;
        }
        switch (e.type) {
        case EV_ARRIVE:    visitor_arrives(); break;
        case EV_TAKEN:     order_taken(e.who); break;
        case EV_DELIVERED: order_delivered(e.who); break;
        case EV_COOKED:    op_cooked(e.who); break;
        }
        /* Участники действуют, пока кто-то может что-то сделать:
         * действие одного открывает возможности для другого */
        do {
            changed = coordinator();
            changed |= cooks_work();
            changed |= waiters_work();
        } while (changed);
    }

    if (stop_signal) {
        say(now, "Ресторан", 0, "получен сигнал %s — моделирование остановлено\n",
            stop_signal == SIGINT ? "SIGINT (Ctrl+C)" : "SIGTERM");
        reason = P.visitors == 0 ? "остановлено пользователем (режим без ограничения)"
                                 : "прервано пользователем до окончания обслуживания";
        code = P.visitors == 0 ? 0 : 1;
    } else if (live > 0) {
        reason = "тупик: событий больше нет, но посетители не обслужены";
        code = 3;
    } else {
        say(now, "Ресторан", 0, "все принятые посетители обслужены, ресторан закрыт\n");
        reason = cancelled ? "приём закрыт, все посетители обслужены, невыполнимые заказы отменены"
                           : "приём закрыт, все посетители обслужены";
    }
    if (violations)
        code = 3;
    print_stats(reason);
    return code;
}

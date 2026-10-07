/*
 * main.c — параметры модели, чтение файла настроек, аргументы командной
 * строки, обработка сигналов и вывод (dprintf в консоль и журнал).
 */
#include "restaurant.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

const char *eq_name[EQ_TYPES] = { "плита", "печь", "гриль", "стол" };
const char *eq_key[EQ_TYPES] = { "stove", "oven", "grill", "table" };

Params P;                              /* параметры модели */
volatile sig_atomic_t stop_signal = 0; /* номер пришедшего сигнала (0 — не было) */
static int log_fd = -1;                /* дескриптор файла журнала */

/* ---------- Вывод ---------- */

/* Вывод идёт через файловые дескрипторы функциями dprintf/vdprintf:
 * на экран (дескриптор 1) и одновременно в журнал (log_fd) */
static void put(const char *prefix, const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    dprintf(STDOUT_FILENO, "%s", prefix);
    vdprintf(STDOUT_FILENO, fmt, ap);
    if (log_fd >= 0) {
        dprintf(log_fd, "%s", prefix);
        vdprintf(log_fd, fmt, copy);
    }
    va_end(copy);
}

void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    put("", fmt, ap);
    va_end(ap);
}

/* Событие модели: «[12:07] Повар 2: начал ...». Время t — минуты от открытия в 12:00 */
void say(int t, const char *who, int id, const char *fmt, ...)
{
    char prefix[80];
    va_list ap;
    int m = 12 * 60 + t;
    if (id > 0)
        snprintf(prefix, sizeof prefix, "[%02d:%02d] %s %d: ", m / 60 % 24, m % 60, who, id);
    else
        snprintf(prefix, sizeof prefix, "[%02d:%02d] %s: ", m / 60 % 24, m % 60, who);
    va_start(ap, fmt);
    put(prefix, fmt, ap);
    va_end(ap);
}

/* Сообщение об ошибке — в поток ошибок (дескриптор 2) */
static void error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    dprintf(STDERR_FILENO, "Ошибка: ");
    vdprintf(STDERR_FILENO, fmt, ap);
    va_end(ap);
}

/* ---------- Параметры ---------- */

/* Добавить блюдо в меню. Шаги записаны строкой «название:оборудование:минуты ...» */
static int add_dish(const char *name, char *steps)
{
    Recipe *r;
    char *save, *tok;

    if (P.nmenu == MAX_DISHES)
        return -1;
    r = &P.menu[P.nmenu];
    memset(r, 0, sizeof *r);
    snprintf(r->name, NAME_LEN, "%s", name);
    for (tok = strtok_r(steps, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        char op[NAME_LEN], eq[16];
        int minutes, k;
        if (r->nsteps == MAX_STEPS || sscanf(tok, "%31[^:]:%15[^:]:%d", op, eq, &minutes) != 3 || minutes <= 0)
            return -1;
        for (k = 0; k < EQ_TYPES && strcmp(eq, eq_key[k]) != 0; k++)
            ;
        if (k == EQ_TYPES)
            return -1;
        snprintf(r->steps[r->nsteps].name, NAME_LEN, "%s", op);
        r->steps[r->nsteps].eq = k;
        r->steps[r->nsteps].minutes = minutes;
        r->nsteps++;
    }
    if (r->nsteps == 0)
        return -1;
    P.nmenu++;
    return 0;
}

static void set_defaults(void)
{
    static const char *dishes[][2] = {          /* меню по умолчанию */
        { "Стейк", "нарезка:table:2 жарка:grill:8 подача:table:1" },
        { "Борщ", "овощи:table:3 варка:stove:10 заправка:stove:1" },
        { "Пицца", "тесто:table:3 выпечка:oven:7" },
        { "Цезарь", "курица:grill:5 салат:table:3" },
        { "Блины", "тесто:table:2 жарка:stove:6" },
    };
    char steps[200];
    int i;

    memset(&P, 0, sizeof P);
    P.visitors = 15;  P.waiters = 2;  P.cooks = 4;
    P.equip[STOVE] = 2;  P.equip[OVEN] = 1;  P.equip[GRILL] = 2;  P.equip[TABLE] = 2;
    P.order_min = 1;  P.order_max = 3;      /* блюд в заказе */
    P.arrive_min = 1; P.arrive_max = 5;     /* минут между посетителями */
    P.take_time = 2;  P.deliver_time = 1;
    P.queue_cap = 8;  P.pass_cap = 8;
    P.vip_percent = 15;
    P.seed = (unsigned)time(NULL);
    P.delay_ms = 200;
    snprintf(P.log_path, sizeof P.log_path, "restaurant.log");
    for (i = 0; i < 5; i++) {
        snprintf(steps, sizeof steps, "%s", dishes[i][1]);   /* add_dish разрезает строку, нужна копия */
        add_dish(dishes[i][0], steps);
    }
}

/* Разобрать одну строку файла настроек «ключ значение». 0 — успешно */
static int parse_line(char *line, int *menu_from_file)
{
    /* числовые параметры: имя в файле и поле, куда записать значение */
    static const struct { const char *key; int *val; } ints[] = {
        { "visitors", &P.visitors }, { "waiters", &P.waiters }, { "cooks", &P.cooks },
        { "stove", &P.equip[STOVE] }, { "oven", &P.equip[OVEN] }, { "grill", &P.equip[GRILL] },
        { "table", &P.equip[TABLE] }, { "take_time", &P.take_time }, { "deliver_time", &P.deliver_time },
        { "queue_cap", &P.queue_cap }, { "pass_cap", &P.pass_cap }, { "vip_percent", &P.vip_percent },
        { "delay", &P.delay_ms },
    };
    char key[32], rest[400];
    int a, b;

    if (sscanf(line, "%31s %399[^\n]", key, rest) < 1 || key[0] == '#')
        return 0;                                   /* пустая строка или комментарий */
    if (strcmp(key, "dish") == 0) {
        char name[NAME_LEN], steps[400];
        if (!*menu_from_file) {                     /* первое блюдо из файла заменяет меню */
            P.nmenu = 0;
            *menu_from_file = 1;
        }
        return sscanf(rest, "%31s %399[^\n]", name, steps) == 2 ? add_dish(name, steps) : -1;
    }
    if (strcmp(key, "priority") == 0) {
        if (strcmp(rest, "fifo") != 0 && strcmp(rest, "vip") != 0)
            return -1;
        P.vip_first = strcmp(rest, "vip") == 0;
        return 0;
    }
    if (strcmp(key, "log") == 0)
        return snprintf(P.log_path, sizeof P.log_path, "%s", rest) > 0 ? 0 : -1;
    if (strcmp(key, "order") == 0 || strcmp(key, "arrive") == 0) {
        if (sscanf(rest, "%d %d", &a, &b) != 2)
            return -1;
        if (key[0] == 'o') { P.order_min = a; P.order_max = b; }
        else               { P.arrive_min = a; P.arrive_max = b; }
        return 0;
    }
    if (sscanf(rest, "%d", &a) != 1)
        return -1;
    if (strcmp(key, "seed") == 0) {
        P.seed = (unsigned)a;
        return 0;
    }
    for (b = 0; b < (int)(sizeof ints / sizeof ints[0]); b++)
        if (strcmp(key, ints[b].key) == 0) {
            *ints[b].val = a;
            return 0;
        }
    return -1;
}

/* Прочитать файл настроек системными вызовами open/read */
static int load_config(const char *path)
{
    char buf[8192], *line, *save;
    ssize_t n, total = 0;
    int fd, menu_from_file = 0;

    if ((fd = open(path, O_RDONLY)) < 0) {
        error("не удалось открыть %s: %s\n", path, strerror(errno));
        return -1;
    }
    while ((n = read(fd, buf + total, sizeof buf - 1 - (size_t)total)) > 0)
        total += n;
    close(fd);
    if (n < 0) {
        error("ошибка чтения %s\n", path);
        return -1;
    }
    buf[total] = '\0';
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (parse_line(line, &menu_from_file) != 0) {
            error("%s: неверная строка «%s»\n", path, line);
            return -1;
        }
    }
    return 0;
}

/* Проверка допустимости параметров */
static int check_params(void)
{
    int ok = P.waiters >= 1 && P.waiters <= MAX_STAFF && P.cooks >= 0 && P.cooks <= MAX_STAFF &&
             P.visitors >= 0 && P.take_time >= 0 && P.deliver_time >= 0 &&
             P.order_min >= 1 && P.order_min <= P.order_max && P.order_max <= MAX_ORDER &&
             P.arrive_min >= 0 && P.arrive_min <= P.arrive_max && P.arrive_max >= 1 &&
             P.queue_cap >= 1 && P.queue_cap <= MAX_QUEUE && P.pass_cap >= 1;
    for (int i = 0; i < EQ_TYPES; i++)
        ok = ok && P.equip[i] >= 0 && P.equip[i] <= MAX_UNITS;
    if (!ok)
        error("недопустимые параметры: официантов 1..%d, поваров 0..%d, оборудования 0..%d, "
              "блюд в заказе 1..%d, очередь 1..%d, остальные числа неотрицательные, мин <= макс\n",
              MAX_STAFF, MAX_STAFF, MAX_UNITS, MAX_ORDER, MAX_QUEUE);
    return ok ? 0 : -1;
}

/* ---------- Сигналы ---------- */

/* Обработчик Ctrl+C (SIGINT) и SIGTERM: только запоминает сигнал,
 * остановку с выводом итогов выполняет основной цикл моделирования */
static void on_signal(int sig)
{
    if (stop_signal)
        _exit(130);         /* повторный Ctrl+C — выйти сразу */
    stop_signal = sig;
}

static void usage(void)
{
    out("Использование: restaurant [-c файл настроек] [-n посетителей (0 — до Ctrl+C)] [-s seed]\n"
        "                  [-d задержка модельной минуты, мс] [-l файл журнала]\n");
}

/* Перевести строку в целое число; -1 — строка не является числом */
static int to_int(const char *s, int *v)
{
    char *end;
    long x = strtol(s, &end, 10);
    if (end == s || *end != '\0')
        return -1;
    *v = (int)x;
    return 0;
}

int main(int argc, char **argv)
{
    const char *config = NULL, *arg_n = NULL, *arg_s = NULL, *arg_d = NULL;
    struct sigaction sa;
    int opt, code, seed;

    set_defaults();
    while ((opt = getopt(argc, argv, "c:n:s:d:l:h")) != -1) {
        switch (opt) {
        case 'c': config = optarg; break;
        case 'n': arg_n = optarg; break;
        case 's': arg_s = optarg; break;
        case 'd': arg_d = optarg; break;
        case 'l': snprintf(P.log_path, sizeof P.log_path, "%s", optarg); break;
        case 'h': usage(); return 0;
        default:  usage(); return 2;
        }
    }
    /* Ключи командной строки важнее файла настроек, поэтому применяются после него */
    if (config && load_config(config) != 0)
        return 2;
    if ((arg_n && to_int(arg_n, &P.visitors) != 0) || (arg_d && to_int(arg_d, &P.delay_ms) != 0) ||
        (arg_s && to_int(arg_s, &seed) != 0)) {
        error("после -n, -s и -d нужно целое число\n");
        return 2;
    }
    if (arg_s)
        P.seed = (unsigned)seed;
    if (check_params() != 0)
        return 2;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;        /* без SA_RESTART: сон (nanosleep) прерывается сигналом сразу */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Журнал создаётся заново при каждом запуске */
    log_fd = open(P.log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0) {
        error("не удалось создать журнал %s: %s\n", P.log_path, strerror(errno));
        return 2;
    }
    srand(P.seed);
    code = run_simulation();
    out("Журнал сохранён в %s\n", P.log_path);
    if (close(log_fd) < 0)
        error("ошибка при закрытии журнала\n");
    return code;
}

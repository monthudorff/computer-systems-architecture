/*
 * restaurant.h — общие типы и функции модели «Кухня ресторана».
 * ИДЗ №1 по АКОС, вариант 32. Кирпаль Андрей Николаевич, БПИ255.
 */
#ifndef RESTAURANT_H
#define RESTAURANT_H

#include <signal.h>

#define MAX_DISHES  16    /* блюд в меню */
#define MAX_STEPS    6    /* операций в одном блюде */
#define MAX_ORDER    5    /* блюд в одном заказе */
#define MAX_STAFF   32    /* официантов и поваров (каждых) */
#define MAX_UNITS   16    /* единиц оборудования одного типа */
#define MAX_QUEUE  256    /* наибольшая вместимость очереди операций */
#define NAME_LEN    32

/* Типы оборудования */
enum { STOVE, OVEN, GRILL, TABLE, EQ_TYPES };
extern const char *eq_name[EQ_TYPES];   /* «плита», «печь», ... */
extern const char *eq_key[EQ_TYPES];    /* stove, oven, ... — для файла настроек */

/* Операция рецепта: название, оборудование и длительность в минутах */
typedef struct {
    char name[NAME_LEN];
    int eq;
    int minutes;
} Step;

/* Блюдо меню — последовательность операций */
typedef struct {
    char name[NAME_LEN];
    int nsteps;
    Step steps[MAX_STEPS];
} Recipe;

/* Входные параметры модели */
typedef struct {
    int visitors;                 /* сколько посетителей придёт; 0 — без ограничения (до Ctrl+C) */
    int waiters, cooks;
    int equip[EQ_TYPES];          /* сколько единиц оборудования каждого типа */
    int nmenu;
    Recipe menu[MAX_DISHES];
    int order_min, order_max;     /* сколько блюд в заказе */
    int arrive_min, arrive_max;   /* интервал между приходами посетителей, мин */
    int take_time;                /* официант принимает заказ, мин */
    int deliver_time;             /* официант несёт заказ посетителю, мин */
    int queue_cap;                /* вместимость очереди операций кухни */
    int pass_cap;                 /* вместимость зоны готовых блюд (в блюдах) */
    int vip_first;                /* правило приоритета: 0 — fifo, 1 — сначала VIP */
    int vip_percent;              /* доля VIP-посетителей, % */
    unsigned seed;                /* начальное значение генератора случайных чисел */
    int delay_ms;                 /* сколько мс показывается одна модельная минута */
    char log_path[128];           /* файл журнала */
} Params;

/* Посетитель вместе со своим заказом: у каждого посетителя ровно один заказ,
 * номер заказа совпадает с номером посетителя */
typedef struct Order {
    int id;
    int vip;
    int arrive;                   /* время прихода */
    int ndishes;
    int dish[MAX_ORDER];          /* номера блюд в меню */
    int done_steps[MAX_ORDER];    /* сколько операций каждого блюда выполнено */
    int in_work[MAX_ORDER];       /* 1 — очередная операция блюда в очереди или готовится */
    int ready;                    /* сколько блюд готово */
    struct Order *next;           /* следующий в очереди (заказ всегда стоит не больше чем в одной) */
} Order;

/* Операция конкретного блюда конкретного заказа */
typedef struct {
    Order *order;
    int dish;                     /* номер блюда в заказе */
} Op;

/* main.c */
extern Params P;
extern volatile sig_atomic_t stop_signal;
void say(int t, const char *who, int id, const char *fmt, ...);
void out(const char *fmt, ...);

/* sim.c */
int run_simulation(void);

#endif

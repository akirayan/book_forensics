#include <stdlib.h>
#include <string.h>
#include "stack.h"


static ARENA *arena_new(void)
{
    ARENA *a = (ARENA *)calloc(1, sizeof(ARENA));
    return a;   // head == NULL
}


static void *arena_alloc(ARENA *a, size_t size)
{
    if (!a || size == 0)
        return NULL;

    void *p = malloc(size);
    if (!p)
        return NULL;

    ARENA_BLOCK *b = (ARENA_BLOCK *)malloc(sizeof(ARENA_BLOCK));
    if (!b) {
        free(p);
        return NULL;
    }

    b->ptr  = p;
    b->next = a->head;
    a->head = b;

    return p;
}


static char *arena_strdup(ARENA *a, const char *s)
{
    if (!a || !s)
        return NULL;

    size_t len = strlen(s) + 1;
    char *p = (char *)arena_alloc(a, len);
    if (!p)
        return NULL;

    memcpy(p, s, len);
    return p;
}



static void arena_free_all(ARENA *a)
{
    if (!a)
        return;

    ARENA_BLOCK *b = a->head;
    while (b) {
        ARENA_BLOCK *next = b->next;
        free(b->ptr);
        free(b);
        b = next;
    }

    free(a);
}








STACK *stack_new(void)
{
    STACK *s = calloc(1, sizeof(STACK));
    if (!s) return NULL;

    s->arena = arena_new();   // ← ここで生成
    if (!s->arena) {
        free(s);
        return NULL;
    }

    return s;
}



void stack_free(STACK *s)
{
    if (!s) return;

    // ノード構造を解放
    while (s->top) {
        STACK_NODE *n = s->top;
        s->top = n->next;
        free(n);
    }

    // arena が管理する全メモリを一括解放
    arena_free_all(s->arena);

    free(s);
}



void stack_push(STACK *s, const char *name)
{
    if (!s || !name) return;

    STACK_NODE *n = malloc(sizeof(STACK_NODE));
    if (!n) return;

    n->data = arena_strdup(s->arena, name);
    n->next = s->top;
    s->top  = n;
    s->depth++;
}

char *stack_pop(STACK *s)
{
    if (!s || !s->top) return NULL;

    STACK_NODE *n = s->top;
    char *data = n->data;

    s->top = n->next;
    free(n);
    s->depth--;

    return data;
}


void *stack_peek(STACK *s)
{
    if (!s || !s->top) return NULL;
    return s->top->data;
}

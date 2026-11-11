#ifndef EVTX_STACK_H
#define EVTX_STACK_H

#include <stddef.h>

/* Using ARENA to hold all data pointers */
typedef struct _ARENA_BLOCK {
    void *ptr;
    struct _ARENA_BLOCK *next;
} ARENA_BLOCK;

typedef struct _ARENA {
    ARENA_BLOCK *head;
} ARENA;



typedef struct _STACK_NODE {
    void *data;
    struct _STACK_NODE *next;
} STACK_NODE;

typedef struct _STACK {
    STACK_NODE *top;
    ARENA *arena;
    int    depth;
} STACK;


/* lifecycle */
STACK *stack_new(void);
void   stack_free(STACK *s);

/* operations */
void   stack_push(STACK *s, const char *data);
char  *stack_pop(STACK *s);
void  *stack_peek(STACK *s);

#endif

#pragma once

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jnode {
    jtype t;
    const char *key;       /* member name inside an object */
    const char *s;         /* J_STR (unescaped in place, UTF-8) */
    double n;              /* J_NUM, J_BOOL */
    struct jnode *child;   /* first element / member */
    struct jnode *next;
} jnode;

typedef struct jpool jpool;

/* Parses `text` IN PLACE (strings point into it). Returns NULL on error. */
jnode *json_parse(char *text, jpool **pool);
void json_free(jpool *pool);

const jnode *json_get(const jnode *obj, const char *key);
const char *json_str(const jnode *obj, const char *key, const char *dflt);
double json_num(const jnode *obj, const char *key, double dflt);
int json_bool(const jnode *obj, const char *key, int dflt);

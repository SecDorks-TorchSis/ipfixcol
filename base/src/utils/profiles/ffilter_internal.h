
#ifndef _FFILTER_INTERNAL_H
#define _FFILTER_INTERNAL_H

#include "ffilter.h"

typedef struct ff_net_s { ff_ip_t ip; ff_ip_t mask; int ver; } ff_net_t;

/* supported operations */
typedef enum {
	FF_OP_NOT = 1,
	FF_OP_OR,
	FF_OP_AND,
	FF_OP_NOOP,
	FF_OP_EQ,
	FF_OP_LT,
	FF_OP_GT,
	FF_OP_ISSET,
	FF_OP_IN,
	FF_OP_YES,
	FF_OP_EXIST
} ff_oper_t;


/* node of syntax tree */
typedef struct ff_node_s {
	ff_extern_id_t field;         /* field ID */
	char *value;                  /* buffer allocated for data */
	size_t vsize;                 /* size of data in value */
	int type;                     /* data type for value */
	ff_oper_t oper;               /* operation */
	int opts;                     /* mpls stack data selector label, exp or eos */
	int n;                        /* extra identification for mpls<n> variant */

	struct ff_node_s *left;
	struct ff_node_s *right;

} ff_node_t;

#ifndef HAVE_HTONLL
#ifdef WORDS_BIGENDIAN
#   define ntohll(n)    (n)
#   define htonll(n)    (n)
#else
#   define ntohll(n)    ((((uint64_t)ntohl(n)) << 32) | ntohl(((uint64_t)(n)) >> 32))
#   define htonll(n)    ((((uint64_t)htonl(n)) << 32) | htonl(((uint64_t)(n)) >> 32))
#endif
#define HAVE_HTONLL 1
#endif


/* scanner instance */
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void* yyscan_t;
#endif

#ifndef YY_TYPEDEF_YY_BUFFER_STATE
#define YY_TYPEDEF_YY_BUFFER_STATE
typedef struct yy_buffer_state *YY_BUFFER_STATE;
#endif


YY_BUFFER_STATE ff_yy_scan_string(const char *str, yyscan_t yyscanner);
int ff_yyparse(yyscan_t yyscanner, ff_t *filter);
//int lnf_filter_yylex(YYSTYPE *yylval, void *scanner);

/* error function for scanner */
void yyerror(yyscan_t yyscanner, ff_t *filter, char *);

/* conversion from string to numeric/bit value */
int64_t get_unit(char *unit);
int64_t strtoll_unit(char *num, char**endptr);
uint64_t strtoull_unit(char *num, char**endptr);

int str_to_uint(ff_t *filter, char *str, ff_type_t type, char **res, size_t *vsize);
int str_to_int(ff_t *filter, char *str, ff_type_t type, char **res, size_t *vsize);

int str_to_uint64(ff_t *filter, char *str, char **res, size_t *vsize);
int str_to_int64(ff_t *filter, char *str, char **res, size_t *vsize);
int str_to_real(ff_t *filter, char *str, char **res, size_t *vsize);
int str_to_mac(ff_t *filter, char *str, char **res, size_t *vsize);
int str_to_addr(ff_t *filter, char *str, char **res, size_t *vsize);
int str_to_timestamp(ff_t *filter, char *str, char **res, size_t *vsize);

/* add new node into parse tree */
ff_node_t* ff_duplicate_node(ff_node_t* original);
ff_node_t* ff_new_mval(yyscan_t scanner, ff_t *filter, char *valstr, ff_oper_t oper,  ff_node_t* nextptr);
ff_node_t* ff_new_leaf(yyscan_t scanner, ff_t *filter, char *fieldstr, ff_oper_t oper, char *valstr);
ff_node_t* ff_new_node(yyscan_t scanner, ff_t *filter, ff_node_t* left, ff_oper_t oper, ff_node_t* right);

/* evaluate filter */
int ff_eval_node(ff_t *filter, ff_node_t *node, void *rec);

/* release memory allocated by nodes */
void ff_free_node(ff_node_t* node);

/* lex bison prototypes */
int ff2_get_column(yyscan_t yyscanner);
void ff2_set_column(int , yyscan_t);
int ff2_lex_init(yyscan_t *yyscanner);
YY_BUFFER_STATE ff2__scan_string(const char *, yyscan_t yyscanner);
int ff2_parse(yyscan_t yyscanner, ff_t*);
int ff2_lex_destroy(yyscan_t yyscanner);



#endif


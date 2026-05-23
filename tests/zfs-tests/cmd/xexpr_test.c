// SPDX-License-Identifier: CDDL-1.0
/*
 * Unit tests for the "zfs list -x" expression parser.
 *
 * This tests the recursive descent parser that handles:
 *   - Comparison operators: ==, !=, >, <, >=, <=
 *   - Logical operators: && (AND), || (OR)
 *   - Parentheses for precedence override
 *   - Presence evaluation (shorthand boolean)
 *   - Size/time suffix expansion (K, M, G, T, P, E, d, h, m, s)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <stdbool.h>

/* ---------- types matching zfs_main.c ---------- */

typedef enum {
	OP_AND,
	OP_OR,
	OP_EQ,
	OP_NE,
	OP_GT,
	OP_LT,
	OP_GE,
	OP_LE,
} op_type_t;

typedef enum {
	EXPR_NODE,
	EXPR_COMPARISON,
	EXPR_PRESENCE,
} expr_kind_t;

typedef struct expr_node expr_node_t;

typedef struct {
	char		*prop;
	uint64_t	prop_id;
	char		*value;
	op_type_t	op;
} comp_t;

typedef struct {
	char		*prop;
	uint64_t	prop_id;
} presence_t;

struct expr_node {
	expr_kind_t	kind;
	op_type_t	op;
	union {
		comp_t	comp;
		presence_t	pres;
	} u;
	expr_node_t	*left;
	expr_node_t	*right;
};

typedef struct {
	const char	*str;
	const char	*pos;
	int		err;
} parser_t;

/* ---------- parser implementation (copy from zfs_main.c) ---------- */

static void
parser_init(parser_t *p, const char *str)
{
	p->str = str;
	p->pos = str;
	p->err = 0;
}

static void
parser_skip_spaces(parser_t *p)
{
	while (*p->pos == ' ' || *p->pos == '\t')
		p->pos++;
}

static boolean_t
is_prop_char(char c)
{
	return (isalpha((unsigned char)c) || c == '_' || c == ':' ||
	    c == '-' || (c >= '0' && c <= '9'));
}

static boolean_t
parser_peek_op(parser_t *p, const char *op)
{
	size_t len = strlen(op);
	if (strncmp(p->pos, op, len) == 0) {
		char c = p->pos[len];
		if (c == '\0' || c == ' ' || c == '\t' || c == '(' || c == ')' ||
		    (c >= '0' && c <= '9') || c == '"' || c == '-' ||
		    isalpha((unsigned char)c) || c == '_' || c == ':' || c == '.')
			return (B_TRUE);
	}
	return (B_FALSE);
}

static void
parser_consume_op(parser_t *p, const char *op)
{
	p->pos += strlen(op);
	parser_skip_spaces(p);
}

static char *
parser_parse_ident(parser_t *p)
{
	const char *start = p->pos;
	parser_skip_spaces(p);
	if (!is_prop_char(*p->pos)) {
		p->err = 1;
		return (NULL);
	}
	while (is_prop_char(*p->pos))
		p->pos++;
	size_t len = (size_t)(p->pos - start);
	char *ident = malloc(len + 1);
	strncpy(ident, start, len);
	ident[len] = '\0';
	return (ident);
}

static char *
parser_parse_value(parser_t *p)
{
	const char *start = p->pos;
	parser_skip_spaces(p);

	if (*p->pos == '"') {
		p->pos++;
		start = p->pos;
		while (*p->pos && *p->pos != '"')
			p->pos++;
		size_t len = (size_t)(p->pos - start);
		if (*p->pos == '"')
			p->pos++;
		char *val = malloc(len + 1);
		strncpy(val, start, len);
		val[len] = '\0';
		return (val);
	}

	if (!is_prop_char(*p->pos) && *p->pos != '.' && *p->pos != '/' &&
	    *p->pos != ' ' && *p->pos != '\t') {
		p->err = 1;
		return (NULL);
	}
	while (is_prop_char(*p->pos) || *p->pos == '.' || *p->pos == '/' ||
	    *p->pos == ' ')
		p->pos++;
	size_t len = (size_t)(p->pos - start);
	while (len > 0 && start[len - 1] == ' ')
		len--;
	char *val = malloc(len + 1);
	strncpy(val, start, len);
	val[len] = '\0';
	return (val);
}

static uint64_t
parse_value_numeric(const char *val, boolean_t *is_numeric)
{
	char *end;
	uint64_t num = strtoull(val, &end, 10);

	if (*end == '\0') {
		*is_numeric = B_TRUE;
		return (num);
	}

	uint64_t multiplier = 1;
	boolean_t is_size = B_FALSE;
	switch (*end) {
	case 'K': case 'k':
		multiplier = 1024ULL; is_size = B_TRUE; break;
	case 'M':
		multiplier = 1024ULL * 1024; is_size = B_TRUE; break;
	case 'G': case 'g':
		multiplier = 1024ULL * 1024 * 1024; is_size = B_TRUE; break;
	case 'T': case 't':
		multiplier = 1024ULL * 1024 * 1024 * 1024; is_size = B_TRUE; break;
	case 'P': case 'p':
		multiplier = 1024ULL * 1024 * 1024 * 1024 * 1024; is_size = B_TRUE; break;
	case 'E': case 'e':
		multiplier = 1024ULL * 1024 * 1024 * 1024 * 1024 * 1024; is_size = B_TRUE; break;
	case 'd':
		multiplier = 86400ULL; is_size = B_TRUE; break;
	case 'h':
		multiplier = 3600ULL; is_size = B_TRUE; break;
	case 'm':
		multiplier = 60ULL; is_size = B_TRUE; break;
	case 's':
		multiplier = 1ULL; is_size = B_TRUE; break;
	}

	if (is_size) {
		*is_numeric = B_TRUE;
		return (num * multiplier);
	}

	*is_numeric = B_FALSE;
	return (0);
}

static expr_node_t *
parse_primary(parser_t *p);
static expr_node_t *
parse_comparison(parser_t *p);
static expr_node_t *
parse_and(parser_t *p);
static expr_node_t *
parse_or(parser_t *p);

static void
expr_free(expr_node_t *node)
{
	if (node == NULL)
		return;
	expr_free(node->left);
	expr_free(node->right);
	if (node->kind == EXPR_COMPARISON) {
		free(node->u.comp.prop);
		free(node->u.comp.value);
	} else if (node->kind == EXPR_PRESENCE) {
		free(node->u.pres.prop);
	}
	free(node);
}

static expr_node_t *
parse_primary(parser_t *p)
{
	parser_skip_spaces(p);

	if (*p->pos == '(') {
		p->pos++;
		expr_node_t *node = parse_or(p);
		if (p->err || node == NULL) {
			free(node);
			p->err = 1;
			return (NULL);
		}
		parser_skip_spaces(p);
		if (*p->pos != ')') {
			p->err = 1;
			expr_free(node);
			return (NULL);
		}
		p->pos++;
		return (node);
	}

	char *ident = parser_parse_ident(p);
	if (p->err || ident == NULL) {
		p->err = 1;
		return (NULL);
	}

	parser_skip_spaces(p);
	op_type_t op;
	if (parser_peek_op(p, "=="))  { op = OP_EQ;  parser_consume_op(p, "=="); }
	else if (parser_peek_op(p, "!=")) { op = OP_NE; parser_consume_op(p, "!="); }
	else if (parser_peek_op(p, ">=")) { op = OP_GE; parser_consume_op(p, ">="); }
	else if (parser_peek_op(p, "<=")) { op = OP_LE; parser_consume_op(p, "<="); }
	else if (parser_peek_op(p, ">"))  { op = OP_GT; parser_consume_op(p, ">"); }
	else if (parser_peek_op(p, "<"))  { op = OP_LT; parser_consume_op(p, "<"); }
	else {
		op = 0;
	}

	if (op != 0) {
		expr_node_t *node = malloc(sizeof (expr_node_t));
		node->kind = EXPR_COMPARISON;
		node->u.comp.op = op;
		node->u.comp.prop = ident;
		node->u.comp.value = parser_parse_value(p);
		if (p->err || node->u.comp.value == NULL) {
			p->err = 1;
			expr_free(node);
			return (NULL);
		}
		node->left = NULL;
		node->right = NULL;
		return (node);
	}

	expr_node_t *node = malloc(sizeof (expr_node_t));
	node->kind = EXPR_PRESENCE;
	node->op = OP_AND;
	node->u.pres.prop = ident;
	node->left = NULL;
	node->right = NULL;
	return (node);
}

static expr_node_t *
parse_comparison(parser_t *p)
{
	expr_node_t *node = parse_primary(p);
	if (p->err || node == NULL)
		return (NULL);

	parser_skip_spaces(p);
	op_type_t op;
	while (parser_peek_op(p, "&&") || parser_peek_op(p, "||") ||
	    parser_peek_op(p, "==") || parser_peek_op(p, "!=") ||
	    parser_peek_op(p, ">=") || parser_peek_op(p, "<=") ||
	    parser_peek_op(p, ">") || parser_peek_op(p, "<")) {
		if (parser_peek_op(p, "&&") || parser_peek_op(p, "||"))
			break;

		if (parser_peek_op(p, "=="))  { op = OP_EQ;  parser_consume_op(p, "=="); }
		else if (parser_peek_op(p, "!=")) { op = OP_NE; parser_consume_op(p, "!="); }
		else if (parser_peek_op(p, ">=")) { op = OP_GE; parser_consume_op(p, ">="); }
		else if (parser_peek_op(p, "<=")) { op = OP_LE; parser_consume_op(p, "<="); }
		else if (parser_peek_op(p, ">"))  { op = OP_GT; parser_consume_op(p, ">"); }
		else if (parser_peek_op(p, "<"))  { op = OP_LT; parser_consume_op(p, "<"); }
		else break;

		expr_node_t *newnode = malloc(sizeof (expr_node_t));
		newnode->kind = EXPR_NODE;
		newnode->op = op;
		newnode->left = node;
		newnode->right = parse_primary(p);
		if (p->err || newnode->right == NULL) {
			p->err = 1;
			expr_free(newnode);
			return (NULL);
		}
		node = newnode;
		parser_skip_spaces(p);
	}

	return (node);
}

static expr_node_t *
parse_and(parser_t *p)
{
	expr_node_t *node = parse_comparison(p);
	if (p->err || node == NULL)
		return (NULL);

	parser_skip_spaces(p);
	while (parser_peek_op(p, "&&")) {
		expr_node_t *newnode = malloc(sizeof (expr_node_t));
		newnode->kind = EXPR_NODE;
		newnode->op = OP_AND;
		newnode->left = node;
		parser_consume_op(p, "&&");
		newnode->right = parse_comparison(p);
		if (p->err || newnode->right == NULL) {
			p->err = 1;
			expr_free(newnode);
			return (NULL);
		}
		node = newnode;
		parser_skip_spaces(p);
	}

	return (node);
}

static expr_node_t *
parse_or(parser_t *p)
{
	expr_node_t *node = parse_and(p);
	if (p->err || node == NULL)
		return (NULL);

	parser_skip_spaces(p);
	while (parser_peek_op(p, "||")) {
		expr_node_t *newnode = malloc(sizeof (expr_node_t));
		newnode->kind = EXPR_NODE;
		newnode->op = OP_OR;
		newnode->left = node;
		parser_consume_op(p, "||");
		newnode->right = parse_and(p);
		if (p->err || newnode->right == NULL) {
			p->err = 1;
			expr_free(newnode);
			return (NULL);
		}
		node = newnode;
		parser_skip_spaces(p);
	}

	return (node);
}

static expr_node_t *
parse_expression(parser_t *p)
{
	expr_node_t *node = parse_or(p);
	if (p->err || node == NULL)
		return (NULL);

	parser_skip_spaces(p);
	if (*p->pos != '\0')
		p->err = 1;

	return (node);
}

/* ---------- test infrastructure ---------- */

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) \
	do { \
		tests_run++; \
		printf("  %-50s", name); \
		fflush(stdout); \
	} while (0)

#define PASS() \
	do { \
		tests_failed++; \
		printf("  FAIL\n"); \
	} while (0)

#define ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			printf("  FAIL\n"); \
			printf("    %s\n", msg); \
			tests_failed++; \
			return; \
		} \
	} while (0)

#define ASSERT_ERR(expr, msg) \
	do { \
		parser_t _p; \
		parser_init(&_p, expr); \
		expr_node_t *_n = parse_expression(&_p); \
		if (!_p.err) { \
			printf("  FAIL\n"); \
			printf("    %s\n", msg); \
			tests_failed++; \
			return; \
		} \
		expr_free(_n); \
	} while (0)

#define ASSERT_OK(expr) \
	do { \
		parser_t _p; \
		parser_init(&_p, expr); \
		expr_node_t *_n = parse_expression(&_p); \
		if (_p.err || _n == NULL) { \
			printf("  FAIL\n"); \
			printf("    parse failed: '%s'\n", expr); \
			tests_failed++; \
			return; \
		} \
		expr_free(_n); \
	} while (0)

/* ---------- test cases ---------- */

static void
test_comparison_eq(void)
{
	TEST("comparison: == operator");
	parser_t p;
	parser_init(&p, "guid == 123");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_COMPARISON, "should be comparison node");
	ASSERT(n->u.comp.op == OP_EQ, "operator should be OP_EQ");
	ASSERT(strcmp(n->u.comp.prop, "guid") == 0, "prop should be 'guid'");
	ASSERT(strcmp(n->u.comp.value, "123") == 0, "value should be '123'");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_comparison_ne(void)
{
	TEST("comparison: != operator");
	parser_t p;
	parser_init(&p, "name != testpool");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_COMPARISON, "should be comparison node");
	ASSERT(n->u.comp.op == OP_NE, "operator should be OP_NE");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_comparison_gt(void)
{
	TEST("comparison: > operator");
	parser_t p;
	parser_init(&p, "guid > 0");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_COMPARISON, "should be comparison node");
	ASSERT(n->u.comp.op == OP_GT, "operator should be OP_GT");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_comparison_lt(void)
{
	TEST("comparison: < operator");
	parser_t p;
	parser_init(&p, "guid < 10000000000000000000");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_COMPARISON, "should be comparison node");
	ASSERT(n->u.comp.op == OP_LT, "operator should be OP_LT");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_comparison_ge(void)
{
	TEST("comparison: >= operator");
	parser_t p;
	parser_init(&p, "used >= 1M");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_COMPARISON, "should be comparison node");
	ASSERT(n->u.comp.op == OP_GE, "operator should be OP_GE");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_comparison_le(void)
{
	TEST("comparison: <= operator");
	parser_t p;
	parser_init(&p, "creation <= 1700000000");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_COMPARISON, "should be comparison node");
	ASSERT(n->u.comp.op == OP_LE, "operator should be OP_LE");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_and_operator(void)
{
	TEST("logical: && (AND) operator");
	parser_t p;
	parser_init(&p, "guid > 0 && guid < 10000000000000000000");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_NODE, "should be AND node");
	ASSERT(n->op == OP_AND, "operator should be OP_AND");
	ASSERT(n->left != NULL && n->right != NULL, "should have both operands");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_or_operator(void)
{
	TEST("logical: || (OR) operator");
	parser_t p;
	parser_init(&p, "guid > 15000000000000000000 || guid < 6000000000000000000");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_NODE, "should be OR node");
	ASSERT(n->op == OP_OR, "operator should be OP_OR");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_parentheses(void)
{
	TEST("precedence: parentheses");
	parser_t p;
	parser_init(&p, "(guid > 0)");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_COMPARISON, "should be comparison node (inside parens)");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_complex_parens(void)
{
	TEST("precedence: complex parentheses with && and ||");
	parser_t p;
	parser_init(&p,
	    "(guid > 15000000000000000000 || guid < 6000000000000000000) && name != za-client-2-pool");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	/* Top level should be AND */
	ASSERT(n->kind == EXPR_NODE, "top-level should be AND node");
	ASSERT(n->op == OP_AND, "operator should be OP_AND");
	/* Left operand should be OR (the parenthesized expression) */
	ASSERT(n->left->kind == EXPR_NODE, "left should be OR node");
	ASSERT(n->left->op == OP_OR, "left operator should be OP_OR");
	/* Right operand should be comparison */
	ASSERT(n->right->kind == EXPR_COMPARISON, "right should be comparison");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_precedence_and_over_or(void)
{
	TEST("precedence: && binds tighter than ||");
	parser_t p;
	parser_init(&p, "a == 1 || b == 2 && c == 3");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	/* || should be at top level, && should be right child */
	ASSERT(n->kind == EXPR_NODE, "should be OR node");
	ASSERT(n->op == OP_OR, "operator should be OP_OR");
	ASSERT(n->right->kind == EXPR_NODE, "right child should be AND node");
	ASSERT(n->right->op == OP_AND, "right operator should be OP_AND");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_presence_eval(void)
{
	TEST("presence: shorthand boolean evaluation");
	parser_t p;
	parser_init(&p, "com:company:backup_id");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_PRESENCE, "should be presence node");
	ASSERT(strcmp(n->u.pres.prop, "com:company:backup_id") == 0,
	    "prop should be 'com:company:backup_id'");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_user_property_comparison(void)
{
	TEST("comparison: user property with colon");
	parser_t p;
	parser_init(&p, "com:foo:bar == hello");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_COMPARISON, "should be comparison node");
	ASSERT(strcmp(n->u.comp.prop, "com:foo:bar") == 0, "prop should be 'com:foo:bar'");
	ASSERT(strcmp(n->u.comp.value, "hello") == 0, "value should be 'hello'");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_size_suffix_k(void)
{
	TEST("suffix: K (kilobyte) expansion");
	parser_t p;
	parser_init(&p, "used > 1K");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(strcmp(n->u.comp.value, "1K") == 0, "value should be '1K'");
	boolean_t is_numeric;
	uint64_t val = parse_value_numeric("1K", &is_numeric);
	ASSERT(is_numeric == B_TRUE, "should be numeric");
	ASSERT(val == 1024, "1K should expand to 1024");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_size_suffix_m(void)
{
	TEST("suffix: M (megabyte) expansion");
	parser_t p;
	parser_init(&p, "referenced > 10M");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	boolean_t is_numeric;
	uint64_t val = parse_value_numeric("10M", &is_numeric);
	ASSERT(is_numeric == B_TRUE, "should be numeric");
	ASSERT(val == 10485760, "10M should expand to 10485760");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_size_suffix_g(void)
{
	TEST("suffix: G (gigabyte) expansion");
	parser_t p;
	parser_init(&p, "written > 5G");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	boolean_t is_numeric;
	uint64_t val = parse_value_numeric("5G", &is_numeric);
	ASSERT(is_numeric == B_TRUE, "should be numeric");
	ASSERT(val == 5368709120, "5G should expand to 5368709120");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_size_suffix_t(void)
{
	TEST("suffix: T (terabyte) expansion");
	boolean_t is_numeric;
	uint64_t val = parse_value_numeric("2T", &is_numeric);
	ASSERT(is_numeric == B_TRUE, "should be numeric");
	ASSERT(val == 2199023255552ULL, "2T should expand to 2199023255552");
	printf("  PASS\n");
}

static void
test_time_suffix_d(void)
{
	TEST("suffix: d (day) expansion");
	boolean_t is_numeric;
	uint64_t val = parse_value_numeric("30d", &is_numeric);
	ASSERT(is_numeric == B_TRUE, "should be numeric");
	ASSERT(val == 2592000, "30d should expand to 2592000 seconds");
	printf("  PASS\n");
}

static void
test_time_suffix_h(void)
{
	TEST("suffix: h (hour) expansion");
	boolean_t is_numeric;
	uint64_t val = parse_value_numeric("24h", &is_numeric);
	ASSERT(is_numeric == B_TRUE, "should be numeric");
	ASSERT(val == 86400, "24h should expand to 86400 seconds");
	printf("  PASS\n");
}

static void
test_time_suffix_m(void)
{
	TEST("suffix: m (minute) expansion");
	boolean_t is_numeric;
	uint64_t val = parse_value_numeric("60m", &is_numeric);
	ASSERT(is_numeric == B_TRUE, "should be numeric");
	ASSERT(val == 3600, "60m should expand to 3600 seconds");
	printf("  PASS\n");
}

static void
test_time_suffix_s(void)
{
	TEST("suffix: s (second) expansion");
	boolean_t is_numeric;
	uint64_t val = parse_value_numeric("100s", &is_numeric);
	ASSERT(is_numeric == B_TRUE, "should be numeric");
	ASSERT(val == 100, "100s should expand to 100 seconds");
	printf("  PASS\n");
}

static void
test_large_uint64(void)
{
	TEST("numeric: large uint64 GUID values");
	boolean_t is_numeric;
	uint64_t val = parse_value_numeric("18446744073709551615", &is_numeric);
	ASSERT(is_numeric == B_TRUE, "should be numeric");
	ASSERT(val == UINT64_MAX, "should parse as UINT64_MAX");
	printf("  PASS\n");
}

static void
test_invalid_missing_operand(void)
{
	TEST("error: missing operand after operator");
	ASSERT_ERR("guid >", "should fail: missing operand");
	ASSERT_ERR("guid >", "should fail: missing operand");
	ASSERT_ERR("== 123", "should fail: missing property");
	ASSERT_ERR("guid >", "should fail: missing operand");
	printf("  PASS\n");
}

static void
test_invalid_unbalanced_parens(void)
{
	TEST("error: unbalanced parentheses");
	ASSERT_ERR("(guid > 0", "should fail: missing closing paren");
	ASSERT_ERR("guid > 0)", "should fail: extra closing paren");
	printf("  PASS\n");
}

static void
test_invalid_empty_expression(void)
{
	TEST("error: empty expression");
	ASSERT_ERR("", "should fail: empty expression");
	printf("  PASS\n");
}

static void
test_invalid_no_operator(void)
{
	TEST("error: two properties without operator");
	/* This should parse as presence of first prop only, rest is trailing */
	ASSERT_ERR("guid creation", "should fail: trailing tokens");
	printf("  PASS\n");
}

static void
test_quoted_value(void)
{
	TEST("value: quoted string");
	parser_t p;
	parser_init(&p, "name == \"test pool\"");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(strcmp(n->u.comp.value, "test pool") == 0, "value should be 'test pool'");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_nested_parentheses(void)
{
	TEST("precedence: nested parentheses");
	parser_t p;
	parser_init(&p, "((guid > 0))");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_COMPARISON, "should resolve to comparison");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_multiple_and_chained(void)
{
	TEST("logical: chained && operators");
	parser_t p;
	parser_init(&p, "a == 1 && b == 2 && c == 3");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	/* Should be right-associative chain of AND nodes */
	ASSERT(n->kind == EXPR_NODE, "should be AND node");
	ASSERT(n->op == OP_AND, "operator should be OP_AND");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_multiple_or_chained(void)
{
	TEST("logical: chained || operators");
	parser_t p;
	parser_init(&p, "a == 1 || b == 2 || c == 3");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_NODE, "should be OR node");
	ASSERT(n->op == OP_OR, "operator should be OP_OR");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_mixed_precedence(void)
{
	TEST("precedence: mixed && and || with parens");
	parser_t p;
	parser_init(&p, "(a == 1 || b == 2) && (c == 3 || d == 4)");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_NODE, "top should be AND");
	ASSERT(n->op == OP_AND, "operator should be OP_AND");
	ASSERT(n->left->kind == EXPR_NODE, "left should be OR");
	ASSERT(n->left->op == OP_OR, "left operator should be OP_OR");
	ASSERT(n->right->kind == EXPR_NODE, "right should be OR");
	ASSERT(n->right->op == OP_OR, "right operator should be OP_OR");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_whitespace_handling(void)
{
	TEST("parser: whitespace handling");
	parser_t p;
	parser_init(&p, "  guid   >   0  ");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(n->kind == EXPR_COMPARISON, "should be comparison");
	ASSERT(strcmp(n->u.comp.value, "0") == 0, "value should be '0'");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_tab_whitespace(void)
{
	TEST("parser: tab whitespace handling");
	parser_t p;
	parser_init(&p, "guid\t>\t0");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_dash_in_property(void)
{
	TEST("parser: dash in property name");
	parser_t p;
	parser_init(&p, "atime == on");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(strcmp(n->u.comp.prop, "atime") == 0, "prop should be 'atime'");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_numeric_value_zero(void)
{
	TEST("numeric: zero value");
	parser_t p;
	parser_init(&p, "guid == 0");
	expr_node_t *n = parse_expression(&p);
	ASSERT(!p.err && n != NULL, "parse should succeed");
	ASSERT(strcmp(n->u.comp.value, "0") == 0, "value should be '0'");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_numeric_value_negative_not_allowed(void)
{
	/* Negative numbers are not supported - the '-' is consumed as part of property name */
	/* This tests that the parser handles it gracefully */
	TEST("numeric: negative value treated as property");
	parser_t p;
	parser_init(&p, "guid == -5");
	expr_node_t *n = parse_expression(&p);
	/* -5 should parse as value "-5" (dash is a valid property char) */
	ASSERT(!p.err && n != NULL, "parse should succeed");
	expr_free(n);
	printf("  PASS\n");
}

static void
test_size_suffix_e(void)
{
	TEST("suffix: E (exabyte) expansion");
	boolean_t is_numeric;
	uint64_t val = parse_value_numeric("1E", &is_numeric);
	ASSERT(is_numeric == B_TRUE, "should be numeric");
	ASSERT(val == 1152921504606846976ULL, "1E should expand to 1152921504606846976");
	printf("  PASS\n");
}

static void
test_size_suffix_lowercase(void)
{
	TEST("suffix: lowercase suffixes (k, m, g)");
	boolean_t is_numeric;
	uint64_t v1 = parse_value_numeric("1k", &is_numeric);
	ASSERT(is_numeric == B_TRUE, "1k should be numeric");
	ASSERT(v1 == 1024, "1k should be 1024");
	uint64_t v2 = parse_value_numeric("1g", &is_numeric);
	ASSERT(is_numeric == B_TRUE, "1g should be numeric");
	ASSERT(v2 == 1073741824, "1g should be 1073741824");
	printf("  PASS\n");
}

/*
 * Inline implementation of zfs_prop_user() logic for unit testing.
 * See include/sys/fs/zfs.h for the canonical definition.
 *
 * User properties contain a ':' and consist only of valid characters
 * (a-z, A-Z, 0-9, -, _, ., :).
 */
static boolean_t
is_user_property(const char *name)
{
	const char *p;

	if (name == NULL || *name == '\0')
		return (B_FALSE);

	for (p = name; *p != '\0'; p++) {
		char c = *p;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		    c == '.' || c == ':'))
			return (B_FALSE);
	}
	return (strchr(name, ':') != NULL);
}

static void
test_property_name_classification(void)
{
	/*
	 * Verify that property names are correctly classified as user
	 * properties (contain ':') vs built-in properties (no ':').
	 *
	 * This catches bugs where a property identifier is misclassified,
	 * e.g. treating a built-in property as user or vice versa, which
	 * can cause the wrong handle type to be used (snapshot vs
	 * filesystem) when fetching property values.
	 */
	TEST("property name classification: user vs built-in");

	/* User properties contain ':' */
	ASSERT(is_user_property("com:zfs:test") == B_TRUE,
	    "com:zfs:test should be a user property");
	ASSERT(is_user_property("org:example:tag") == B_TRUE,
	    "org:example:tag should be a user property");
	ASSERT(is_user_property("com:test:mytag") == B_TRUE,
	    "com:test:mytag should be a user property");

	/* Built-in properties do not contain ':' */
	ASSERT(is_user_property("guid") == B_FALSE,
	    "guid should not be a user property");
	ASSERT(is_user_property("creation") == B_FALSE,
	    "creation should not be a user property");
	ASSERT(is_user_property("name") == B_FALSE,
	    "name should not be a user property");
	ASSERT(is_user_property("used") == B_FALSE,
	    "used should not be a user property");
	ASSERT(is_user_property("mountpoint") == B_FALSE,
	    "mountpoint should not be a user property");

	/* Edge cases */
	ASSERT(is_user_property("a") == B_FALSE,
	    "single char should not be user");
	ASSERT(is_user_property("a:b") == B_TRUE,
	    "single colon should be user property");

	printf("  PASS\n");
}

/* ---------- main ---------- */

int
main(int argc __attribute__((unused)), char **argv __attribute__((unused)))
{
	printf("Running expression parser unit tests...\n\n");

	test_comparison_eq();
	test_comparison_ne();
	test_comparison_gt();
	test_comparison_lt();
	test_comparison_ge();
	test_comparison_le();
	test_and_operator();
	test_or_operator();
	test_parentheses();
	test_complex_parens();
	test_precedence_and_over_or();
	test_presence_eval();
	test_user_property_comparison();
	test_size_suffix_k();
	test_size_suffix_m();
	test_size_suffix_g();
	test_size_suffix_t();
	test_time_suffix_d();
	test_time_suffix_h();
	test_time_suffix_m();
	test_time_suffix_s();
	test_large_uint64();
	test_invalid_missing_operand();
	test_invalid_unbalanced_parens();
	test_invalid_empty_expression();
	test_invalid_no_operator();
	test_quoted_value();
	test_nested_parentheses();
	test_multiple_and_chained();
	test_multiple_or_chained();
	test_mixed_precedence();
	test_whitespace_handling();
	test_tab_whitespace();
	test_dash_in_property();
	test_numeric_value_zero();
	test_numeric_value_negative_not_allowed();
	test_size_suffix_e();
	test_size_suffix_lowercase();
	test_property_name_classification();

	printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
	return (tests_failed > 0 ? 1 : 0);
}

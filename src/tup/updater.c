#include "updater.h"
#include "graph.h"
#include "fileio.h"
#include "debug.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/file.h>

static int create_flag_cb(void *arg, struct db_node *dbn);
static int process_create_nodes(void);
static int md_flag_cb(void *arg, struct db_node *dbn);
static int build_graph(struct graph *g);
static int add_file(struct graph *g, struct node *src, struct db_node *dbn);
static int find_deps(struct graph *g, struct node *n);
static int execute_graph(struct graph *g);
static void show_progress(int n, int tot);

static int (*create)(const char *dir);
static int update(struct node *n);
static int delete_file(struct node *n);

static int do_show_progress = 1;

struct name_list {
	struct list_head list;
	char *name;
	tupid_t tupid;
};

int updater(int argc, char **argv)
{
	char *create_so;
	struct graph g;
	int upd_lock;
	void *handle;
	int x;

	for(x=1; x<argc; x++) {
		if(strcmp(argv[x], "-d") == 0) {
			debug_enable("tup.updater");
		}
	}

	upd_lock = open(TUP_UPDATE_LOCK, O_RDONLY);
	if(upd_lock < 0) {
		perror(TUP_UPDATE_LOCK);
		return 1;
	}
	if(flock(upd_lock, LOCK_EX|LOCK_NB) < 0) {
		if(errno == EWOULDBLOCK) {
			printf("Waiting for lock...\n");
			if(flock(upd_lock, LOCK_EX) == 0)
				goto lock_success;
		}
		perror("flock");
		return 1;
	}
lock_success:

	if(tup_db_config_get_string(&create_so, "create_so", "make.so") < 0)
		return -1;
	do_show_progress = tup_db_config_get_int("show_progress");

	handle = dlopen(create_so, RTLD_LAZY);
	if(!handle) {
		fprintf(stderr, "Error: Unable to load %s\n", create_so);
		return 1;
	}
	create = dlsym(handle, "create");
	if(!create) {
		fprintf(stderr, "Error: Couldn't find 'create' symbol in "
			"builder.\n");
		return 1;
	}

	if(process_create_nodes() < 0)
		return 1;
	if(build_graph(&g) < 0)
		return 1;
	if(execute_graph(&g) < 0)
		return 1;

	flock(upd_lock, LOCK_UN);
	close(upd_lock);
	return 0;
}

static int create_flag_cb(void *arg, struct db_node *dbn)
{
	struct list_head *list = arg;
	struct name_list *nl;

	nl = malloc(sizeof *nl);
	if(!nl) {
		perror("malloc");
		return -1;
	}

	nl->name = strdup(dbn->name);
	if(!nl->name) {
		perror("strdup");
		return -1;
	}
	nl->tupid = dbn->tupid;

	list_add(&nl->list, list);

	/* Move all existing commands over to delete - then the ones that are
	 * re-created will be moved back out in create(). All those that are
	 * no longer generated remain in delete for cleanup.
	 */
	if(tup_db_set_cmdchild_flags(dbn->tupid, TUP_FLAGS_DELETE) < 0)
		return -1;

	return 0;
}

static int process_create_nodes(void)
{
	struct name_list *nl;
	LIST_HEAD(namelist);

	/* TODO: Do in while loop in case it creates more create nodes? */
	if(tup_db_select_node_by_flags(create_flag_cb, &namelist,
				       TUP_FLAGS_CREATE) != 0)
		return -1;

	while(!list_empty(&namelist)) {
		nl = list_entry(namelist.next, struct name_list, list);
		if(create(nl->name) < 0)
			return -1;
		if(tup_db_set_flags_by_id(nl->tupid, TUP_FLAGS_NONE) < 0)
			return -1;
		list_del(&nl->list);
		free(nl->name);
		free(nl);
	}

	return 0;
}

static int md_flag_cb(void *arg, struct db_node *dbn)
{
	struct graph *g = arg;
	if(add_file(g, g->cur, dbn) < 0)
		return -1;
	return 0;
}

static int build_graph(struct graph *g)
{
	struct node *cur;

	if(create_graph(g) < 0)
		return -1;

	g->cur = g->root;
	g->root->flags = TUP_FLAGS_MODIFY;
	if(tup_db_select_node_by_flags(md_flag_cb, g, TUP_FLAGS_MODIFY) < 0)
		return -1;

	g->root->flags = TUP_FLAGS_DELETE;
	if(tup_db_select_node_by_flags(md_flag_cb, g, TUP_FLAGS_DELETE) < 0)
		return -1;

	g->root->flags = TUP_FLAGS_NONE;

	while(!list_empty(&g->plist)) {
		cur = list_entry(g->plist.next, struct node, list);
		if(cur->state == STATE_INITIALIZED) {
			DEBUGP("find deps for node: %lli\n", cur->tupid);
			if(find_deps(g, cur) < 0)
				return -1;
			cur->state = STATE_PROCESSING;
		} else if(cur->state == STATE_PROCESSING) {
			DEBUGP("remove node from stack: %lli\n", cur->tupid);
			list_del(&cur->list);
			list_add_tail(&cur->list, &g->node_list);
			cur->state = STATE_FINISHED;
		}
	}

	return 0;
}

static int add_file(struct graph *g, struct node *src, struct db_node *dbn)
{
	struct node *n;

	/* Inherit flags of the parent, unless the parent is a file, in which
	 * case we default to just modify (since a command is only deleted
	 * if the directory is modified and isn't re-created in the create
	 * phase. Yeah that totally makes sense.)
	 */
	dbn->flags = src->flags;
	if(src->type == TUP_NODE_FILE)
		dbn->flags = TUP_FLAGS_MODIFY;

	if((n = find_node(g, dbn->tupid)) != NULL) {
		if(!(n->flags & dbn->flags)) {
			/* Check to see if we counted this as a node we'll
			 * print something about later that we'll actually
			 * skip (a file with modify flags just gets skipped)
			 */
			if(n->type == TUP_NODE_FILE &&
			   n->flags == TUP_FLAGS_DELETE &&
			   (dbn->flags&TUP_FLAGS_MODIFY)) {
				g->num_nodes--;
			}
			DEBUGP("adding flag (0x%x) to %lli\n", dbn->flags, dbn->tupid);
			n->flags |= dbn->flags;
		}
		goto edge_create;
	}
	n = create_node(g, dbn);
	if(!n)
		return -1;
	DEBUGP("create node: %lli (0x%x)\n", dbn->tupid, dbn->type);

edge_create:
	if(n->state == STATE_PROCESSING) {
		fprintf(stderr, "Error: Circular dependency detected! "
			"Last edge was: %lli -> %lli\n",
			src->tupid, dbn->tupid);
		return -1;
	}
	if(create_edge(src, n) < 0)
		return -1;
	return 0;
}

static int find_deps(struct graph *g, struct node *n)
{

	g->cur = n;
	if(tup_db_select_node_by_link(md_flag_cb, g, n->tupid) < 0)
		return -1;
	if(tup_db_select_node_by_cmdlink(md_flag_cb, g, n->tupid) < 0)
		return -1;
	return 0;
}

static int execute_graph(struct graph *g)
{
	struct node *root;
	int num_processed = 0;

	root = list_entry(g->node_list.next, struct node, list);
	DEBUGP("root node: %lli\n", root->tupid);
	list_move(&root->list, &g->plist);

	show_progress(num_processed, g->num_nodes);
	while(!list_empty(&g->plist)) {
		struct node *n;
		n = list_entry(g->plist.next, struct node, list);
		DEBUGP("cur node: %lli [%i]\n", n->tupid, n->incoming_count);
		if(n->incoming_count) {
			list_move(&n->list, &g->node_list);
			n->state = STATE_FINISHED;
			continue;
		}
		if(n != root) {
			if(n->type == TUP_NODE_FILE &&
			   (n->flags == TUP_FLAGS_DELETE)) {
				delete_file(n);
				num_processed++;
				show_progress(num_processed, g->num_nodes);
			}
			if(n->type == TUP_NODE_CMD) {
				if(n->flags & TUP_FLAGS_DELETE) {
					printf("[35mDelete[%lli]: %s[0m\n", n->tupid, n->name);
					if(delete_name_file(n->tupid) < 0)
						return -1;
				} else {
					if(update(n) < 0)
						return -1;
				}
				num_processed++;
				show_progress(num_processed, g->num_nodes);
			}
		}
		while(n->edges) {
			struct edge *e;
			e = n->edges;
			if(e->dest->state != STATE_PROCESSING) {
				list_del(&e->dest->list);
				list_add(&e->dest->list, &g->plist);
				e->dest->state = STATE_PROCESSING;
			}
			/* TODO: slist_del? */
			n->edges = remove_edge(e);
		}
		if(tup_db_set_flags_by_id(n->tupid, TUP_FLAGS_NONE) < 0)
			return -1;
		remove_node(g, n);
	}
	if(!list_empty(&g->node_list) || !list_empty(&g->plist)) {
		fprintf(stderr, "Error: Graph is not empty after execution.\n");
		return -1;
	}
	return 0;
}

static int update(struct node *n)
{
	int rc;
	char s[32];
	tupid_t tupid;

	tupid = tup_db_create_dup_node(n->name, n->type, TUP_FLAGS_NONE);
	if(tupid < 0)
		return -1;

	if(snprintf(s, sizeof(s), "%lli", tupid) >= (signed)sizeof(s)) {
		fprintf(stderr, "Buffer size error in update()\n");
		goto err_delete_node;
	}

	if(setenv(TUP_CMD_ID, s, 1) < 0) {
		perror("setenv");
		goto err_delete_node;
	}

	printf("%s\n", n->name);
	rc = system(n->name);
	unsetenv(TUP_CMD_ID);
	if(rc != 0)
		goto err_delete_node;
	if(tup_db_move_cmdlink(n->tupid, tupid) < 0)
		return -1;
	delete_name_file(n->tupid);
	return 0;

err_delete_node:
	delete_name_file(tupid);
	return -1;
}

static int delete_file(struct node *n)
{
	printf("[35mDelete[%lli]: %s[0m\n", n->tupid, n->name);
	if(delete_name_file(n->tupid) < 0)
		return -1;
	if(unlink(n->name) < 0) {
		/* Don't care if the file is already gone. */
		if(errno != ENOENT) {
			perror(n->name);
			return -1;
		}
	}

	return 0;
}

static void show_progress(int n, int tot)
{
	if(do_show_progress && tot) {
		int x, a, b;
		const int max = 40;
		char c = '=';
		if(tot > max) {
			a = n * max / tot;
			b = max;
			c = '#';
		} else {
			a = n;
			b = tot;
		}
		printf("[");
		for(x=0; x<a; x++) {
			printf("%c", c);
		}
		for(x=a; x<b; x++) {
			printf(" ");
		}
		printf("] %i/%i (%3i%%) ", n, tot, n*100/tot);
		if(n == tot)
			printf("\n");
	}
}

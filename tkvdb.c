/*
 * tkvdb
 *
 * Copyright (c) 2016-2018, Vladimir Misyurov
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "tkvdb.h"

#define TKVDB_SIGNATURE    "tkvdb003"

/* at the begin of each on-disk block there is a byte with type.
 * footer marked as removed is used in vacuum procedure */
#define TKVDB_BLOCKTYPE_TRANSACTION  0
#define TKVDB_BLOCKTYPE_FOOTER       1
#define TKVDB_BLOCKTYPE_RM_FOOTER    2

/* node properties */
#define TKVDB_NODE_VAL  (1 << 0)
#define TKVDB_NODE_META (1 << 1)

/* max number of subnodes we store as [symbols array] => [offsets array]
 * if number of subnodes is more than TKVDB_SUBNODES_THR, they stored on disk
 * as array of 256 offsets */
#define TKVDB_SUBNODES_THR (256 - 256 / sizeof(uint64_t))

/* read block size */
#define TKVDB_READ_SIZE 4096

/* FIXME: allocate stack dynamically */
#define TKVDB_STACK_MAX_DEPTH 128

/* helper macro for executing functions which returns TKVDB_RES */
#define TKVDB_EXEC(FUNC)                   \
do {                                       \
	TKVDB_RES r = FUNC;                \
	if (r != TKVDB_OK) {               \
		return r;                  \
	}                                  \
} while (0)

/* skip replaced nodes */
#define TKVDB_SKIP_RNODES(NODE)            \
while (NODE->replaced_by) {                \
	NODE = NODE->replaced_by;          \
} while (0)

/* replace node with updated one */
/* FIXME: (optional) memory barrier? */
#define TKVDB_REPLACE_NODE(NODE, NEWNODE)  \
do {                                       \
	NODE->replaced_by = NEWNODE;       \
} while (0)

struct tkvdb_params
{
	int flags;              /* db file flags (as passed to open()) */
	mode_t mode;            /* db file mode */

	size_t write_buf_limit; /* size of database write buffer */
	int write_buf_dynalloc; /* realloc buffer when needed */

	size_t tr_buf_limit;    /* size of transaction buffer */
	int tr_buf_dynalloc;    /* realloc transaction buffer when needed */
};

/* on-disk transaction header */
struct tkvdb_tr_header
{
	uint8_t type;
	uint64_t footer_off;       /* pointer to footer */
} __attribute__((packed));

/* on-disk transaction footer */
struct tkvdb_tr_footer
{
	uint8_t type;
	uint8_t signature[8];
	uint64_t root_off;         /* offset of root node */
	uint64_t transaction_size; /* transaction size */
	uint64_t transaction_id;   /* transaction number */

	uint64_t gap_begin;
	uint64_t gap_end;
} __attribute__((packed));

#define TKVDB_TR_FTRSIZE (sizeof(struct tkvdb_tr_footer))

/* database file information */
struct tkvdb_db_info
{
	struct tkvdb_tr_footer footer;

	uint64_t filesize;
};

/* database */
struct tkvdb
{
	int fd;                     /* database file handle */
	struct tkvdb_db_info info;

	tkvdb_params params;        /* database params */

	uint8_t *write_buf;
	size_t write_buf_allocated;
};

/* on-disk node */
struct tkvdb_disknode
{
	uint32_t size;        /* node size */
	uint8_t type;         /* type (has value or metadata) */
	uint16_t nsubnodes;   /* number of subnodes */
	uint32_t prefix_size; /* prefix size */

	uint8_t data[1];      /* variable size data */
} __attribute__((packed));


/* node in memory */
typedef struct tkvdb_memnode
{
	int type;
	size_t prefix_size;
	size_t val_size;
	size_t meta_size;

	uint64_t disk_size;               /* size of node on disk */
	uint64_t disk_off;                /* offset of node on disk */
	unsigned int nsubnodes;           /* number of subnodes */

	struct tkvdb_memnode *replaced_by;

	struct tkvdb_memnode *next[256];  /* subnodes in memory */
	uint64_t fnext[256];              /* positions of subnodes in file */

	unsigned char prefix_val_meta[1]; /* prefix, value and metadata */
} tkvdb_memnode;

/* transaction in memory */
struct tkvdb_tr
{
	tkvdb *db;
	tkvdb_memnode *root;

	int started;

	uint8_t *tr_buf;                /* transaction buffer */
	size_t tr_buf_allocated;
	uint8_t *tr_buf_ptr;

	/* size of transaction buffer */
	size_t tr_buf_limit;
	/* allow reallocation of transaction buffer when needed */
	int tr_buf_dynalloc;
};

struct tkvdb_visit_helper
{
	tkvdb_memnode *node;
	int off;                /* index of subnode in node */

};

/* database cursor */
struct tkvdb_cursor
{
	size_t stack_size;
	struct tkvdb_visit_helper stack[TKVDB_STACK_MAX_DEPTH];

	size_t prefix_size;
	unsigned char *prefix;

	size_t val_size;
	uint8_t *val;

	tkvdb_tr *tr;
};

/* get next subnode (or load from disk) */
#define TKVDB_SUBNODE_NEXT(TR, NODE, NEXT, OFF)                           \
do {                                                                      \
	if (NODE->next[OFF]) {                                            \
		NEXT = node->next[OFF];                                   \
	} else if (TR->db && NODE->fnext[OFF]) {                          \
		tkvdb_memnode *tmp;                                       \
		TKVDB_EXEC( tkvdb_node_read(TR, NODE->fnext[OFF], &tmp) );\
		NODE->next[OFF] = tmp;                                    \
		NEXT = tmp;                                               \
	}                                                                 \
} while (0)

#define TKVDB_SUBNODE_SEARCH(TR, NODE, NEXT, OFF, INCR)   \
do {                                                      \
	int lim, step;                                    \
	NEXT = NULL;                                      \
	if (INCR) {                                       \
		lim = 256;                                \
		step = 1;                                 \
	} else {                                          \
		lim = -1;                                 \
		step = -1;                                \
	}                                                 \
	for (; OFF!=lim; OFF+=step) {                     \
		TKVDB_SUBNODE_NEXT(TR, NODE, NEXT, OFF);  \
		if (next) {                               \
			break;                            \
		}                                         \
	}                                                 \
} while (0)


static TKVDB_RES
tkvdb_info_read(const int fd, struct tkvdb_db_info *info)
{
	struct stat st;
	off_t footer_pos; /* position of footer in database file */
	ssize_t io_res;

	/* get file size */
	if (fstat(fd, &st) != 0) {
		return TKVDB_IO_ERROR;
	}

	info->filesize = st.st_size;

	if (info->filesize == 0) {
		/* empty file */
		return TKVDB_OK;
	}

	if (info->filesize <= (off_t)TKVDB_TR_FTRSIZE) {
		/* file is too small */
		return TKVDB_CORRUPTED;
	}

	/* seek to the end of file (e.g. footer of last transaction) */
	footer_pos = info->filesize - TKVDB_TR_FTRSIZE;
	if (lseek(fd, footer_pos, SEEK_SET) != footer_pos) {
		return TKVDB_IO_ERROR;
	}

	io_res = read(fd, &info->footer, TKVDB_TR_FTRSIZE);
	if (io_res < (ssize_t)TKVDB_TR_FTRSIZE) {
		/* read less than footer, assuming it's error */
		return TKVDB_IO_ERROR;
	}

	/* check signature */
	if ((memcmp(info->footer.signature, TKVDB_SIGNATURE,
		sizeof(TKVDB_SIGNATURE) - 1)) != 0) {

		return TKVDB_CORRUPTED;
	}

	if (info->footer.transaction_size > (uint64_t)footer_pos) {
		return TKVDB_CORRUPTED;
	}

	return TKVDB_OK;
}

/* fill tkvdb_params with default values */
void
tkvdb_params_init(tkvdb_params *params)
{
	params->write_buf_dynalloc = 1;
	params->write_buf_limit = SIZE_MAX;

	params->tr_buf_dynalloc = 1;
	params->tr_buf_limit = SIZE_MAX;

	params->flags = O_RDWR | O_CREAT;
	params->mode = S_IRUSR | S_IWUSR;
}

/* open database file */
tkvdb *
tkvdb_open(const char *path, tkvdb_params *user_params)
{
	tkvdb *db;
	TKVDB_RES r;

	db = malloc(sizeof(tkvdb));
	if (!db) {
		goto fail;
	}

	if (user_params) {
		db->params = *user_params;
	} else {
		tkvdb_params_init(&db->params);
	}

	db->fd = open(path, db->params.flags, db->params.mode);
	if (db->fd < 0) {
		goto fail_free;
	}

	r = tkvdb_info_read(db->fd, &(db->info));
	if (r != TKVDB_OK) {
		/* error */
		goto fail_close;
	}

	/* init params */
	if (db->params.write_buf_dynalloc) {
		db->write_buf = NULL;
		db->write_buf_allocated = 0;
	} else {
		db->write_buf = malloc(db->params.write_buf_limit);
		if (!db->write_buf) {
			goto fail_close;
		}
		db->write_buf_allocated = db->params.write_buf_limit;
	}

	return db;

fail_close:
	close(db->fd);
fail_free:
	free(db);
fail:
	return NULL;
}

/* close database and free data */
TKVDB_RES
tkvdb_close(tkvdb *db)
{
	TKVDB_RES r = TKVDB_OK;

	if (!db) {
		return TKVDB_OK;
	}

	if (close(db->fd) < 0) {
		r = TKVDB_IO_ERROR;
	}
	if (db->write_buf) {
		free(db->write_buf);
	}

	free(db);
	return r;
}

/* get memory for node
 * memory block is taken from system using malloc()
 * when 'tr->tr_buf_dynalloc' is true
 * or from preallocated buffer
 * preallocation occurs in tkvdb_tr_create_m() */
static tkvdb_memnode *
tkvdb_node_alloc(tkvdb_tr *tr, size_t node_size)
{
	tkvdb_memnode *node;

	if ((tr->tr_buf_allocated + node_size) > tr->tr_buf_limit) {
		/* memory limit exceeded */
		return NULL;
	}

	if (tr->tr_buf_dynalloc) {
		node = malloc(node_size);
		if (!node) {
			return NULL;
		}
	} else {
		/* FIXME: don't hardcode! check space! */
		/* align to 16-byte boundary */
		tr->tr_buf_ptr = (uint8_t *)
			((uintptr_t)(tr->tr_buf_ptr + 16 - 1) & (-16));
		node = (tkvdb_memnode *)tr->tr_buf_ptr;
		tr->tr_buf_ptr += node_size;
	}

	tr->tr_buf_allocated += node_size;
	return node;
}

/* create new node and append prefix and value */
static tkvdb_memnode *
tkvdb_node_new(tkvdb_tr *tr, int type, size_t prefix_size,
	const void *prefix, size_t vlen, const void *val)
{
	tkvdb_memnode *node;
	size_t node_size;

	node_size = sizeof(tkvdb_memnode) + prefix_size + vlen;
	node = tkvdb_node_alloc(tr, node_size);
	if (!node) {
		return NULL;
	}

	node->type = type;
	node->prefix_size = prefix_size;
	node->val_size = vlen;
	node->meta_size = 0;
	node->replaced_by = NULL;
	if (node->prefix_size > 0) {
		memcpy(node->prefix_val_meta, prefix, node->prefix_size);
	}
	if (node->val_size > 0) {
		memcpy(node->prefix_val_meta + node->prefix_size,
			val, node->val_size);
	}

	memset(node->next, 0, sizeof(tkvdb_memnode *) * 256);
	memset(node->fnext, 0, sizeof(uint64_t) * 256);

	node->disk_size = 0;
	node->disk_off = 0;

	return node;
}

static void
tkvdb_clone_subnodes(tkvdb_memnode *dst, tkvdb_memnode *src)
{
	memcpy(dst->next,  src->next, sizeof(tkvdb_memnode *) * 256);
	memcpy(dst->fnext, src->fnext, sizeof(uint64_t) * 256);
}

/* read node from disk */
static TKVDB_RES
tkvdb_node_read(tkvdb_tr *tr, uint64_t off, tkvdb_memnode **node_ptr)
{
	uint8_t buf[TKVDB_READ_SIZE];
	ssize_t read_res;
	struct tkvdb_disknode *disknode;
	size_t prefix_val_meta_size;
	uint8_t *ptr;
	int fd;

	fd = tr->db->fd;

	if (lseek(fd, off, SEEK_SET) != (off_t)off) {
		return TKVDB_IO_ERROR;
	}

	read_res = read(fd, buf, TKVDB_READ_SIZE);
	if (read_res < 0) {
		return TKVDB_IO_ERROR;
	}

	disknode = (struct tkvdb_disknode *)buf;

	if (((uint32_t)read_res < disknode->size) && (disknode->size < TKVDB_READ_SIZE)) {
		return TKVDB_IO_ERROR;
	}

	/* calculate size of prefix + value + metadata */
	prefix_val_meta_size = disknode->size - sizeof(struct tkvdb_disknode)
		+ sizeof(uint8_t) * 1;

	if (disknode->type & TKVDB_NODE_VAL) {
		prefix_val_meta_size -= sizeof(uint32_t);
	}
	if (disknode->type & TKVDB_NODE_META) {
		prefix_val_meta_size -= sizeof(uint32_t);
	}

	if (disknode->nsubnodes > TKVDB_SUBNODES_THR) {
		prefix_val_meta_size -= 256 * sizeof(uint64_t);
	} else {
		prefix_val_meta_size -= disknode->nsubnodes * sizeof(uint8_t);
		prefix_val_meta_size -= disknode->nsubnodes * sizeof(uint64_t);
	}

	/* allocate memnode */
	*node_ptr = tkvdb_node_alloc(tr, sizeof(tkvdb_memnode)
		+ prefix_val_meta_size);

	if (!(*node_ptr)) {
		return TKVDB_ENOMEM;
	}

	(*node_ptr)->replaced_by = NULL;
	/* now fill memnode with values from disk node */
	(*node_ptr)->type = disknode->type;
	(*node_ptr)->prefix_size = disknode->prefix_size;

	(*node_ptr)->disk_size = 0;
	(*node_ptr)->disk_off = 0;

	ptr = disknode->data;

	(*node_ptr)->val_size = (*node_ptr)->meta_size = 0;
	if (disknode->type & TKVDB_NODE_VAL) {
		(*node_ptr)->val_size = *((uint32_t *)ptr);
		ptr += sizeof(uint32_t);
	}
	if (disknode->type & TKVDB_NODE_META) {
		(*node_ptr)->meta_size = *((uint32_t *)ptr);
		ptr += sizeof(uint32_t);
	}

	memset((*node_ptr)->next, 0, sizeof(tkvdb_memnode *) * 256);

	if (disknode->nsubnodes > TKVDB_SUBNODES_THR) {
		memcpy((*node_ptr)->fnext, ptr, 256 * sizeof(uint64_t));
		ptr += 256 * sizeof(uint64_t);
	} else {
		int i;
		uint64_t *offptr;

		offptr = (uint64_t *)(ptr
			+ disknode->nsubnodes * sizeof(uint8_t));

		memset((*node_ptr)->fnext, 0, sizeof(uint64_t) * 256);

		for (i=0; i<disknode->nsubnodes; i++) {
			(*node_ptr)->fnext[*ptr] = *offptr;
			ptr++;
			offptr++;
		}
		ptr += disknode->nsubnodes * sizeof(uint64_t);
	}

	if (disknode->size > TKVDB_READ_SIZE) {
		/* prefix + value + metadata bigger than read block */
		size_t blk_tail = disknode->size - prefix_val_meta_size;

		memcpy((*node_ptr)->prefix_val_meta, ptr,
			TKVDB_READ_SIZE - blk_tail);
		ptr += TKVDB_READ_SIZE - blk_tail;
		read(fd, ptr, disknode->size - (TKVDB_READ_SIZE - blk_tail));
	} else {
		memcpy((*node_ptr)->prefix_val_meta, ptr,
			prefix_val_meta_size);
	}

	return TKVDB_OK;
}

/* free node and subnodes */
static void
tkvdb_node_free(tkvdb_memnode *node)
{
	size_t stack_size = 0;
	struct tkvdb_visit_helper stack[TKVDB_STACK_MAX_DEPTH];

	tkvdb_memnode *next;
	int off = 0;

	for (;;) {
		if (node->replaced_by) {
			next = node->replaced_by;
			free(node);
			node = next;
			continue;
		}

		/* search in subnodes */
		next = NULL;
		for (; off<256; off++) {
			if (node->next[off]) {
				next = node->next[off];
				break;
			}
		}

		if (next) {
			/* push */
			stack[stack_size].node = node;
			stack[stack_size].off = off;
			stack_size++;

			node = next;
			off = 0;
		} else {
			/* no more subnodes */
			if (stack_size < 1) {
				break;
			}

			free(node);
			/* get node from stack's top */
			stack_size--;
			node = stack[stack_size].node;
			off = stack[stack_size].off;
			off++;
		}
	}
	free(node);
}

/* add key-value pair to memory transaction */
TKVDB_RES
tkvdb_put(tkvdb_tr *tr, const tkvdb_datum *key, const tkvdb_datum *val)
{
	const unsigned char *sym;  /* pointer to current symbol in key */
	tkvdb_memnode *node;       /* current node */
	size_t pi;                 /* prefix index */

	if (!tr->started) {
		return TKVDB_NOT_STARTED;
	}

	/* new root */
	if (tr->root == NULL) {
		if (tr->db && (tr->db->info.filesize > 0)) {
			/* we have underlying non-empty db file */
			TKVDB_EXEC( tkvdb_node_read(tr,
				tr->db->info.footer.root_off,
				&(tr->root)) );
		} else {
			tr->root = tkvdb_node_new(tr, TKVDB_NODE_VAL,
				key->len, key->data, val->len, val->data);
			if (!tr->root) {
				return TKVDB_ENOMEM;
			}
			return TKVDB_OK;
		}
	}

	sym = key->data;
	node = tr->root;

next_node:
	TKVDB_SKIP_RNODES(node);
	pi = 0;

next_byte:

/* end of key
  Here we have two cases:
  [p][r][e][f][i][x] - prefix
  [p][r][e] - new key

  or exact match:
  [p][r][e][f][i][x] - prefix
  [p][r][e][f][i][x] - new key
*/
	if (sym >= ((unsigned char *)key->data + key->len)) {
		tkvdb_memnode *newroot, *subnode_rest;

		if (pi == node->prefix_size) {
			/* exact match */
			if ((node->val_size == val->len) && (val->len != 0)) {
				/* same value size, so copy new value and
					return */
				memcpy(node->prefix_val_meta
					+ node->prefix_size,
					val->data, val->len);
				return TKVDB_OK;
			}

			newroot = tkvdb_node_new(tr, TKVDB_NODE_VAL,
				pi, node->prefix_val_meta,
				val->len, val->data);
			if (!newroot) return TKVDB_ENOMEM;

			tkvdb_clone_subnodes(newroot, node);

			TKVDB_REPLACE_NODE(node, newroot);

			return TKVDB_OK;
		}

/* split node with prefix
  [p][r][e][f][i][x] - prefix
  [p][r][e] - new key

  becomes
  [p][r][e] - new root
  next['f'] => [i][x] - tail
*/
		newroot = tkvdb_node_new(tr, TKVDB_NODE_VAL, pi,
			node->prefix_val_meta,
			val->len, val->data);
		if (!newroot) return TKVDB_ENOMEM;

		subnode_rest = tkvdb_node_new(tr, node->type,
			node->prefix_size - pi - 1,
			node->prefix_val_meta + pi + 1,
			node->val_size,
			node->prefix_val_meta + node->prefix_size);

		if (!subnode_rest) {
			if (tr->tr_buf_dynalloc) {
				free(newroot);
			}
			return TKVDB_ENOMEM;
		}
		tkvdb_clone_subnodes(subnode_rest, node);

		newroot->next[node->prefix_val_meta[pi]] = subnode_rest;

		TKVDB_REPLACE_NODE(node, newroot);

		return TKVDB_OK;
	}

/* end of prefix
  [p][r][e][f][i][x] - old prefix
  [p][r][e][f][i][x][n][e][w]- new prefix

  so we hold old node and change only pointer to next
  [p][r][e][f][i][x]
  next['n'] => [e][w] - tail
*/
	if (pi >= node->prefix_size) {
		if (node->next[*sym] != NULL) {
			/* continue with next node */
			node = node->next[*sym];
			sym++;
			goto next_node;
		} else if (tr->db && (node->fnext[*sym] != 0)) {
			tkvdb_memnode *tmp;

			/* load subnode from disk */
			TKVDB_EXEC( tkvdb_node_read(tr, node->fnext[*sym],
				&tmp) );

			node->next[*sym] = tmp;
			node = tmp;
			sym++;
			goto next_node;
		} else {
			tkvdb_memnode *tmp;

			/* allocate tail */
			tmp = tkvdb_node_new(tr, TKVDB_NODE_VAL,
				key->len -
					(sym - (unsigned char *)key->data) - 1,
				sym + 1,
				val->len, val->data);
			if (!tmp) return TKVDB_ENOMEM;

			node->next[*sym] = tmp;
			return TKVDB_OK;
		}
	}

/* node prefix don't match with corresponding part of key
  [p][r][e][f][i][x] - old prefix
  [p][r][e][p][a][r][e]- new prefix

  [p][r][e] - new root
  next['f'] => [i][x] - tail from old prefix
  next['p'] => [a][r][e] - tail from new prefix
*/
	if (node->prefix_val_meta[pi] != *sym) {
		tkvdb_memnode *newroot, *subnode_rest, *subnode_key;

		/* split current node into 3 subnodes */
		newroot = tkvdb_node_new(tr, 0, pi,
			node->prefix_val_meta, 0, NULL);
		if (!newroot) return TKVDB_ENOMEM;

		/* rest of prefix (skip current symbol) */
		subnode_rest = tkvdb_node_new(tr, node->type,
			node->prefix_size - pi - 1,
			node->prefix_val_meta + pi + 1,
			node->val_size,
			node->prefix_val_meta + node->prefix_size);
		if (!subnode_rest) {
			if (tr->tr_buf_dynalloc) {
				free(newroot);
			}
			return TKVDB_ENOMEM;
		}
		tkvdb_clone_subnodes(subnode_rest, node);

		/* rest of key */
		subnode_key = tkvdb_node_new(tr, TKVDB_NODE_VAL,
			key->len -
				(sym - (unsigned char *)key->data) - 1,
			sym + 1,
			val->len, val->data);
		if (!subnode_key) {
			if (tr->tr_buf_dynalloc) {
				free(subnode_rest);
				free(newroot);
			}
			return TKVDB_ENOMEM;
		}

		newroot->next[node->prefix_val_meta[pi]] = subnode_rest;
		newroot->next[*sym] = subnode_key;

		TKVDB_REPLACE_NODE(node, newroot);

		return TKVDB_OK;
	}

	sym++;
	pi++;
	goto next_byte;

	return TKVDB_OK;
}

/* cursors */

tkvdb_cursor *
tkvdb_cursor_create(tkvdb_tr *tr)
{
	tkvdb_cursor *c;

	c = malloc(sizeof(tkvdb_cursor));
	if (!c) {
		return NULL;
	}

	c->stack_size = 0;

	c->prefix_size = 0;
	c->prefix = NULL;

	c->val_size = 0;
	c->val = NULL;

	c->tr = tr;

	return c;
}

TKVDB_RES
tkvdb_cursor_free(tkvdb_cursor *c)
{
	if (c->prefix) {
		free(c->prefix);
		c->prefix = NULL;
	}
	c->prefix_size = 0;

	c->val_size = 0;
	c->val = NULL;

	c->stack_size = 0;

	free(c);

	return TKVDB_OK;
}

/* todo: rewrite */
static int
tkvdb_cursor_expand_prefix(tkvdb_cursor *c, int n)
{
	unsigned char *tmp_pfx;

	if (n == 0) {
		/* underflow */
		if (c->prefix) {
			free(c->prefix);
			c->prefix = NULL;
		}
		return TKVDB_OK;
	}

	/* empty key is ok */
	if ((c->prefix_size + n) == 0) {
		free(c->prefix);
		c->prefix = NULL;
		return TKVDB_OK;
	}

	tmp_pfx = realloc(c->prefix, c->prefix_size + n);
	if (!tmp_pfx) {
		free(c->prefix);
		c->prefix = NULL;
		return TKVDB_ENOMEM;
	}
	c->prefix = tmp_pfx;

	return TKVDB_OK;
}

/* add (push) node to cursor */
static int
tkvdb_cursor_push(tkvdb_cursor *c, tkvdb_memnode *node, int off)
{
	c->stack[c->stack_size].node = node;
	c->stack[c->stack_size].off = off;
	c->stack_size++;

	c->val_size = node->val_size;
	c->val = node->prefix_val_meta + node->prefix_size;

	return TKVDB_OK;
}

/* pop node from cursor */
static int
tkvdb_cursor_pop(tkvdb_cursor *c)
{
	int r;
	tkvdb_memnode *node;

	if (c->stack_size <= 1) {
		return TKVDB_NOT_FOUND;
	}

	node = c->stack[c->stack_size - 1].node;
	/* erase prefix */
	if ((r = tkvdb_cursor_expand_prefix(c, -(node->prefix_size + 1)))
		!= TKVDB_OK) {
		return r;
	}
	c->prefix_size -= node->prefix_size + 1;

	c->stack_size--;

	c->val_size = node->val_size;
	c->val = node->prefix_val_meta + node->prefix_size;

	return TKVDB_OK;
}

static TKVDB_RES
tkvdb_cursor_append(tkvdb_cursor *c, uint8_t *str, size_t n)
{
	if (n > 0) {
		TKVDB_EXEC( tkvdb_cursor_expand_prefix(c, n) );
		memcpy(c->prefix + c->prefix_size, str, n);
		c->prefix_size += n;
	}

	return TKVDB_OK;
}

static TKVDB_RES
tkvdb_cursor_append_sym(tkvdb_cursor *c, int sym)
{
	TKVDB_EXEC( tkvdb_cursor_expand_prefix(c, 1) );
	c->prefix[c->prefix_size] = sym;
	c->prefix_size++;

	return TKVDB_OK;
}

static void
tkvdb_cursor_reset(tkvdb_cursor *c)
{
	c->stack_size = 0;
	if (c->prefix_size && c->prefix) {
		free(c->prefix);
		c->prefix = NULL;
	}
	c->prefix_size = 0;

	c->val_size = 0;
	c->val = NULL;
}

static TKVDB_RES
tkvdb_smallest(tkvdb_cursor *c, tkvdb_memnode *node)
{
	int off;
	tkvdb_memnode *next;

	for (;;) {
		/* skip replaced nodes */
		TKVDB_SKIP_RNODES(node);

		/* if node has prefix, append it to cursor */
		if (node->prefix_size > 0) {
			TKVDB_EXEC( tkvdb_cursor_expand_prefix(c,
				node->prefix_size) );

			/* append prefix */
			memcpy(c->prefix + c->prefix_size,
				node->prefix_val_meta,
				node->prefix_size);
			c->prefix_size += node->prefix_size;
		}

		/* stop search at key-value node */
		if (node->type & TKVDB_NODE_VAL) {
			TKVDB_EXEC( tkvdb_cursor_push(c, node, /*off*/-1) );
			break;
		}

		/* if current node is key without value, search in subnodes */
		off = 0;

		TKVDB_SUBNODE_SEARCH(c->tr, node, next, off, 1);
		if (!next) {
			/* key node and no subnodes, return error */
			return TKVDB_CORRUPTED;
		}

		TKVDB_EXEC( tkvdb_cursor_expand_prefix(c, 1) );

		c->prefix[c->prefix_size] = off;
		c->prefix_size++;

		/* push node */
		TKVDB_EXEC( tkvdb_cursor_push(c, node, off) );

		node = next;
	}

	return TKVDB_OK;
}

static TKVDB_RES
tkvdb_biggest(tkvdb_cursor *c, tkvdb_memnode *node)
{
	int off;
	tkvdb_memnode *next;

	for (;;) {
		TKVDB_SKIP_RNODES(node);

		/* if node has prefix, append it to cursor */
		if (node->prefix_size > 0) {
			TKVDB_EXEC( tkvdb_cursor_expand_prefix(c,
				node->prefix_size) );

			/* append prefix */
			memcpy(c->prefix + c->prefix_size,
				node->prefix_val_meta,
				node->prefix_size);
			c->prefix_size += node->prefix_size;
		}

		/* if current node is key without value, search in subnodes */
		off = 255;
		TKVDB_SUBNODE_SEARCH(c->tr, node, next, off, 0);

		if (!next) {
			if (node->type & TKVDB_NODE_VAL) {
				TKVDB_EXEC( tkvdb_cursor_push(c, node, -1) );
				break;
			} else {
				return TKVDB_CORRUPTED;
			}
		}

		TKVDB_EXEC( tkvdb_cursor_expand_prefix(c, 1) );

		c->prefix[c->prefix_size] = off;
		c->prefix_size++;

		TKVDB_EXEC( tkvdb_cursor_push(c, node, off) );

		node = next;
	}

	return TKVDB_OK;
}

static TKVDB_RES
tkvdb_cursor_load_root(tkvdb_cursor *c)
{
	if (!c->tr->root) {
		/* empty root node */
		if (!c->tr->db) {
			/* and no database file */
			return TKVDB_EMPTY;
		}

		if (c->tr->db->info.filesize == 0) {
			/* database is empty */
			return TKVDB_EMPTY;
		}
		/* try to read root node */
		TKVDB_EXEC( tkvdb_node_read(c->tr,
			c->tr->db->info.footer.root_off, &(c->tr->root)) );
	}

	return TKVDB_OK;
}

TKVDB_RES
tkvdb_first(tkvdb_cursor *c)
{
	tkvdb_cursor_reset(c);
	TKVDB_EXEC( tkvdb_cursor_load_root(c) );
	return tkvdb_smallest(c, c->tr->root);
}

TKVDB_RES
tkvdb_last(tkvdb_cursor *c)
{
	tkvdb_cursor_reset(c);
	TKVDB_EXEC( tkvdb_cursor_load_root(c) );
	return tkvdb_biggest(c, c->tr->root);
}

/* seek to key (or to nearest key, less or greater) */
TKVDB_RES
tkvdb_seek(tkvdb_cursor *c, const tkvdb_datum *key, TKVDB_SEEK seek)
{
	tkvdb_memnode *node, *next;

	const uint8_t *sym;
	size_t pi;
	int off = 0;

	TKVDB_EXEC( tkvdb_cursor_load_root(c) );
	tkvdb_cursor_reset(c);

	node = c->tr->root;
	sym = key->data;

next_node:
	TKVDB_SKIP_RNODES(node);
	pi = 0;

next_byte:
	if (sym >= ((uint8_t *)key->data + key->len)) {
		/* end of key */
		if ((pi == node->prefix_size)
			&& (node->type & TKVDB_NODE_VAL)) {
			TKVDB_EXEC ( tkvdb_cursor_append(c,
				node->prefix_val_meta, node->prefix_size) );
			TKVDB_EXEC ( tkvdb_cursor_push(c, node, *sym) );
			return TKVDB_OK;
		}

		if (seek == TKVDB_SEEK_EQ) {
			return TKVDB_NOT_FOUND;
		}

		TKVDB_EXEC ( tkvdb_smallest(c, node) );
		if (seek == TKVDB_SEEK_LE) {
			return tkvdb_prev(c);
		}

		return TKVDB_OK;
	}

	if (pi >= node->prefix_size) {
		/* end of prefix (but not the key) */
		next = NULL;
		TKVDB_SUBNODE_NEXT(c->tr, node, next, *sym);
		if (next) {
			TKVDB_EXEC ( tkvdb_cursor_append(c,
				node->prefix_val_meta, node->prefix_size) );

			TKVDB_EXEC ( tkvdb_cursor_append_sym(c, *sym) );
			TKVDB_EXEC ( tkvdb_cursor_push(c, node, *sym) );

			node = next;
			sym++;
			goto next_node;
		}

		if (seek == TKVDB_SEEK_EQ) {
			tkvdb_cursor_reset(c);
			return TKVDB_NOT_FOUND;
		}

		off = *sym;
		if (seek == TKVDB_SEEK_LE) {
			TKVDB_SUBNODE_SEARCH(c->tr, node, next, off, 0);
			if (next) {
				TKVDB_EXEC ( tkvdb_cursor_append_sym(c, off) );
				TKVDB_EXEC ( tkvdb_cursor_push(c, node, off) );
				return tkvdb_biggest(c, next);
			}
			if (node->type & TKVDB_NODE_VAL) {
				return TKVDB_OK;
			}
			TKVDB_EXEC ( tkvdb_smallest(c, node) );
			return tkvdb_prev(c);
		} else {
			/* greater */
			TKVDB_SUBNODE_SEARCH(c->tr, node, next, off, 1);
			if (next) {
				TKVDB_EXEC ( tkvdb_cursor_append_sym(c, off) );
				TKVDB_EXEC ( tkvdb_cursor_push(c, node, off) );
				return tkvdb_smallest(c, next);
			}

			TKVDB_EXEC ( tkvdb_biggest(c, node) );
			return tkvdb_next(c);
		}
	}

	if (node->prefix_val_meta[pi] != *sym) {
		if (seek == TKVDB_SEEK_EQ) {
			tkvdb_cursor_reset(c);
			return TKVDB_NOT_FOUND;
		}

		if (seek == TKVDB_SEEK_LE) {
			if (node->prefix_val_meta[pi] < *sym) {
				/* symbol in prefix is lesser than in key */
				return tkvdb_biggest(c, node);
			}
			/* not optimal, we push node and pop it in prev() */
			TKVDB_EXEC (tkvdb_cursor_append(c,
				node->prefix_val_meta, node->prefix_size) );
			TKVDB_EXEC ( tkvdb_cursor_push(c, node, -1) );
			return tkvdb_prev(c);
		} else {
			/* greater */
			if (node->prefix_val_meta[pi] > *sym) {
				return tkvdb_smallest(c, node);
			}
			TKVDB_EXEC (tkvdb_cursor_append(c,
				node->prefix_val_meta, node->prefix_size) );
			TKVDB_EXEC ( tkvdb_cursor_push(c, node, *sym) );
			return tkvdb_next(c);
		}
	}

	sym++;
	pi++;
	goto next_byte;

	/* unreachable */
	return TKVDB_OK;
}

TKVDB_RES
tkvdb_next(tkvdb_cursor *c)
{
	int *off;
	tkvdb_memnode *node, *next;

	for (;;) {
		if (c->stack_size < 1) {
			break;
		}

		/* get node from stack's top */
		node = c->stack[c->stack_size - 1].node;
		off = &(c->stack[c->stack_size - 1].off);
		(*off)++;

		if (*off > 255) {
			TKVDB_EXEC( tkvdb_cursor_pop(c) );
			continue;
		}

		TKVDB_SUBNODE_SEARCH(c->tr, node, next, *off, 1);

		if (next) {
			/* expand cursor key */
			TKVDB_EXEC( tkvdb_cursor_expand_prefix(c, 1) );
			c->prefix[c->prefix_size] = *off;
			c->prefix_size++;

			return tkvdb_smallest(c, next);
		}

		/* pop */
		TKVDB_EXEC( tkvdb_cursor_pop(c) );
	}

	return TKVDB_NOT_FOUND;
}

TKVDB_RES
tkvdb_prev(tkvdb_cursor *c)
{
	int *off;
	tkvdb_memnode *node, *next = NULL;

	for (;;) {
		if (c->stack_size < 1) {
			return TKVDB_NOT_FOUND;
		}

		node = c->stack[c->stack_size - 1].node;
		off = &(c->stack[c->stack_size - 1].off);
		(*off)--;

		/* special case? */
		if ((*off == -1) && (node->type & TKVDB_NODE_VAL)) {
			break;
		}

		if (*off < 0) {
			TKVDB_EXEC( tkvdb_cursor_pop(c) );
			continue;
		}

		TKVDB_SUBNODE_SEARCH(c->tr, node, next, *off, 0);

		if (next) {
			TKVDB_EXEC( tkvdb_cursor_expand_prefix(c, 1) );
			c->prefix[c->prefix_size] = *off;
			c->prefix_size++;

			return tkvdb_biggest(c, next);
		}

		if (node->type & TKVDB_NODE_VAL) {
			break;
		}

		TKVDB_EXEC( tkvdb_cursor_pop(c) );
	}

	return TKVDB_OK;
}

void *
tkvdb_cursor_key(tkvdb_cursor *c)
{
	return c->prefix;
}

size_t
tkvdb_cursor_keysize(tkvdb_cursor *c)
{
	return c->prefix_size;
}

void *
tkvdb_cursor_val(tkvdb_cursor *c)
{
	return c->val;
}

size_t
tkvdb_cursor_valsize(tkvdb_cursor *c)
{
	return c->val_size;
}

tkvdb_tr *
tkvdb_tr_create_m(tkvdb *db, size_t limit, int dynalloc)
{
	tkvdb_tr *tr;

	tr = malloc(sizeof(tkvdb_tr));
	if (!tr) {
		return NULL;
	}

	tr->db = db;
	tr->root = NULL;

	tr->started = 0;

	tr->tr_buf_dynalloc = dynalloc;
	tr->tr_buf_limit = limit;

	if (!tr->tr_buf_dynalloc) {
		tr->tr_buf = malloc(tr->tr_buf_limit);
		if (tr->tr_buf) {
			tr->tr_buf_ptr = tr->tr_buf;
		} else {
			free(tr);
			tr = NULL;
		}
	} else {
		tr->tr_buf = NULL;
		tr->tr_buf_ptr = NULL;
	}
	tr->tr_buf_allocated = 0;

	return tr;
}

tkvdb_tr *
tkvdb_tr_create(tkvdb *db)
{
	tkvdb_tr *tr;

	if (db) {
		/* inherit from database parameters */
		tr = tkvdb_tr_create_m(db,
			db->params.tr_buf_limit, db->params.tr_buf_dynalloc);
	} else {
		/* if no db, use dynamic allocation without limits */
		tr = tkvdb_tr_create_m(db, SIZE_MAX, 1);
	}

	return tr;
}


/* reset transaction to initial state */
static void
tkvdb_tr_reset(tkvdb_tr *tr)
{
	if (tr->tr_buf_dynalloc) {
		if (tr->root) {
			tkvdb_node_free(tr->root);
		}
	} else {
		tr->tr_buf_ptr = tr->tr_buf;
	}

	tr->root = NULL;

	tr->tr_buf_allocated = 0;
	tr->started = 0;
}

void
tkvdb_tr_free(tkvdb_tr *tr)
{
	if (tr->tr_buf_dynalloc) {
		tkvdb_tr_reset(tr);
	} else {
		free(tr->tr_buf);
	}

	free(tr);
}


TKVDB_RES
tkvdb_begin(tkvdb_tr *tr)
{
	if (tr->started) {
		/* ignore if transaction is already started */
		return TKVDB_OK;
	}

	if (!tr->db) {
		/* no underlying database file */
		tr->started = 1;
		return TKVDB_OK;
	}

	/* read database info to find root node */
	TKVDB_EXEC( tkvdb_info_read(tr->db->fd, &(tr->db->info)) );

	if (tr->db->info.filesize == 0) {
		memset(&(tr->db->info.footer),
			0, sizeof(struct tkvdb_tr_footer));
	} else {
		/* increase transaction number */
		tr->db->info.footer.transaction_id += 1;
	}

	tr->started = 1;

	return TKVDB_OK;
}

TKVDB_RES
tkvdb_rollback(tkvdb_tr *tr)
{
	tkvdb_tr_reset(tr);

	return TKVDB_OK;
}

static TKVDB_RES
tkvdb_writebuf_realloc(tkvdb *db, size_t new_size)
{
	if (new_size > db->params.write_buf_limit) {
		/* not enough space for node in buffer */
		return TKVDB_ENOMEM;
	}
	if (new_size > db->write_buf_allocated) {
		uint8_t *tmp;

		if (!db->params.write_buf_dynalloc) {
			return TKVDB_ENOMEM;
		}

		tmp = realloc(db->write_buf, new_size);
		if (!tmp) {
			return TKVDB_ENOMEM;
		}

		db->write_buf = tmp;
		db->write_buf_allocated = new_size;
	}

	return TKVDB_OK;
}

/* compact node and put it to write buffer */
static TKVDB_RES
tkvdb_node_to_buf(tkvdb *db, tkvdb_memnode *node, uint64_t transaction_off)
{
	struct tkvdb_disknode *disknode;
	uint8_t *ptr;
	uint64_t iobuf_off;

	iobuf_off = node->disk_off - transaction_off;

	TKVDB_EXEC( tkvdb_writebuf_realloc(db, iobuf_off + node->disk_size) );

	disknode = (struct tkvdb_disknode *)(db->write_buf + iobuf_off);

	disknode->size = node->disk_size;
	disknode->type = node->type;
	disknode->nsubnodes = node->nsubnodes;
	disknode->prefix_size = node->prefix_size;

	ptr = disknode->data;

	if (node->type & TKVDB_NODE_VAL) {
		*((uint32_t *)ptr) = node->val_size;
		ptr += sizeof(uint32_t);
	}
	if (node->type & TKVDB_NODE_META) {
		*((uint32_t *)ptr) = node->meta_size;
		ptr += sizeof(uint32_t);
	}

	if (node->nsubnodes > TKVDB_SUBNODES_THR) {
		memcpy(ptr, node->fnext, sizeof(uint64_t) * 256);
		ptr += sizeof(uint64_t) * 256;
	} else {
		int i;
		uint8_t *symbols;

		/* array of next symbols */
		symbols = ptr;
		ptr += node->nsubnodes * sizeof(uint8_t);
		for (i=0; i<256; i++) {
			if (node->fnext[i]) {
				*symbols = i;
				symbols++;

				*((uint64_t *)ptr) = node->fnext[i];
				ptr += sizeof(uint64_t);
			}
		}
	}

	memcpy(ptr, node->prefix_val_meta, node->prefix_size + node->val_size
		+ node->meta_size);

	return TKVDB_OK;
}

/* calculate size of node on disk */
static void
tkvdb_node_calc_disksize(tkvdb_memnode *node)
{
	unsigned int i;

	node->nsubnodes = 0;

	for (i=0; i<256; i++) {
		if (node->next[i] || node->fnext[i]) {
			node->nsubnodes++;
		}
	}

	node->disk_size = sizeof(struct tkvdb_disknode) - 1;

	/* if node has value add 4 bytes for value size */
	if (node->type & TKVDB_NODE_VAL) {
		node->disk_size += sizeof(uint32_t);
	}
	/* 4 bytes for metadata size */
	if (node->type & TKVDB_NODE_META) {
		node->disk_size += sizeof(uint32_t);
	}

	/* subnodes */
	if (node->nsubnodes > TKVDB_SUBNODES_THR) {
		node->disk_size += 256 * sizeof(uint64_t);
	} else {
		node->disk_size += node->nsubnodes * sizeof(uint8_t)
			+ node->nsubnodes * sizeof(uint64_t);
	}

	/* prefix + value + metadata */
	node->disk_size += node->prefix_size + node->val_size
		+ node->meta_size;
}

/* commit and return new root offset */
static TKVDB_RES
tkvdb_do_commit(tkvdb_tr *tr, uint64_t *gap_end_ptr)
{
	size_t stack_depth = 0;
	struct tkvdb_visit_helper stack[TKVDB_STACK_MAX_DEPTH];

	struct tkvdb_db_info info;

	/* offset of whole transaction in file */
	uint64_t transaction_off;
	/* offset of next node in file */
	uint64_t node_off;
	/* size of last accessed node, will be added to node_off */
	uint64_t last_node_size;
	int append;
	struct tkvdb_tr_header *header_ptr;

	tkvdb_memnode *node;
	int off = 0;
	TKVDB_RES r = TKVDB_OK;

	if (!tr->started) {
		return TKVDB_NOT_STARTED;
	}

	if (!tr->db) {
		tkvdb_tr_reset(tr);
		return TKVDB_OK;
	}

	if (!tr->root) {
		/* empty transaction, rollback */
		tkvdb_tr_reset(tr);
		return TKVDB_OK;
	}

	/* read transaction footer before commit to make some checks */
	TKVDB_EXEC( tkvdb_info_read(tr->db->fd, &info) );

	if (info.filesize != tr->db->info.filesize) {
		/* file was modified during transaction */
		return TKVDB_MODIFIED;
	}

	if (info.filesize > 0) {
		if ((info.footer.transaction_id + 1)
			!= tr->db->info.footer.transaction_id) {

			return TKVDB_MODIFIED;
		}

		if ((info.footer.gap_end - info.footer.gap_begin)
			> tr->tr_buf_allocated) {

			/* we have enough space in vacuumed gap */
			transaction_off = info.footer.gap_begin;
			append = 0;
		} else {
			/* append transaction to the end of file */
			transaction_off = info.filesize;
			append = 1;
		}
	} else {
		/* empty data file */
		memcpy(tr->db->info.footer.signature,
			TKVDB_SIGNATURE,
			sizeof(TKVDB_SIGNATURE) - 1);

		transaction_off = 0;
		append = 1;
	}

	/* first node offset, skip transaction header */
	node_off = transaction_off + sizeof(struct tkvdb_tr_header);

	last_node_size = 0;

	/* now iterate through nodes in transaction */
	node = tr->root;

	for (;;) {
		tkvdb_memnode *next;

		TKVDB_SKIP_RNODES(node);

		if (node->disk_size == 0) {
			tkvdb_node_calc_disksize(node);

			node->disk_off = node_off;
			last_node_size = node->disk_size;
		}

		next = NULL;
		for (; off<256; off++) {
			if (node->next[off]) {
				/* found next subnode */
				next = node->next[off];
				break;
			}
		}

		if (next) {
			TKVDB_SKIP_RNODES(next);

			node_off += last_node_size;
			node->fnext[off] = node_off;

			/* push node and position to stack */
			stack[stack_depth].node = node;
			stack[stack_depth].off = off;
			stack_depth++;

			node = next;
			off = 0;
		} else {
			/* no more subnodes, serialize node to memory buffer */
			r = tkvdb_node_to_buf(tr->db, node, transaction_off);
			if (r != TKVDB_OK) {
				goto fail_node_to_buf;
			}

			/* pop */
			if (stack_depth == 0) {
				break;
			}

			stack_depth--;
			node = stack[stack_depth].node;
			off  = stack[stack_depth].off + 1;
		}
	}

	node_off += last_node_size;

	tr->db->info.footer.root_off = transaction_off
		+ sizeof(struct tkvdb_tr_header);
	tr->db->info.footer.transaction_size = node_off - transaction_off;

	/* seek */
	if (lseek(tr->db->fd, transaction_off, SEEK_SET)
		!= (off_t)transaction_off) {
		return TKVDB_IO_ERROR;
	}

	/* prepare header, footer and write */
	header_ptr = (struct tkvdb_tr_header *)tr->db->write_buf;
	header_ptr->type = TKVDB_BLOCKTYPE_TRANSACTION;
	tr->db->info.footer.type = TKVDB_BLOCKTYPE_FOOTER;
	if (gap_end_ptr) {
		tr->db->info.footer.gap_end = *gap_end_ptr;
	}
	if (append) {
		ssize_t wsize;
		struct tkvdb_tr_footer *footer_ptr;

		header_ptr->footer_off = node_off;

		wsize = tr->db->info.footer.transaction_size
			+ TKVDB_TR_FTRSIZE;

		/* try to append footer to buffer */
		TKVDB_EXEC( tkvdb_writebuf_realloc(tr->db, wsize) );

		footer_ptr = (struct tkvdb_tr_footer *)
			&(tr->db->write_buf[wsize - TKVDB_TR_FTRSIZE]);

		*footer_ptr = tr->db->info.footer;
		if (write(tr->db->fd, tr->db->write_buf, wsize) != wsize) {
			return TKVDB_IO_ERROR;
		}
	} else {
		ssize_t wsize;

		wsize = tr->db->info.footer.transaction_size;
		tr->db->info.footer.gap_begin += wsize;

		header_ptr->footer_off = tr->db->info.filesize;
		if (write(tr->db->fd, tr->db->write_buf, wsize) != wsize) {
			return TKVDB_IO_ERROR;
		}
		/* seek to end of file */
		if (lseek(tr->db->fd, tr->db->info.filesize, SEEK_SET)
			!= (off_t)tr->db->info.filesize) {

			return TKVDB_IO_ERROR;
		}
		/* write footer */
		wsize = sizeof(struct tkvdb_tr_footer);
		if (write(tr->db->fd, &tr->db->info.footer, wsize) != wsize) {
			return TKVDB_IO_ERROR;
		}
	}

	r = TKVDB_OK;

	/* return root offset */
/*
	if (root_off) {
		*root_off = tr->db->info.footer.root_off;
	}
*/
/*
	if (sync && (fsync(tr->db->fd) < 0)) {
		return TKVDB_IO_ERROR;
	}
*/
fail_node_to_buf:
	tkvdb_tr_reset(tr);

	return r;
}

TKVDB_RES
tkvdb_commit(tkvdb_tr *tr)
{
	return tkvdb_do_commit(tr, NULL);
}

static TKVDB_RES
tkvdb_do_del(tkvdb_tr *tr, tkvdb_memnode *node, tkvdb_memnode *prev,
	int prev_off, int del_pfx)
{
	int i, n_subnodes = 0, concat_sym = -1;
	tkvdb_memnode *new_node, *old_node;

	if (!prev) {
		/* remove root node */
		tkvdb_node_free(node);
		node = tkvdb_node_new(tr, 0, 0, NULL, 0, NULL);
		if (!node) {
			return TKVDB_ENOMEM;
		}
		tr->root = node;

		return TKVDB_OK;
	}

	if (del_pfx) {
		prev->next[prev_off] = NULL;
		prev->fnext[prev_off] = 0;
		tkvdb_node_free(node);
		return TKVDB_OK;
	} else if (node->type & TKVDB_NODE_VAL) {
		/* check if we have at least 1 subnode */
		for (i=0; i<256; i++) {
			if (node->next[i] || node->fnext[i]) {
				n_subnodes = 1;
				break;
			}
		}

		if (!n_subnodes) {
			/* no subnodes, delete node */
			prev->next[prev_off] = NULL;
			prev->fnext[prev_off] = 0;
			tkvdb_node_free(node);
			return TKVDB_OK;
		}
		/* we have subnodes, so just clear value bit */
		node->type &= ~TKVDB_NODE_VAL;
		return TKVDB_OK;
	} else {
		return TKVDB_NOT_FOUND;
	}

	if (prev->type & TKVDB_NODE_VAL) {
		return TKVDB_OK;
	}

	/* calculate number of subnodes in parent (prev) */
	for (i=0; i<256; i++) {
		if (prev->next[i] || prev->fnext[i]) {
			n_subnodes++;
			if (n_subnodes > 1) {
				/* more than one subnode */
				return TKVDB_OK;
			}
			concat_sym = i;
		}
	}

	if (n_subnodes == 0) {
		return TKVDB_CORRUPTED;
	}


	/* we have parent node with just one subnode */
	old_node = prev->next[concat_sym];
	if (!old_node) {
		TKVDB_EXEC( tkvdb_node_read(tr, prev->fnext[concat_sym],
			&old_node) );
	}
	/* allocate new (concatenated) node */
	new_node = tkvdb_node_alloc(tr, sizeof(tkvdb_memnode)
		+ prev->prefix_size + 1
		+ old_node->prefix_size
		+ old_node->val_size + old_node->meta_size);
	if (!new_node) {
		return TKVDB_ENOMEM;
	}

	new_node->type = old_node->type;
	new_node->prefix_size = prev->prefix_size + 1 + old_node->prefix_size;
	new_node->val_size = old_node->val_size;
	new_node->meta_size = old_node->meta_size;

	if (prev->prefix_size > 0) {
		memcpy(new_node->prefix_val_meta, prev->prefix_val_meta,
			prev->prefix_size);
	}
	new_node->prefix_val_meta[prev->prefix_size] = concat_sym;
	if (old_node->prefix_size > 0) {
		memcpy(new_node->prefix_val_meta + prev->prefix_size + 1,
			old_node->prefix_val_meta,
			old_node->prefix_size);
	}

	if (old_node->val_size > 0) {
		memcpy(new_node->prefix_val_meta + new_node->prefix_size,
			old_node->prefix_val_meta + old_node->prefix_size,
			old_node->val_size);
	}
	memcpy(new_node->next, old_node->next, sizeof(tkvdb_memnode *) * 256);
	memcpy(new_node->fnext, old_node->fnext, sizeof(uint64_t) * 256);

	new_node->disk_size = 0;
	new_node->disk_off = 0;

	TKVDB_REPLACE_NODE(prev, new_node);

	return TKVDB_OK;
}

TKVDB_RES
tkvdb_del(tkvdb_tr *tr, const tkvdb_datum *key, int del_pfx)
{
	const unsigned char *sym;
	tkvdb_memnode *node, *prev;
	size_t pi;
	int prev_off = 0;

	if (!tr->started) {
		return TKVDB_NOT_STARTED;
	}

	/* check root */
	if (tr->root == NULL) {
		if (tr->db && (tr->db->info.filesize > 0)) {
			/* we have underlying non-empty db file */
			TKVDB_EXEC( tkvdb_node_read(tr,
				tr->db->info.footer.root_off,
				&(tr->root)) );
		} else {
			return TKVDB_EMPTY;
		}
	}

	sym = key->data;
	node = tr->root;
	prev = NULL;

next_node:
	TKVDB_SKIP_RNODES(node);
	pi = 0;

next_byte:

	if (sym >= ((unsigned char *)key->data + key->len)) {
		/* end of key */
		if (pi == node->prefix_size) {
			/* exact match */
			return tkvdb_do_del(tr, node, prev, prev_off, del_pfx);
		}
	}

	if (pi >= node->prefix_size) {
		/* end of prefix */
		if (node->next[*sym] != NULL) {
			/* continue with next node */
			prev = node;
			prev_off = *sym;

			node = node->next[*sym];
			sym++;
			goto next_node;
		} else if (tr->db && (node->fnext[*sym] != 0)) {
			tkvdb_memnode *tmp;

			/* load subnode from disk */
			TKVDB_EXEC( tkvdb_node_read(tr, node->fnext[*sym],
				&tmp) );

			prev = node;
			prev_off = *sym;

			node->next[*sym] = tmp;
			node = tmp;
			sym++;
			goto next_node;
		} else {
			return TKVDB_NOT_FOUND;
		}
	}

	if (node->prefix_val_meta[pi] != *sym) {
		return TKVDB_NOT_FOUND;
	}

	sym++;
	pi++;
	goto next_byte;

	return TKVDB_OK;
}

/* get value for given key */
TKVDB_RES
tkvdb_get(tkvdb_tr *tr, const tkvdb_datum *key, tkvdb_datum *val)
{
	const unsigned char *sym;
	size_t pi;
	tkvdb_memnode *node = NULL;
	uint64_t off;

	if (!tr->started) {
		return TKVDB_NOT_STARTED;
	}

	/* check root */
	if (tr->root == NULL) {
		if (tr->db && (tr->db->info.filesize > 0)) {
			/* we have underlying non-empty db file */
			TKVDB_EXEC( tkvdb_node_read(tr,
				tr->db->info.footer.root_off,
				&(tr->root)) );
		} else {
			return TKVDB_EMPTY;
		}
	}

	sym = key->data;
	node = tr->root;
	off = tr->db->info.footer.root_off;

next_node:
	TKVDB_SKIP_RNODES(node);
	pi = 0;

next_byte:

	if (sym >= ((unsigned char *)key->data + key->len)) {
		/* end of key */
		if ((pi == node->prefix_size)
			&& (node->type & TKVDB_NODE_VAL)) {
			/* exact match and node with value */
			val->len = node->val_size;
			val->data = node->prefix_val_meta + node->prefix_size;
			return TKVDB_OK;
		} else {
			return TKVDB_NOT_FOUND;
		}
	}

	if (pi >= node->prefix_size) {
		/* end of prefix */
		if (node->next[*sym] != NULL) {
			/* continue with next node */
			node = node->next[*sym];
			sym++;
			goto next_node;
		} else if (tr->db && (node->fnext[*sym] != 0)) {
			tkvdb_memnode *tmp;

			/* load subnode from disk */
			off = node->fnext[*sym];
			TKVDB_EXEC( tkvdb_node_read(tr, off,
				&tmp) );

			node->next[*sym] = tmp;
			node = tmp;
			sym++;
			goto next_node;
		} else {
			return TKVDB_NOT_FOUND;
		}
	}

	if (node->prefix_val_meta[pi] != *sym) {
		return TKVDB_NOT_FOUND;
	}

	sym++;
	pi++;
	goto next_byte;

	return TKVDB_OK;
}

/* get value for given key;
 * the only difference from tkvdb_get is that this function sets flag (in_tr)
 * if any part of key (node) is in given range on disk */
static TKVDB_RES
tkvdb_vac_get(tkvdb_tr *tr, const void *key, size_t klen,
	int *in_tr, uint64_t trdisk_begin, uint64_t trdisk_end)
{
	const unsigned char *sym;
	size_t pi;
	tkvdb_memnode *node = NULL;
	uint64_t off;

	if (!tr->started) {
		return TKVDB_NOT_STARTED;
	}

	/* check root */
	if (tr->root == NULL) {
		if (tr->db && (tr->db->info.filesize > 0)) {
			/* we have underlying non-empty db file */
			TKVDB_EXEC( tkvdb_node_read(tr,
				tr->db->info.footer.root_off,
				&(tr->root)) );
		} else {
			return TKVDB_EMPTY;
		}
	}

	sym = key;
	node = tr->root;
	off = tr->db->info.footer.root_off;

	if ((off >= trdisk_begin) && (off <= trdisk_end)) {
		*in_tr = 1;
	} else {
		*in_tr = 0;
	}

next_node:
	TKVDB_SKIP_RNODES(node);
	pi = 0;

next_byte:

	if (sym >= ((unsigned char *)key + klen)) {
		/* end of key */
		if ((pi == node->prefix_size)
			&& (node->type & TKVDB_NODE_VAL)) {
			/* exact match and node with value */
			return TKVDB_OK;
		} else {
			return TKVDB_NOT_FOUND;
		}
	}

	if (pi >= node->prefix_size) {
		/* end of prefix */
		if (node->next[*sym] != NULL) {
			/* continue with next node */
			node = node->next[*sym];
			sym++;
			goto next_node;
		} else if (tr->db && (node->fnext[*sym] != 0)) {
			tkvdb_memnode *tmp;

			/* load subnode from disk */
			off = node->fnext[*sym];
			TKVDB_EXEC( tkvdb_node_read(tr, off,
				&tmp) );

			if ((off >= trdisk_begin) && (off <= trdisk_end)) {
				*in_tr = 1;
			}

			node->next[*sym] = tmp;
			node = tmp;
			sym++;
			goto next_node;
		} else {
			return TKVDB_NOT_FOUND;
		}
	}

	if (node->prefix_val_meta[pi] != *sym) {
		return TKVDB_NOT_FOUND;
	}

	sym++;
	pi++;
	goto next_byte;

	return TKVDB_OK;
}



/* smallest key-value pair of vacuumed transaction */
static TKVDB_RES
tkvdb_vac_smallest(tkvdb_cursor *c, tkvdb_memnode *node,
	uint64_t trdisk_begin, uint64_t trdisk_end)
{
	int off;
	tkvdb_memnode *next;

	for (;;) {
		/* if node has prefix, append it to cursor */
		if (node->prefix_size > 0) {
			TKVDB_EXEC( tkvdb_cursor_expand_prefix(c,
				node->prefix_size) );

			/* append prefix */
			memcpy(c->prefix + c->prefix_size,
				node->prefix_val_meta,
				node->prefix_size);
			c->prefix_size += node->prefix_size;
		}

		/* stop search at key-value node */
		if (node->type & TKVDB_NODE_VAL) {
			TKVDB_EXEC( tkvdb_cursor_push(c, node, -1) );
			break;
		}

		/* if current node is key without value, search in subnodes */
		next = NULL;
		for (off=0; off<256; off++) {
			if ((node->fnext[off] > trdisk_begin)
				&& (node->fnext[off] < trdisk_end)) {

				if (node->next[off]) {
					/* next subnode already loaded */
					next = node->next[off];
				} else {
					tkvdb_memnode *tmp;
					TKVDB_EXEC( tkvdb_node_read(c->tr, node->fnext[off], &tmp) );
					node->next[off] = tmp;
					next = tmp;
				}
				break;
			}
		}

		if (!next) {
			/* XXX: return not found? */
			return TKVDB_CORRUPTED;
		}

		TKVDB_EXEC( tkvdb_cursor_expand_prefix(c, 1) );

		c->prefix[c->prefix_size] = off;
		c->prefix_size++;

		/* push node */
		TKVDB_EXEC( tkvdb_cursor_push(c, node, off) );

		node = next;
	}

	return TKVDB_OK;
}

static TKVDB_RES
tkvdb_vac_next(tkvdb_cursor *c, uint64_t trdisk_begin, uint64_t trdisk_end)
{
	int *off;
	tkvdb_memnode *node, *next;

	for (;;) {
		if (c->stack_size < 1) {
			break;
		}

		/* get node from stack's top */
		node = c->stack[c->stack_size - 1].node;
		off = &(c->stack[c->stack_size - 1].off);
		(*off)++;

		if (*off > 255) {
			TKVDB_EXEC( tkvdb_cursor_pop(c) );
			continue;
		}

		next = NULL;
		for (; (*off)<256; (*off)++) {
			if ((node->fnext[*off] > trdisk_begin)
				&& (node->fnext[*off] <= trdisk_end)) {

				if (node->next[*off]) {
					/* next subnode already loaded */
					next = node->next[*off];
				} else {
					tkvdb_memnode *tmp;
					TKVDB_EXEC( tkvdb_node_read(c->tr, node->fnext[*off], &tmp) );
					node->next[*off] = tmp;
					next = tmp;
				}
				break;
			}
		}

		if (next) {
			/* expand cursor key */
			TKVDB_EXEC( tkvdb_cursor_expand_prefix(c, 1) );
			c->prefix[c->prefix_size] = *off;
			c->prefix_size++;

			return tkvdb_vac_smallest(c, next, trdisk_begin, trdisk_end);
		}

		/* pop */
		TKVDB_EXEC( tkvdb_cursor_pop(c) );
	}

	return TKVDB_OK;
}

TKVDB_RES
tkvdb_vacuum(tkvdb_tr *tr, tkvdb_tr *vac, tkvdb_tr *tres, tkvdb_cursor *c)
{
	struct tkvdb *db;
	struct tkvdb_db_info info;
	size_t vac_tr_start; /* start of vacuumed transaction */
	uint64_t trsize;
	uint64_t root_off; /* offset of root after commit */
	tkvdb_memnode *node;
	TKVDB_RES r;

	db = tr->db;
	if (!db) {
		return TKVDB_OK; /* XXX: return error? */
	}

	TKVDB_EXEC( tkvdb_info_read(db->fd, &info) );

	trsize = info.footer.transaction_size;

	if (info.filesize == 0) {
		/* empty database */
		return TKVDB_OK;
	} else {
		/* read root node */
		TKVDB_EXEC( tkvdb_node_read(tr,
			tr->db->info.footer.root_off,
			&(tr->root)) );
	}

	/* read root node of old transaction */
	TKVDB_EXEC( tkvdb_node_read(vac,
		tr->db->info.footer.gap_end + sizeof(struct tkvdb_tr_header),
		&(vac->root)) );


	node = vac->root;
	/* forcibly assign cursor to vacuumed transaction */
	c->tr = vac;

	TKVDB_EXEC( tkvdb_begin(tres) );

	r = tkvdb_vac_smallest(c, node,
		tr->db->info.footer.gap_end,
		tr->db->info.footer.gap_end + trsize);

	while (r == TKVDB_OK) {
		int in_tr;

		r = tkvdb_vac_get(tr, tkvdb_cursor_key(c),
			tkvdb_cursor_keysize(c),
			&in_tr,
			tr->db->info.footer.gap_end,
			tr->db->info.footer.gap_end + trsize);

		if ((r == TKVDB_OK) && in_tr) {
			/* key is in vac transaction */
			tkvdb_datum key, val;

			key.data = tkvdb_cursor_key(c);
			key.len  = tkvdb_cursor_keysize(c);
			val.data = tkvdb_cursor_val(c);
			val.len  = tkvdb_cursor_valsize(c);
			TKVDB_EXEC( tkvdb_put(tres, &key, &val) );
		}
		r = tkvdb_vac_next(c,
			tr->db->info.footer.gap_end,
			tr->db->info.footer.gap_end + trsize);
	}

	TKVDB_EXEC( tkvdb_do_commit(tres, &root_off) );

	/* FIXME: wrong! (DB header changed after commit) */
	/* write new database footer */

	return TKVDB_OK;
}

TKVDB_RES
tkvdb_dbinfo(tkvdb *db, uint64_t *root_off,
	uint64_t *gap_begin, uint64_t *gap_end)
{
	struct tkvdb_db_info info;

	TKVDB_EXEC( tkvdb_info_read(db->fd, &info) );

	*root_off = info.footer.root_off;

	*gap_begin = info.footer.gap_begin;
	*gap_end = info.footer.gap_end;

	return TKVDB_OK;
}


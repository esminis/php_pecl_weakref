/*
   +----------------------------------------------------------------------+
   | Weakref                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2011 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Etienne Kneuss <colder@php.net>                             |
   +----------------------------------------------------------------------+
 */

/* $Id$ */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "zend_exceptions.h"
#include "ext/standard/info.h"
#include "wr_weakref.h"
#include "php_weakref.h"
#include "ext/spl/php_spl.h"

void wr_store_init() /* {{{ */
{
	wr_store *store = emalloc(sizeof(wr_store));

	zend_hash_init(&store->old_handlers, 0, NULL, NULL, 0);
	zend_hash_init(&store->objs, 0, NULL, NULL, 0);

	WR_G(store) = store;

} /* }}} */


void wr_store_destroy() /* {{{ */
{
	wr_store *store = WR_G(store);

	zend_hash_destroy(&store->old_handlers);
	zend_hash_destroy(&store->objs);

	efree(store);

	WR_G(store) = NULL;

} /* }}} */

/* This function is invoked whenever an object tracked by a weak ref/map is
 * destroyed */
void wr_store_tracked_object_dtor(zend_object *ref_obj) /* {{{ */
{
	wr_store                  *store    = WR_G(store);

	zend_object_handlers      *handlers = zend_hash_index_find_ptr(&store->old_handlers, (ulong)ref_obj->handlers);
	zend_object_dtor_obj_t     orig_dtor  = handlers->dtor_obj;

	/* Original dtor has been called, we invalidate the necessary weakrefs: */
	orig_dtor(ref_obj);

	wr_ref_list *list_entry;
	if ((list_entry = zend_hash_index_find_ptr(&store->objs, ref_obj->handle)) != NULL) {
		/* Invalidate wrefs_head while dtoring, to prevent detach on same wr */
		zend_hash_index_del(&store->objs, ref_obj->handle);

		while (list_entry != NULL) {
			wr_ref_list *next = list_entry->next;
			list_entry->dtor(list_entry->wref_obj, ref_obj);
			efree(list_entry);
			list_entry = next;
		}
	}

	zend_object_handlers *old_handlers;
	if ((old_handlers = zend_hash_index_find_ptr(&store->old_handlers, (ulong)ref_obj->handlers)) != NULL) {
		efree((zend_object_handlers *)ref_obj->handlers);
		ref_obj->handlers = old_handlers;
	}
}
/* }}} */

/**
 * This function is called when a given weak-ref/map starts tracking the 'ref'
 * object.
 */
void wr_store_track(zend_object *wref_obj, wr_ref_dtor dtor, zend_object *ref_obj) /* {{{ */
{
	wr_store *store        = WR_G(store);
	ulong     handlers_key = (ulong)ref_obj->handlers;
	ulong     handle_key   = (ulong)ref_obj->handle;

	if (!store->hash_replaced) {
		store->hash_replaced = 1;
		EG(function_table)->pDestructor = NULL;
		zend_function *weakref_object_hash = zend_hash_str_find_ptr(EG(function_table), "weakref_object_hash", sizeof("weakref_object_hash")-1);
		zend_function *spl_object_hash     = zend_hash_str_find_ptr(EG(function_table), "spl_object_hash",     sizeof("spl_object_hash")-1);

		zend_hash_str_update_ptr(EG(function_table), "spl_object_hash", sizeof("spl_object_hash")-1, weakref_object_hash);
		zend_hash_str_update_ptr(EG(function_table), "weakref_object_hash", sizeof("weakref_object_hash")-1, spl_object_hash);
		EG(function_table)->pDestructor = ZEND_FUNCTION_DTOR;
	}

	if (zend_hash_index_find_ptr(&store->old_handlers, handlers_key) == NULL) {
		const zend_object_handlers *old_handlers = ref_obj->handlers;

		zend_object_handlers *new_handlers = emalloc(sizeof(zend_object_handlers));
		memcpy(new_handlers, old_handlers, sizeof(zend_object_handlers));
		new_handlers->dtor_obj = wr_store_tracked_object_dtor;
		ref_obj->handlers = new_handlers;

		zend_hash_index_update_ptr(&store->old_handlers, (ulong)new_handlers, (void *)old_handlers);
	}

	wr_ref_list *tail = zend_hash_index_find_ptr(&store->objs, handle_key);

	/* We now put the weakref 'wref' in the list of weak references that need
	 * to be invalidated when 'ref' is destroyed */
	wr_ref_list *head = emalloc(sizeof(wr_ref_list));
	head->wref_obj = wref_obj;
	head->dtor     = dtor;
	head->next     = tail;

	zend_hash_index_update_ptr(&store->objs, handle_key, head);
}
/* }}} */

/**
 * This function is called when a given weak-ref/map stops tracking the 'ref'
 * object.
 */
void wr_store_untrack(zend_object *wref_obj, zend_object *ref_obj) /* {{{ */
{
	wr_store      *store           = WR_G(store);

	if (!store) {
		// detach() can be called after the store has already been cleaned up,
		// depending on the shutdown sequence (i.e. in case of a fatal).
		// See tests/weakref_007.phpt
		return;
	} else {
		wr_ref_list   *cur = zend_hash_index_find_ptr(&store->objs, ref_obj->handle);
		wr_ref_list   *prev  = NULL;

		if (!cur) {
			// We are detaching from a wr that is being dtored, skip
			return;
		}

		while (cur && cur->wref_obj != wref_obj) {
			prev = cur;
			cur  = cur->next;
		}

		assert(cur != NULL);

		if (prev) {
			prev->next = cur->next;
		} else {
			zend_hash_index_update_ptr(&store->objs, ref_obj->handle, cur->next);
		}

		efree(cur);
	}
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: fdm=marker
 * vim: noet sw=4 ts=4
 */

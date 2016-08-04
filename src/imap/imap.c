/*
 * imap.c - map for uint64_t integer keys
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 03-08-2016
 *
 */
#include <assert.h>
#include <imap/imap.h>
#include <logger/logger.h>
#include <siri/err.h>
#include <stdio.h>
#include <stdlib.h>

#define IMAP_NODE_SZ 32

static void IMAP_node_free(imap_node_t * node);
static void IMAP_node_free_cb(imap_node_t * node, imap_cb cb, void * data);
static int IMAP_add(imap_node_t * node, uint64_t id, void * data);
static void * IMAP_get(imap_node_t * node, uint64_t id);
static void * IMAP_pop(imap_node_t * node, uint64_t id);
static void IMAP_walk(imap_node_t * node, imap_cb cb, void * data, int * rc);
static void IMAP_walkn(imap_node_t * node, imap_cb cb, void * data, size_t * n);
static void IMAP_2slist(imap_node_t * node, slist_t * slist);
static void IMAP_2slist_ref(imap_node_t * node, slist_t * slist);
static void IMAP_union_ref(imap_node_t * dest, imap_node_t * node);

/*
 * Returns NULL and raises a SIGNAL in case an error has occurred.
 */
imap_t * imap_new(void)
{
    imap_t * imap = (imap_t *) calloc(
            1,
            sizeof(imap_t) + IMAP_NODE_SZ * sizeof(imap_node_t));
    if (imap == NULL)
    {
        ERR_ALLOC
    }
    else
    {
        imap->len = 0;
    }
    return imap;
}

/*
 * Destroy imap. (parsing NULL is NOT allowed)
 */
void imap_free(imap_t * imap)
{
    if (imap->len)
    {
        imap_node_t * nd;

        for (uint8_t i = 0; i < IMAP_NODE_SZ; i++)
        {
            if ((nd = imap->nodes + i)->nodes != NULL)
            {
                IMAP_node_free(nd);
            }
        }
    }
    free(imap);
}

/*
 * Destroy imap using a call-back function. (parsing NULL is NOT allowed)
 */
void imap_free_cb(imap_t * imap, imap_cb cb, void * data)
{
    if (imap->len)
    {
        imap_node_t * nd;

        for (uint8_t i = 0; i < IMAP_NODE_SZ; i++)
        {
            nd = imap->nodes + i;

            if (nd->data != NULL)
            {
                (*cb)(nd->data, data);
            }

            if (nd->nodes != NULL)
            {
                IMAP_node_free_cb(nd, cb, data);
            }
        }
    }
    free(imap);
}

/*
 * Add data by id to the map.
 *
 * Returns 0 when data is overwritten and 1 if a new id/value is set.
 *
 * In case of an error we return -1 and a SIGNAL is raised.
 */
int imap_add(imap_t * imap, uint64_t id, void * data)
{
#ifdef DEBUG
    /* insert NULL is not allowed */
    assert (data != NULL);
#endif

    imap_node_t * nd = imap->nodes + (id % IMAP_NODE_SZ);
    id /= IMAP_NODE_SZ;

    if (!id)
    {
        if (nd->data == NULL)
        {
            imap->len++;
        }

        nd->data = data;

        return 0;
    }

    int rc = IMAP_add(nd, id - 1, data);

    if (rc > 0)
    {
        imap->len++;
    }

    return rc;
}

/*
 * Returns data by a given id, or NULL when not found.
 */
void * imap_get(imap_t * imap, uint64_t id)
{
    imap_node_t * nd = imap->nodes + (id % IMAP_NODE_SZ);
    id /= IMAP_NODE_SZ;

    if (!id)
    {
        return nd->data;
    }

    return (nd->nodes == NULL) ? NULL : IMAP_get(nd, id - 1);
}

/*
 * Remove and return an item by id or return NULL in case the id is not found.
 */
void * imap_pop(imap_t * imap, uint64_t id)
{
    void * data;
    imap_node_t * nd = imap->nodes + (id % IMAP_NODE_SZ);
    id /= IMAP_NODE_SZ;

    if (!id)
    {
        if ((data = nd->data) != NULL)
        {
            nd->data = NULL;
            imap->len--;
        }

        return data;
    }

    data = (nd->nodes == NULL) ? NULL : IMAP_pop(nd, id - 1);
    imap->len -= (data != NULL);

    return data;
}

/*
 * Run the call-back function on all items in the map.
 *
 * All the results are added together and are returned as the result of
 * this function.
 */
int imap_walk(imap_t * imap, imap_cb cb, void * data)
{
    int rc = 0;

    if (imap->len)
    {
        imap_node_t * nd;

        for (uint8_t i = 0; i < IMAP_NODE_SZ; i++)
        {
            nd = imap->nodes + i;

            if (nd->data != NULL)
            {
                rc += (*cb)(nd->data, data);
            }

            if (nd->nodes != NULL)
            {
                IMAP_walk(nd, cb, data, &rc);
            }
        }
    }

    return rc;
}

/*
 * Recursive function, call-back function will be called on each item.
 *
 * Walking stops either when the call-back is called on each value or
 * when 'n' is zero. 'n' will be decremented by the result of each call-back.
 */
void imap_walkn(imap_t * imap, size_t * n, imap_cb cb, void * data)
{
    if (imap->len)
    {
        imap_node_t * nd;

        for (uint8_t i = 0; *n && i < IMAP_NODE_SZ; i++)
        {
            nd = imap->nodes + i;

            if (nd->data != NULL && !(*n -= (*cb)(nd->data, data)))
            {
                return;
            }

            if (nd->nodes != NULL)
            {
                IMAP_walkn(nd, cb, data, n);
            }
        }
    }
}

/*
 * Returns NULL and raises a SIGNAL in case an error has occurred.
 */
slist_t * imap_2slist(imap_t * imap)
{
    slist_t * slist = slist_new(imap->len);

    if (slist == NULL)
    {
        ERR_ALLOC
    }
    else if (imap->len)
    {
        imap_node_t * nd;

        for (uint8_t i = 0; i < IMAP_NODE_SZ; i++)
        {
            nd = imap->nodes + i;

            if (nd->data != NULL)
            {
                slist_append(slist, nd->data);
            }

            if (nd->nodes != NULL)
            {
                IMAP_2slist(nd, slist);
            }
        }
    }
    return slist;
}

/*
 * Use this function to create a s-list copy and update the ref count
 * for each object. We expect each object to have object->ref (uint16_t) on
 * top of the object definition.
 *
 * There is no function to handle the decrement for the ref count since they
 * are different for each object. Best is to handle the decrement while looping
 * over the returned list.
 *
 * Returns NULL and raises a SIGNAL in case an error has occurred.
 */
slist_t * imap_2slist_ref(imap_t * imap)
{
    slist_t * slist = slist_new(imap->len);

    if (slist == NULL)
    {
        ERR_ALLOC
    }
    else if (imap->len)
    {
        imap_node_t * nd;

        for (uint8_t i = 0; i < IMAP_NODE_SZ; i++)
        {
            nd = imap->nodes + i;

            if (nd->data != NULL)
            {
                slist_append(slist, nd->data);
                slist_object_incref(nd->data);
            }

            if (nd->nodes != NULL)
            {
                IMAP_2slist_ref(nd, slist);
            }
        }
    }
    return slist;
}

void imap_union_ref(imap_t * dest, imap_t ** imap)
{
    if ((*imap)->len)
    {
        imap_node_t * dest_nd;
        imap_node_t * imap_nd;

        for (uint8_t i = 0; i < IMAP_NODE_SZ; i++)
        {
            dest_nd = dest->nodes + i;
            imap_nd = (*imap)->nodes + i;

            if (imap_nd->data != NULL)
            {
                if (dest_nd->data != NULL)
                {
#ifdef DEBUG
                    /* this must be the same object */
                    assert (imap_nd->data == dest_nd->data);
#endif
                    /* we are sure there is a ref left */
                    slist_object_decref(imap_nd->data);
                }
                else
                {
                    dest_nd->data = imap_nd->data;
                    dest->len++;
                }
            }

            if (imap_nd->nodes != NULL)
            {
                if (dest_nd->nodes != NULL)
                {
                    size_t tmp = dest_nd->size;
                    IMAP_union_ref(dest_nd, imap_nd);
                    dest->len += dest_nd->size - tmp;
                }
                else
                {
                    dest_nd->nodes = imap_nd->nodes;
                    dest_nd->size = imap_nd->size;
                    imap_nd->nodes = NULL;
                    dest->len += dest_nd->size;
                }
            }
        }
    }

    /* cleanup source imap */
    free(*imap);
    *imap = NULL;
}

static void IMAP_node_free(imap_node_t * node)
{
    imap_node_t * nd;

    for (uint8_t i = 0; i < IMAP_NODE_SZ; i++)
    {
        if ((nd = node->nodes + i)->nodes != NULL)
        {
            IMAP_node_free(nd);
        }
    }

    free(node->nodes);
}

static void IMAP_node_free_cb(imap_node_t * node, imap_cb cb, void * data)
{
    imap_node_t * nd;

    for (uint8_t i = 0; i < IMAP_NODE_SZ; i++)
    {
        nd = node->nodes + i;

        if (nd->data != NULL)
        {
            (*cb)(nd->data, data);
        }

        if (nd->nodes != NULL)
        {
            IMAP_node_free_cb(nd, cb, data);
        }
    }
    free(node->nodes);
}

/*
 * Add data by id to the map.
 *
 * Returns 0 when data is overwritten and 1 if a new id/value is set.
 *
 * In case of an error we return -1 and a SIGNAL is raised.
 */
static int IMAP_add(imap_node_t * node, uint64_t id, void * data)
{
    if (!node->size)
    {
        node->nodes = (imap_node_t *) calloc(
                IMAP_NODE_SZ,
                sizeof(imap_node_t));

        if (node->nodes == NULL)
        {
            ERR_ALLOC
            return -1;
        }
    }

    int rc;
    imap_node_t * nd = node->nodes + (id % IMAP_NODE_SZ);
    id /= IMAP_NODE_SZ;

    if (!id)
    {
        rc = (nd->data == NULL);

        nd->data = data;
        node->size += rc;

        return rc;
    }

    rc = IMAP_add(nd, id - 1, data);

    if (rc > 0)
    {
        node->size++;
    }

    return rc;
}

static void * IMAP_get(imap_node_t * node, uint64_t id)
{
    imap_node_t * nd = node->nodes + (id % IMAP_NODE_SZ);
    id /= IMAP_NODE_SZ;

    if (!id)
    {
        return nd->data;
    }

    return (nd->nodes == NULL) ? NULL : IMAP_get(nd, id - 1);
}

static void * IMAP_pop(imap_node_t * node, uint64_t id)
{
    void * data;
    imap_node_t * nd = node->nodes + (id % IMAP_NODE_SZ);
    id /= IMAP_NODE_SZ;

    if (!id)
    {
        if ((data = nd->data) != NULL)
        {
            if (--node->size)
            {
                nd->data = NULL;
            }
            else
            {
                free(node->nodes);
                node->nodes = NULL;
            }
        }

        return data;
    }

    data = (nd->nodes == NULL) ? NULL : IMAP_pop(nd, id - 1);

    if (data != NULL && !--node->size)
    {
        free(node->nodes);
        node->nodes = NULL;
    }

    return data;
}

static void IMAP_walk(imap_node_t * node, imap_cb cb, void * data, int * rc)
{
    imap_node_t * nd;

    for (uint8_t i = 0; i < IMAP_NODE_SZ; i++)
    {
        nd = node->nodes + i;

        if (nd->data != NULL)
        {
            *rc += (*cb)(nd->data, data);
        }

        if (nd->nodes != NULL)
        {
            IMAP_walk(nd, cb, data, rc);
        }
    }
}

static void IMAP_walkn(imap_node_t * node, imap_cb cb, void * data, size_t * n)
{
    imap_node_t * nd;

    for (uint8_t i = 0; *n && i < IMAP_NODE_SZ; i++)
    {
        nd = node->nodes + i;

        if (nd->data != NULL && !(*n -= (*cb)(nd->data, data)))
        {
             return;
        }

        if (nd->nodes != NULL)
        {
            IMAP_walkn(nd, cb, data, n);
        }
    }
}

static void IMAP_2slist(imap_node_t * node, slist_t * slist)
{
    imap_node_t * nd;

    for (uint8_t i = 0; i < IMAP_NODE_SZ; i++)
    {
        nd = node->nodes + i;

        if (nd->data != NULL)
        {
            slist_append(slist, nd->data);
        }

        if (nd->nodes != NULL)
        {
            IMAP_2slist(nd, slist);
        }
    }
}

static void IMAP_2slist_ref(imap_node_t * node, slist_t * slist)
{
    imap_node_t * nd;

    for (uint8_t i = 0; i < IMAP_NODE_SZ; i++)
    {
        nd = node->nodes + i;

        if (nd->data != NULL)
        {
            slist_append(slist, nd->data);
            slist_object_incref(nd->data);
        }

        if (nd->nodes != NULL)
        {
            IMAP_2slist_ref(nd, slist);
        }
    }
}

static void IMAP_union_ref(imap_node_t * dest, imap_node_t * node)
{
    imap_node_t * dest_nd;
    imap_node_t * node_nd;

    for (uint8_t i = 0; i < IMAP_NODE_SZ; i++)
    {
        dest_nd = dest->nodes + i;
        node_nd = node->nodes + i;

        if (node_nd->data != NULL)
        {
            if (dest_nd->data != NULL)
            {
#ifdef DEBUG
                /* this must be the same object */
                assert (node_nd->data == dest_nd->data);
#endif
                /* we are sure there is a ref left */
                slist_object_decref(node_nd->data);
            }
            else
            {
                dest_nd->data = node_nd->data;
                dest->size++;
            }
        }

        if (node_nd->nodes != NULL)
        {
            if (dest_nd->nodes != NULL)
            {
                size_t tmp = dest_nd->size;
                IMAP_union_ref(dest_nd, node_nd);
                dest->size += dest_nd->size - tmp;
            }
            else
            {
                dest_nd->nodes = node_nd->nodes;
                dest_nd->size = node_nd->size;
                node_nd->nodes = NULL;
                dest->size += dest_nd->size;
            }
        }
    }
    free(node->nodes);
}

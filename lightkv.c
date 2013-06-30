#include "lightkv.h"
#include <sys/mman.h> /* mmap inside */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h> /* file open modes and stuff */
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>


void print_record(record *rec) {
    char key[rec->extlen+1];
    char val[rec->len - rec->extlen - RECORD_HEADER_SIZE];
    strncpy(key, (char *) rec + RECORD_HEADER_SIZE, rec->extlen);
    key[rec->extlen] = '\0';
    strncpy(val, (char *) rec + RECORD_HEADER_SIZE + rec->extlen, sizeof(val));

    printf("Record:\n");
    printf("Type:%d\nSize:%d\nKey:%s\nValue:%s\n",
            rec->type,
            rec->len,
            key,
            val);
}

static uint32_t roundsize(uint32_t v) {
    uint32_t n;
    // TODO: Add bound checks
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

static int get_sizeslot(uint32_t v) {
    int n = 0;
    while( v>>=1 ) n++;
    n = n - FIRST_SIZECLASS;
    if (n < 0) {
        n = 0;
    } else if (n > FIRST_SIZECLASS + MAX_SIZES - 1) {
        n = MAX_SIZES - 1;
    }

    return n;
}

static uint32_t get_slotsize(int slot) {
    slot += FIRST_SIZECLASS;
    return pow(2, slot);
}

static freeloc *freeloc_new(loc l) {
    freeloc *f = malloc(sizeof(freeloc));
    f->l = l;
    f->next = NULL;
    f->prev = NULL;
}

static freeloc *freelist_add(freeloc *head, freeloc *n) {
    if (head) {
        head->prev = n;
        n->next = head;
    }

    return n;
}

static freeloc *freelist_get(freeloc *head, uint32_t size) {
    freeloc *f = NULL;
    uint32_t diff;

    while (head) {
        if (f == NULL) {
            f = head;
            diff = get_slotsize(head->l.l.sclass) - size;
        } else {
            uint32_t t;
            t = get_slotsize(head->l.l.sclass) - size;
            if (t < diff) {
                diff = t;
                f = head;
            }
        }
        head = head->next;
    }

    return f;
}

static freeloc *freelist_remove(freeloc *head, freeloc *f) {
    if (f) {
        if (f == head) {
            if (f->next) {
                f->next->prev = NULL;
            }
            head = f;
        } else {
            freeloc *tmp;
            tmp = f->next;
            tmp->prev = f->prev;
            f->prev->next = tmp;
        }

        free(f);
    }

    return head;
}

static int alloc_file(char *filepath, size_t size) {
    int fd;
    int rv;

    fd = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return fd;
    }

    // Fill the file with zeros
    lseek (fd, size-2, SEEK_SET);
    if ((rv = write(fd, "",1)) < 0) {
        return rv;
    }

    return 0;
}

static int map_file(void **map, char *filepath) {
    int fd;
    struct stat st;
    fd = open(filepath, O_RDWR);
    if (fd < 0) {
        return fd;
    }

    errno = 0;
    if (fstat(fd, &st) < 0) {
        return errno;
    }

    errno = 0;
    *map = mmap(0, st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (errno < 0) {
        return errno;
    }

    close(fd);
    return 0;
}


static loc create_nextloc(lightkv *kv, uint32_t size) {
    loc next = kv->end_loc;
    next.l.sclass = 0;
    uint64_t off = kv->end_loc.l.offset + size + 2;
    if (off > MAX_FILESIZE) {
        // FIXME: in prealloc, its different.

        next.l.num++;
        next.l.offset = 1;
        kv->nfiles++;

        char *f = getfilepath(kv->basepath, next.l.num);
        alloc_file(f, MAX_FILESIZE);

        if (map_file(&kv->filemaps[next.l.num], f) < 0) {
            assert(false);
        }
        free(f);
    } else {
       next.l.offset++;
    }

    return next;
}

static int write_record(lightkv *kv, loc l, record *rec) {
    char *dst;

    dst = kv->filemaps[l.l.num] + l.l.offset;
    memcpy(dst, rec, rec->len);

    printf("Wrote: %s\n", rec);
    printf("Wrote record at file, %d:%d\n", l.l.num, l.l.offset);
    printf("type: %d, extlen: %d, size: %d\n", rec->type, rec->extlen, rec->len);
    return rec->len;
}

static int read_record(lightkv *kv, loc l, record **rec) {
    char *src;
    size_t slotsize = get_slotsize(l.l.sclass);
    *rec = malloc(slotsize);
    src = kv->filemaps[l.l.num] + l.l.offset;
    memcpy(*rec, src, slotsize);

    printf("Read the following record\n");
    print_record(*rec);

    return 0;
}

int lightkv_init(lightkv **kv, char *base, bool prealloc) {
    // TODO: Add sanity checks

    *kv = malloc(sizeof(lightkv));
    assert(*kv > 0);

    (*kv)->prealloc = prealloc;
    (*kv)->basepath = base;
    (*kv)->filemaps[0] = NULL;
    (*kv)->nfiles = 1;

    char *f = getfilepath(base, 0);
    alloc_file(f, MAX_FILESIZE);

    if (map_file(&(*kv)->filemaps[0], f) < 0) {
        assert(false);
    }
    loc x;
    x.l.num = 0;
    x.l.offset = 0;

    (*kv)->end_loc = x;
    x.l.offset = 1;
    (*kv)->start_loc = x;

    free(f);

    return 0;
}

uint64_t lightkv_insert(lightkv *kv, char *key, char *val, uint32_t len) {
    size_t size, keylen;
    freeloc *f;
    loc diskloc;
    record *rec;
    keylen = strlen(key);
    size = RECORD_HEADER_SIZE + keylen + len;

    rec = malloc(size);
    rec->type = RECORD_VAL ;
    rec->len = size;
    rec->extlen = keylen;

    memcpy((char *) rec + RECORD_HEADER_SIZE, key, keylen);
    memcpy((char *) rec + RECORD_HEADER_SIZE + keylen, val, len);

    printf("key:%s val:%s, rec:%s\n", key, val, rec);
    print_buf(rec, size);
    printf("len:%d, extlen=%d\n", size, keylen);

    int rsize = roundsize(size);
    int slot = get_sizeslot(rsize);
    f = freelist_get(kv->freelist[slot], size);
    if (f) {
        diskloc = f->l;
        kv->freelist[slot] = freelist_remove(kv->freelist[slot], f);
    } else {
        diskloc = create_nextloc(kv, rsize);
    }
    diskloc.l.sclass = slot;
    int l = write_record(kv, diskloc, rec);
    kv->end_loc.l.num = diskloc.l.num;
    kv->end_loc.l.offset = diskloc.l.offset + l - 1;

    return diskloc.val;
}

char *get_key(record *r) {
    int l = r->extlen;
    char *buf = malloc(l+1);
    memcpy(buf, (char *) r + RECORD_HEADER_SIZE, l);
    buf[l] = '\0';
    return buf;
}

size_t get_val(record *r, char **v) {
    int l = r->len - RECORD_HEADER_SIZE - r->extlen;
    char *buf = malloc(l);
    memcpy(buf, (char *) r + RECORD_HEADER_SIZE + r->extlen, l);
    *v = buf;
    return l;
}

bool lightkv_get(lightkv *kv, uint64_t recid, char **key, char **val, uint32_t *len) {
    bool rv;
    record *rec;
    loc l = (loc) recid;
    read_record(kv, l, &rec);
    rv = rec->type == RECORD_VAL ? true: false;
    if (rv == false) {
        return false;
    }

    *key = get_key(rec);
    *len = get_val(rec, val);

    return true;
}

bool lightkv_delete(lightkv *kv, uint64_t recid) {
    // TODO: basic sanity
    loc l = (loc) recid;
    size_t slotsize = get_slotsize(l.l.sclass);
    record *rec = malloc(slotsize);
    rec->type = RECORD_DEL;
    rec->len = 0;
    rec->extlen = 0;
    write_record(kv, l, rec);
    freeloc *f = freeloc_new(l);
    kv->freelist[l.l.sclass] = freelist_add(kv->freelist[l.l.sclass], f);
    free(rec);
    return true;
}

uint64_t lightkv_update(lightkv *kv, uint64_t recid, char *key, char *val, uint32_t len) {

}

main() {
    lightkv *kv;
    lightkv_init(&kv, "/tmp/", true);
    uint64_t rid;

    rid = lightkv_insert(kv, "test_key1", "hello", 5);
    printf("record is %llu\n", rid);

    rid = lightkv_insert(kv, "test_key2", "helli", 5);
    printf("record is %llu\n", rid);
    lightkv_delete(kv, rid);

    rid = lightkv_insert(kv, "test_key3333", "hell3", 5);
    printf("record is %llu\n", rid);
    char *k,*v;
    int l;
    lightkv_get(kv, 4295098368, &k, &v, &l);

    printf("got record, key:%s, val:%s\n", k, v);
}
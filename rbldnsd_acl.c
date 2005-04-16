/* $Id$
 * ACL (Access Control Lists) implementation for rbldnsd.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <netinet/in.h>
#include "rbldnsd.h"

struct dsdata {
  struct ip4trie trie;
  const char *def_rr;
  const char *def_action;
};

definedstype(acl, 0, "Access Control List dataset");

/* special cases for pseudo-RRs */
static const struct {
  const char *name;
  const char *rr;
} keywords[] = {
  /* ignore (don't answer) queries from this IP */
#define RR_BLACKHOLE	((const char *)1)
 { "blackhole", RR_BLACKHOLE },
 { "ignore", RR_BLACKHOLE },
 /* refuse *data* queries from this IP (but not metadata) */
#define RR_REFUSE	((const char *)2)
 { "refuse", RR_REFUSE },
 /* pretend the zone is completely empty */
#define RR_EMPTY	((const char *)3)
 { "empty", RR_EMPTY },
#define RR_LAST RR_EMPTY
};

static void ds_acl_reset(struct dsdata *dsd, int UNUSED unused_freeall) {
  memset(dsd, 0, sizeof(*dsd));
}

static void ds_acl_start(struct dataset *ds) {
  ds->ds_dsd->def_rr = ds->ds_dsd->def_action = def_rr;
}

static const char *keyword(const char *s) {
  const char *k, *p;
  unsigned i;
  if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')))
    return NULL;
  for (i = 0; i < sizeof(keywords)/sizeof(keywords[0]); ++i)
    for (k = keywords[i].name, p = s;;)
      if ((*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p) != *k++)
        break;
      else if (!*++p || *p == ':' || ISSPACE(*p) || ISCOMMENT(*p))
        return keywords[i].rr;
  return NULL;
}

static int
ds_acl_parse_val(char *s, const char **rr_p, struct dsdata *dsd,
                 struct dsctx *dsc) {
  int r;
  if (*s == '=') {
    if ((*rr_p = keyword(s+1)))
      return 0;
    dswarn(dsc, "invalid keyword");
    return -1;
  }
  if (*s == ':' && (*rr_p = keyword(s+1)))
    return 0;
  r = parse_a_txt(s, rr_p, dsd->def_rr, dsc);
  return r ? r : -1;
}

static int
ds_acl_line(struct dataset *ds, char *s, struct dsctx *dsc) {
  struct dsdata *dsd = ds->ds_dsd;
  ip4addr_t a;
  int bits;
  const char *rr;
  int rrl;
  struct ip4trie_node *node;

  if (*s == ':' || *s == '=') {
    if ((rrl = ds_acl_parse_val(s, &rr, dsd, dsc)) < 0)
      return 1;
    else if (!rrl)
      dsd->def_action = rr;
    else if (!(rr = mp_dmemdup(ds->ds_mp, rr, rrl)))
      return 0;
    dsd->def_rr = dsd->def_action = rr;
    return 1;
  }

  if ((bits = ip4cidr(s, &a, &s)) <= 0 ||
     (*s && !ISSPACE(*s) && !ISCOMMENT(*s) && *s != ':')) {
    dswarn(dsc, "invalid address");
    return 1;
  }
  if (accept_in_cidr)
    a &= ip4mask(bits);
  else if (a & ~ip4mask(bits)) {
    dswarn(dsc, "invalid range (non-zero host part)");
    return 1;
  }
  if (dsc->dsc_ip4maxrange && dsc->dsc_ip4maxrange <= ~ip4mask(bits)) {
    dswarn(dsc, "too large range (%u) ignored (%u max)",
           ~ip4mask(bits) + 1, dsc->dsc_ip4maxrange);
    return 1;
  }

  SKIPSPACE(s);
  if (!*s || ISCOMMENT(*s))
    rr = dsd->def_action;
  else if ((rrl = ds_acl_parse_val(s, &rr, dsd, dsc)) < 0)
    return 1;
  else if (rrl && !(rr = mp_dmemdup(ds->ds_mp, rr, rrl)))
    return 0;

  node = ip4trie_addnode(&dsd->trie, a, bits, ds->ds_mp);
  if (!node)
     return 0;

  if (node->ip4t_data) {
    dswarn(dsc, "duplicated entry for %s/%d", ip4atos(a), bits);
    return 1;
  }
  node->ip4t_data = rr;
  ++dsd->trie.ip4t_nents;

  return 1;
}

static void ds_acl_finish(struct dataset *ds, struct dsctx *dsc) {
  struct dsdata *dsd = ds->ds_dsd;
  dsloaded(dsc, "ent=%u nodes=%u mem=%u",
           dsd->trie.ip4t_nents, dsd->trie.ip4t_nnodes,
           dsd->trie.ip4t_nnodes * sizeof(struct ip4trie_node));
}

int
ds_acl_query(const struct dataset *ds, const struct dnsqinfo *qi,
             struct dnspacket UNUSED *pkt) {
  const struct sockaddr_in *sin = (const struct sockaddr_in *)qi;
  const char *rr;
  if (sin->sin_family != AF_INET) return 0;
  rr = ip4trie_lookup(&ds->ds_dsd->trie, ntohl(sin->sin_addr.s_addr));
#if 0
  switch(rr) {
  case NULL: return 0;
  case RR_BLACKHOLE: return 1;
  case RR_REFUSE: return 2;
  case RR_EMPTY: return 3;
  default: return 4;
  }
#endif
  return 0;
}

#ifndef NO_MASTER_DUMP
static void
ds_acl_dump(const struct dataset UNUSED *ds,
            const unsigned char UNUSED *unused_odn,
            FILE UNUSED *f) {
}
#endif
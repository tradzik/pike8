/*
|| This file is part of Pike. For copyright information see COPYRIGHT.
|| Pike is distributed under GPL, LGPL and MPL. See the file COPYING
|| for more information.
*/

#include "global.h"
#include "stralloc.h"
#include "pike_macros.h"
#include "dynamic_buffer.h"
#include "pike_macros.h"
#include "pike_memory.h"
#include "pike_error.h"
#include "gc.h"
#include "stuff.h"
#include "bignum.h"
#include "interpret.h"
#include "operators.h"
#include "pike_float.h"
#include "block_allocator.h"

#include <errno.h>
#include <float.h>
#include <ctype.h>
#include <math.h>

#define SET_HSIZE(X) htable_mask=(htable_size=(X))-1
#define HMODULO(X) ((X) & (htable_mask))

static unsigned INT32 htable_mask;

#if (SIZEOF_LONG == 4) && defined(_LP64)
/* Kludge for gcc and the system header files not using the same model... */
#undef LONG_MIN
#undef LONG_MAX
#undef ULONG_MAX
#define LONG_MIN	INT_MIN
#define LONG_MAX	INT_MAX
#define ULONG_MAX	UINT_MAX
#endif

#define BEGIN_HASH_SIZE 1024

static unsigned int hash_prefix_len=64;
static unsigned int need_more_hash_prefix_depth=0;

static unsigned int need_new_hashkey_depth=0;
static size_t hashkey = 0;

static unsigned INT32 htable_size=0;
static struct pike_string **base_table=0;
static unsigned INT32 num_strings=0;
PMOD_EXPORT struct pike_string *empty_pike_string = 0;

/*** Main string hash function ***/

#define StrHash(s,len) low_do_hash(s,len,0)
#define low_do_hash(STR,LEN,SHIFT) low_hashmem( (STR), (LEN)<<(SHIFT), hash_prefix_len<<(SHIFT), hashkey )
#define do_hash(STR) low_do_hash(STR->str,STR->len,STR->size_shift)

/* Returns true if str could contain n. */
PMOD_EXPORT int string_range_contains( struct pike_string *str, int n )
{
  INT32 min, max;
  check_string_range( str, 1, &min, &max );
  if( n >= min && n <= max )
    return 1;
  return 0;
}

/* Returns true if str2 could be in str1. */
PMOD_EXPORT int string_range_contains_string( struct pike_string *str1,
                                              struct pike_string *str2 )
{
  INT32 max1, min1;
  INT32 max2, min2;
  if( !str2->len ) return 1; /* Empty string is part of every string */
  check_string_range( str1, 1, &min1, &max1 );
  check_string_range( str2, 1, &min2, &max2 );
  if( (min2 < min1) || (max2 > max1) )
  {
    if( (str1->flags & STRING_CONTENT_CHECKED) ==
        (str2->flags & STRING_CONTENT_CHECKED) )
      return 0;
    /* fallback to simple size-shift check.  */
    return str1->size_shift >= str2->size_shift;
  }
  return 1;
}

PMOD_EXPORT void check_string_range( struct pike_string *str,
                                     int loose,
                                     INT32 *min, INT32 *max )
{
  INT32 s_min = MAX_INT32;
  INT32 s_max = MIN_INT32;
  ptrdiff_t i;

  if( loose || ((str->flags & STRING_CONTENT_CHECKED ) && (!str->size_shift || !max)) )
  {
    if( str->flags & STRING_CONTENT_CHECKED )
    {
      switch (str->size_shift) {
      case eightbit:
        s_min = str->min;
        s_max = str->max;
        break;
      case sixteenbit:
        s_min = str->min;
        s_max = str->max;
        s_min *= 256;
        s_max *= 256;
        s_max += 255;
        break;
      case thirtytwobit: {
        unsigned INT32 tmp;

        tmp = str->min;
        tmp *= (1 << 24);
        s_min = tmp;

        tmp = str->max;
        tmp *= (1 << 24);
        tmp += (1 << 24) - 1;
        s_max = tmp;
        break;
      }
      }
    }
    else
    {
      switch( str->size_shift )
      {
        case 2: s_min = MIN_INT32; s_max=MAX_INT32; break;
        case 1: s_min = 0; s_max = 65535; break;
        case 0: s_min = 0; s_max = 255; break;
      }
    }
  }
  else
  {
    str->flags |= STRING_CONTENT_CHECKED;

    switch( str->size_shift )
    {
      case 0:
       {
         p_wchar0 *p = (p_wchar0*)str->str;
         int upper = 0, lower = 0;
         for( i=0; i<str->len; i++,p++ )
         {
           /* For 7-bit strings it's easy to check for
            * lower/uppercase, so do that here as well.
            */
           if( *p >= 'A' && *p <= 'Z') upper++;
           if( *p >= 'a' && *p <= 'z') lower++;

           if( *p > s_max ) s_max = *p;
           if( *p < s_min ) s_min = *p;
         }

         if( s_max < 128 )
         {
           if( !lower )
             str->flags |= STRING_IS_UPPERCASE;
           if( !upper )
             str->flags |= STRING_IS_LOWERCASE;
         }
       }
       str->min = s_min;
       str->max = s_max;
       break;

      case 1:
       {
         p_wchar1 *p = (p_wchar1*)str->str;
         for( i=0; i<str->len; i++,p++ )
         {
           if( *p > s_max ) s_max = *p;
           if( *p < s_min ) s_min = *p;
         }
       }
       str->min = s_min / 256;
       str->max = s_max / 256;
       break;

      case 2:
       {
         p_wchar2 *p = (p_wchar2*)str->str;
         for( i=0; i<str->len; i++,p++ )
         {
           if( *p > s_max ) s_max = *p;
           if( *p < s_min ) s_min = *p;
         }
       }
       str->min = (unsigned INT32)s_min / (1 << 24);
       str->max = (unsigned INT32)s_max / (1 << 24);
       break;
    }
  }
  if( min ) *min = s_min;
  if( max ) *max = s_max;
}

static INLINE int find_magnitude1(const p_wchar1 *s, ptrdiff_t len)
{
  const p_wchar1 *e=s+len;
  while(s<e)
    if(*s++>=256)
      return 1;
  return 0;
}

static INLINE int find_magnitude2(const p_wchar2 *s, ptrdiff_t len)
{
  const p_wchar2 *e=s+len;
  while(s<e)
  {
    if((unsigned INT32)*s>=256)
    {
      do
      {
	if((unsigned INT32)*s++>=65536)
	  return 2;
      }while(s<e);
      return 1;
    }
    s++;
  }
  return 0;
}

static INLINE unsigned min_magnitude(const unsigned c)
{
  return LIKELY(c<256) ? 0 : LIKELY(c<65536) ? 1 : 2;
}

void low_set_index(struct pike_string *s, ptrdiff_t pos, int value)
{
#ifdef PIKE_DEBUG
  if(pos > s->len || pos<0) {
    if (s->len) {
      Pike_fatal("String index %"PRINTPTRDIFFT"d is out of "
		 "range 0..%"PRINTPTRDIFFT"d.\n",
		 pos, s->len-1);
    } else {
      Pike_fatal("Attempt to index the empty string with %"PRINTPTRDIFFT"d.\n",
		 pos);
    }
  }

  if(pos == s->len && value)
    Pike_fatal("string zero termination foul!\n");
#endif
  s->flags |= STRING_NOT_HASHED;

  if(!s->size_shift)
    STR0(s)[pos]=value;
  else if(s->size_shift == 1)
    STR1(s)[pos]=value;
  else 
    STR2(s)[pos]=value;
}

#ifdef PIKE_DEBUG
PMOD_EXPORT struct pike_string *debug_check_size_shift(const struct pike_string *a,
						       int shift)
{
  if(a->size_shift != shift)
    Pike_fatal("Wrong STRX macro used!\n");
  return (struct pike_string*)a;
}
#endif

#define CONVERT(FROM,TO)						\
  void PIKE_CONCAT4(convert_,FROM,_to_,TO) (PIKE_CONCAT(p_wchar,TO) *to, \
					    const PIKE_CONCAT(p_wchar,FROM) *from, \
					    ptrdiff_t len)		\
  {									\
    while(--len>=0) *(to++)= (PIKE_CONCAT (p_wchar, TO)) *(from++);	\
  }									\
  INT32 PIKE_CONCAT4(compare_,FROM,_to_,TO) (const PIKE_CONCAT(p_wchar,TO) *to,	\
					     const PIKE_CONCAT(p_wchar,FROM) *from, \
					     ptrdiff_t len)		\
  {									\
    int tmp;								\
    while(--len>=0)							\
      if((tmp=*(to++)-*(from++)))					\
	return tmp;							\
    return 0;								\
  }

CONVERT(0,1)
CONVERT(0,2)
CONVERT(1,0)
CONVERT(1,2)
CONVERT(2,0)
CONVERT(2,1)

#define TWO_SIZES(X,Y) (((X)<<2)+(Y))

void generic_memcpy(PCHARP to,
                    const PCHARP from,
                    ptrdiff_t len)
{
#ifdef PIKE_DEBUG
  if(len<0)
    Pike_fatal("Cannot copy %ld bytes!\n",
	  DO_NOT_WARN((long)len));
#endif

  switch(TWO_SIZES(from.shift,to.shift))
  {
    case TWO_SIZES(0,0):
      convert_0_to_0((p_wchar0 *)to.ptr,(p_wchar0 *)from.ptr,len);
      break;
    case TWO_SIZES(0,1):
      convert_0_to_1((p_wchar1 *)to.ptr,(p_wchar0 *)from.ptr,len);
      break;
    case TWO_SIZES(0,2):
      convert_0_to_2((p_wchar2 *)to.ptr,(p_wchar0 *)from.ptr,len);
      break;

    case TWO_SIZES(1,0):
      convert_1_to_0((p_wchar0 *)to.ptr,(p_wchar1 *)from.ptr,len);
      break;
    case TWO_SIZES(1,1):
      convert_1_to_1((p_wchar1 *)to.ptr,(p_wchar1 *)from.ptr,len);
      break;
    case TWO_SIZES(1,2):
      convert_1_to_2((p_wchar2 *)to.ptr,(p_wchar1 *)from.ptr,len);
      break;

    case TWO_SIZES(2,0):
      convert_2_to_0((p_wchar0 *)to.ptr,(p_wchar2 *)from.ptr,len);
      break;
    case TWO_SIZES(2,1):
      convert_2_to_1((p_wchar1 *)to.ptr,(p_wchar2 *)from.ptr,len);
      break;
    case TWO_SIZES(2,2):
      convert_2_to_2((p_wchar2 *)to.ptr,(p_wchar2 *)from.ptr,len);
      break;
  }
}

PMOD_EXPORT void pike_string_cpy(PCHARP to, const struct pike_string *from)
{
  generic_memcpy(to,MKPCHARP_STR(from),from->len);
}


#ifdef PIKE_DEBUG
#ifdef DEBUG_MALLOC
#define DM(X) X
#else
#define DM(X)
#endif

PMOD_EXPORT p_wchar2 index_shared_string(const struct pike_string *s,
                                         ptrdiff_t pos)
{
  if(pos > s->len || pos<0) {
    if (s->len) {
      Pike_fatal("String index %"PRINTPTRDIFFT"d is out of "
		 "range 0..%"PRINTPTRDIFFT"d.\n",
		 pos, s->len-1);
    } else {
      Pike_fatal("Attempt to index the empty string with %"PRINTPTRDIFFT"d.\n",
		 pos);
    }
  }
  return generic_extract(s->str,s->size_shift,pos);
}

PMOD_EXPORT p_wchar2 generic_extract (const void *str, int size, ptrdiff_t pos)
{
  switch(size)
  {
    case 0: return ((p_wchar0 *)str)[pos];
    case 1: return ((p_wchar1 *)str)[pos];
    case 2: return ((p_wchar2 *)str)[pos];
  }
  Pike_fatal("Unsupported string shift: %d\n", size);
  return 0;
}

static void locate_problem(int (*isproblem)(const struct pike_string *))
{
  unsigned INT32 e;
  struct pike_string *s;
  DM(struct memhdr *yes=alloc_memhdr());
  DM(struct memhdr *no=alloc_memhdr());

  for(e=0;e<htable_size;e++)
  {
    for(s=base_table[e];s;s=s->next)
    {
      if(isproblem(s))
      {
	fprintf(stderr,"***Guilty string:\n");
	debug_dump_pike_string(s, 70);
	DM(add_marks_to_memhdr(yes,s));
      }else{
	DM(add_marks_to_memhdr(no,s));
      }
    }
  }

  DM(fprintf(stderr,"Plausible problem location(s):\n"));
  DM(dump_memhdr_locations(yes,0,0));

  DM(fprintf(stderr,"More Plausible problem location(s):\n"));
  DM(dump_memhdr_locations(yes,no,0));
}

static int bad_pointer(const struct pike_string *s)
{
  return (((ptrdiff_t)s)&(sizeof(struct pike_string *)-1));
}

static int has_zero_refs(const struct pike_string *s)
{
  return s->refs<=0;
}
static int wrong_hash(const struct pike_string *s)
{
  return s->hval != do_hash(s);
}
static int improper_zero_termination(const struct pike_string *s)
{
  return index_shared_string(s,s->len);
}
#else
#define locate_problem(X)
#endif /* PIKE_DEBUG */

/* Find a string in the shared string table.
 * This assumes that the string is minimized!!!! 
 */
static struct pike_string *internal_findstring(const char *s,
                                               ptrdiff_t len,
                                               int size_shift,
                                               size_t hval)
{
  struct pike_string *curr;
//,**prev, **base;
  unsigned int depth=0;
  unsigned int prefix_depth=0;

  size_t h;
  h=HMODULO(hval);
  for(curr = base_table[h]; curr; curr = curr->next)
  {
#ifdef PIKE_DEBUG
    if(curr->refs<1)
    {
      debug_dump_pike_string(curr, 70);
      locate_problem(has_zero_refs);
      Pike_fatal("String with no references.\n");
    }
#endif
    debug_malloc_touch(curr);

    if ( len == curr->len &&
        size_shift == curr->size_shift &&
         hval == curr->hval &&
        ( curr->str == s ||
          !memcmp(curr->str, s,len<<size_shift))) /* found it */
    {
      /* *prev = curr->next; */
      /* curr->next = *base; */
      /* *base = curr; */
      return curr;		/* pointer to string */
    }
    depth++;
    if (curr->len > (ptrdiff_t)hash_prefix_len)
      prefix_depth++;
  }
  if (depth > need_new_hashkey_depth) {
    /* Keep track of whether the hashtable is getting unbalanced. */
    need_new_hashkey_depth = depth;
  }
  /* These heuruistics might require tuning! /Hubbe */
  if (prefix_depth > need_more_hash_prefix_depth)
  {
#if 0
    fprintf(stderr,
	    "prefix_depth=%d  num_strings=%d need_more_hash_prefix_depth=%d\n"
	    "  hash_prefix_len=%d\n",
	    prefix_depth, num_strings, need_more_hash_prefix_depth,
	    hash_prefix_len);
#endif /* 0 */
    need_more_hash_prefix_depth = prefix_depth;
  }
  return 0; /* not found */
}

struct pike_string *binary_findstring(const char *foo, ptrdiff_t l)
{
  return internal_findstring(foo, l, 0, StrHash(foo,l));
}

struct pike_string *binary_findstring_pcharp(PCHARP foo, ptrdiff_t l)
{
  int in = foo.shift;
  void *tmp = NULL;
  struct pike_string *res;
  if( !foo.shift )
    return binary_findstring( (void*)foo.ptr, l );
  
  if( UNLIKELY(foo.shift == 2) )
    foo.shift=find_magnitude2( (void*)foo.ptr, l );
  else if( foo.shift == 1 )
    foo.shift=find_magnitude1( (void*)foo.ptr, l );

  if( UNLIKELY(foo.shift != in) )
  {
    tmp = malloc( l * (1<<foo.shift) );
    switch(in)
    {
      case 1:
	convert_1_to_0( tmp, (void*)foo.ptr, l );
	break;
      case 2:
	if( foo.shift == 1 )
	  convert_2_to_1( tmp, (void*)foo.ptr, l );
	else
	  convert_2_to_0( tmp, (void*)foo.ptr, l );
    }
    foo.ptr = tmp;
  }

  res=internal_findstring((void*)foo.ptr, l, foo.shift, 
			  low_do_hash(foo.ptr,l,foo.shift));

  if( tmp )
    free( tmp );
  return res;
}

struct pike_string *findstring(const char *foo)
{
  return binary_findstring(foo, strlen(foo));
}

/*
 * find a string that is already shared and move it to the head
 * of that list in the hastable
 */
static struct pike_string *propagate_shared_string(const struct pike_string *s,
						   ptrdiff_t h)
{
  struct pike_string *curr, **prev, **base;

  for(base = prev = base_table + h;( curr=*prev ); prev=&curr->next)
  {
    if (curr == s) /* found it */
    {
      *prev=curr->next;
      curr->next=*base;
      *base=curr;
      return curr;
    }
#ifdef PIKE_DEBUG
    if(curr->refs<1)
    {
      debug_dump_pike_string(curr, 70);
      locate_problem(has_zero_refs);
      Pike_fatal("String with no references.\n");
    }
#endif
  }
  return 0; /* not found */
}

/*** rehash ***/

static void rehash_string_backwards(struct pike_string *s)
{
  struct pike_string *prev = NULL;
  struct pike_string *next;

  if(!s) return;

  /* Reverse the hash list. */
  while ((next = s->next)) {
    s->next = prev;
    prev = s;
    s = next;
  }
  s->next = prev;

  /* Rehash the strings for this list. */
  do {
    ptrdiff_t h = HMODULO(s->hval);
    next = s->next;
    s->next = base_table[h];
    base_table[h] = s;
  } while ((s = next));
}

static void stralloc_rehash(void)
{
  int h,old;
  struct pike_string **old_base;

  old=htable_size;
  old_base=base_table;

  SET_HSIZE(htable_size<<1);

  base_table=xcalloc(sizeof(struct pike_string *), htable_size);

  need_more_hash_prefix_depth = 0;

  for(h=0;h<old;h++)
    rehash_string_backwards(old_base[h]);

  if(old_base)
    free(old_base);
}

/* Allocation of strings */

/* Without the str at the end, to get the size of the header. */
struct pike_string_hdr {
  PIKE_STRING_CONTENTS;
};

/* Allocate some fixed string sizes with BLOCK_ALLOC. */

#define SHORT_STRING_BLOCK	256


/* breakpoint at 50% overhead. This doubles the size of short strings
   on 64-bit systems, but it only increases memory usage for short
   strings, on average, by 25% or so. */

#define SHORT_STRING_THRESHOLD ((ptrdiff_t)sizeof(struct pike_string_hdr)-1)

struct short_pike_string0 {
  PIKE_STRING_CONTENTS;
  p_wchar0 str[sizeof(struct pike_string_hdr)];
};

static struct block_allocator string_allocator = BA_INIT(sizeof(struct short_pike_string0), SHORT_STRING_BLOCK);

#define free_unlinked_pike_string(s) do { \
    if (s->flags & STRING_IS_SHORT) { \
      ba_free(&string_allocator, s);  \
    } else { \
      debug_free(s, DMALLOC_LOCATION(), 1); \
    } \
  } while(0)

/* note that begin_shared_string expects the _exact_ size of the string,
 * not the maximum size
 */
PMOD_EXPORT struct pike_string *debug_begin_shared_string(size_t len)
{
  struct pike_string *t;
#ifdef PIKE_DEBUG
  if(d_flag>10)
    verify_shared_strings_tables();
#endif
  if (len <= SHORT_STRING_THRESHOLD)
  {
    t=ba_alloc(&string_allocator);
    t->flags = STRING_NOT_HASHED | STRING_NOT_SHARED | STRING_IS_SHORT;
  } else
  {
    t=xalloc(len + 1 + sizeof(struct pike_string_hdr));
    t->flags = STRING_NOT_HASHED | STRING_NOT_SHARED;
  }
  t->refs = 0;
  add_ref(t);	/* For DMALLOC */
  t->str[len]=0;
  t->len=len;
/*  t->min = t->max = 0; */
  t->size_shift=0;
  DO_IF_DEBUG(t->next = NULL);
  return t;
}

static void link_pike_string(struct pike_string *s, size_t hval)
{
  size_t h;

#ifdef PIKE_DEBUG
  if (!(s->flags & STRING_NOT_SHARED)) {
    debug_dump_pike_string(s, 70);
    Pike_fatal("String already linked.\n");
  }

  if (PIKE_MEM_NOT_DEF_RANGE (s->str, (s->len + 1) << s->size_shift))
    Pike_fatal ("Got undefined contents in pike string %p.\n", s);
#endif

  h=HMODULO(hval);
  s->next = base_table[h];
  base_table[h] = s;
  s->hval=hval;
  s->flags &= ~(STRING_NOT_HASHED|STRING_NOT_SHARED);
  num_strings++;

  if(num_strings > htable_size) {
    stralloc_rehash();
  }

  /* These heuristics might require tuning! /Hubbe */
  if((need_more_hash_prefix_depth > 4) ||
     (need_new_hashkey_depth > 128))
  {
    /* Changed heuristic 2005-01-17:
     *
     *   Increase hash_prefix_len if there's some bucket containing
     *   more than 4 strings that are longer
     *   than hash_prefix_len.
     * /grubba
     *
     * Changed heuristic 2011-12-30:
     *
     *   Generate a new hash key if there's some bucket containing
     *   more than  16 strings. This ought to
     *   suffice to alleviate the #hashdos vulnerability.
     *
     * /grubba
     */

    /* This could in theory have a pretty ugly complexity
     * /Hubbe
     */

    if (need_new_hashkey_depth > 128) {
      /* A simple mixing function. */
      hashkey ^= (hashkey << 5) ^ (current_time.tv_sec ^ current_time.tv_usec);
      need_new_hashkey_depth = 0;
    }

    if (need_more_hash_prefix_depth > 4)
      hash_prefix_len=hash_prefix_len*2;

    /* NOTE: No need to update to the correct values, since that will
     *       be done on demand.
     */
    need_more_hash_prefix_depth=0;

    for(h=0;h<htable_size;h++)
    {
      struct pike_string *tmp=base_table[h];
      base_table[h]=0;
      while(tmp)
      {
	size_t h2;
	struct pike_string *tmp2=tmp; /* First unlink */
	tmp=tmp2->next;

	tmp2->hval=do_hash(tmp2); /* compute new hash value */
	h2=HMODULO(tmp2->hval);

	tmp2->next=base_table[h2];    /* and re-hash */
	base_table[h2]=tmp2;
      }
    }
  }
}

PMOD_EXPORT struct pike_string *debug_begin_wide_shared_string(size_t len, int shift)
{
  struct pike_string *t = NULL;
#ifdef PIKE_DEBUG
  if(d_flag>10)
    verify_shared_strings_tables();
#endif
  if ((len<<shift) <= SHORT_STRING_THRESHOLD) {
#ifdef PIKE_DEBUG
    if (shift > 2)
      Pike_fatal("Unsupported string shift: %d\n", shift);
#endif /* PIKE_DEBUG */
    t=ba_alloc(&string_allocator);
    t->flags = STRING_NOT_HASHED|STRING_NOT_SHARED|STRING_IS_SHORT;
  } else {
    t=xalloc(((len + 1)<<shift) + sizeof(struct pike_string_hdr));
    t->flags = STRING_NOT_HASHED|STRING_NOT_SHARED;
  }
  t->refs = 0;
  add_ref(t);	/* For DMALLOC */
  t->len=len;
  t->size_shift=shift;
  DO_IF_DEBUG(t->next = NULL);
  low_set_index(t,len,0);
  return t;
}

/*
 * This function assumes that the shift size is already the minimum it
 * can be.
 */
struct pike_string *low_end_shared_string(struct pike_string *s)
{
  ptrdiff_t len;
  size_t h=0;
  struct pike_string *s2;

#ifdef PIKE_DEBUG
  if (d_flag) {
    switch (s->size_shift) {
      case 0:
	break;

      case 1:
	if(!find_magnitude1(STR1(s),s->len))
	  Pike_fatal ("String %p that should have shift 1 really got 0.\n", s);
	break;

      case 2: {
	int m = find_magnitude2 (STR2 (s), s->len);
	if (m != 2)
	  Pike_fatal ("String %p that should have shift 2 really got %d.\n",
		      s, m);
	break;
      }

      default:
	Pike_fatal("ARGHEL! size_shift:%d\n", s->size_shift);
    }
  }
#endif

  len = s->len;
  if (s->flags & STRING_NOT_HASHED) {
    h = s->hval = do_hash(s);
    s->flags &= ~STRING_NOT_HASHED;
  }
  s2 = internal_findstring(s->str, len, s->size_shift, h);
#ifdef PIKE_DEBUG
  if(s2==s) 
    Pike_fatal("end_shared_string called twice! (or something like that)\n");
#endif

  if(s2)
  {
    free_string(s);
    s = s2;
    add_ref(s);
  }else{
    link_pike_string(s, h);
  }

  return s;
  
}

/*
 * This function checks if the shift size can be decreased before
 * entering the string in the shared string table
 */
PMOD_EXPORT struct pike_string *end_shared_string(struct pike_string *s)
{
  struct pike_string *s2;

  switch(s->size_shift)
  {
    default:
#ifdef PIKE_DEBUG
      Pike_fatal("ARGHEL! size_shift:%d\n", s->size_shift);

    case 2:
#endif
      switch(find_magnitude2(STR2(s),s->len))
      {
	case 0:
	  s2=begin_shared_string(s->len);
	  convert_2_to_0(STR0(s2),STR2(s),s->len);
	  free_string(s);
	  s=s2;
	  break;

	case 1:
	  s2=begin_wide_shared_string(s->len,1);
	  convert_2_to_1(STR1(s2),STR2(s),s->len);
	  free_string(s);
	  s=s2;
	  /* Fall though */
      }
      break;

    case 1:
      if(!find_magnitude1(STR1(s),s->len))
      {
	s2=begin_shared_string(s->len);
	convert_1_to_0(STR0(s2),STR1(s),s->len);
	free_string(s);
	s=s2;
      }
      break;

    case 0: break;
  }

  return low_end_shared_string(s);
}

PMOD_EXPORT struct pike_string *end_and_resize_shared_string(struct pike_string *str, ptrdiff_t len)
{
  struct pike_string *tmp;
#ifdef PIKE_DEBUG
  if(len > str->len)
    Pike_fatal("Cannot extend string here!\n");
#endif
  if( (str->len <= SHORT_STRING_THRESHOLD) ||
      ((len > SHORT_STRING_THRESHOLD) && (str->len <= (len<<1))) )
  {
    /* Short string before and after or
     * long string and at least half the length of the buffer.
     *
     * NOTE: We lose track of the actual buffer length here, so don't
     *       use this function to shorten a string in decrements.
     */
    str->len=len;
    SET_INDEX_PCHARP(MKPCHARP_STR(str), len, 0);
    return end_shared_string(str);
  }
  tmp = make_shared_binary_pcharp(MKPCHARP_STR(str),len);
  free_string(str);
  return tmp;
}


PMOD_EXPORT struct pike_string * debug_make_shared_binary_string(const char *str,size_t len)
{
  struct pike_string *s;
  ptrdiff_t h = StrHash(str, len);

  s = internal_findstring(str,len,0,h);
  if (!s) 
  {
    s=begin_shared_string(len);
    memcpy(s->str, str, len);
    link_pike_string(s, h);
  } else {
    add_ref(s);
  }

  return s;
}

PMOD_EXPORT struct pike_string * debug_make_shared_binary_pcharp(const PCHARP str,size_t len)
{
  switch(str.shift)
  {
    case 0:
      return make_shared_binary_string((char *)(str.ptr),  len);
    case 1:
      return make_shared_binary_string1((p_wchar1 *)(str.ptr),  len);
    case 2:
      return make_shared_binary_string2((p_wchar2 *)(str.ptr),  len);
#ifdef PIKE_DEBUG
    default:
      Pike_fatal("Unknown string width!\n");
#endif
  }
  /* NOT REACHED */
  return NULL;	/* Keep the compiler happy */
}

PMOD_EXPORT struct pike_string * debug_make_shared_pcharp(const PCHARP str)
{
  return debug_make_shared_binary_pcharp(str, pcharp_strlen(str));
}

PMOD_EXPORT struct pike_string * debug_make_shared_binary_string0(const p_wchar0 *str,size_t len)
{
  return debug_make_shared_binary_string((const char *)str, len);
}

PMOD_EXPORT struct pike_string * debug_make_shared_binary_string1(const p_wchar1 *str,size_t len)
{
  struct pike_string *s;
  ptrdiff_t h;

  if(!find_magnitude1(str,len))
  {
    /* Wrong size, convert */
    s=begin_shared_string(len);
    convert_1_to_0(STR0(s),str,len);
    return end_shared_string(s);
  }

  h=low_do_hash(str, len, 1);

  s = internal_findstring((char *)str,len,1,h);
  if (!s) 
  {
    s=begin_wide_shared_string(len,1);
    memcpy(s->str, str, len<<1);
    link_pike_string(s, h);
  } else {
    add_ref(s);
  }

  return s;
}

PMOD_EXPORT struct pike_string * debug_make_shared_binary_string2(const p_wchar2 *str,size_t len)
{
  struct pike_string *s;
  ptrdiff_t h;

  switch(find_magnitude2(str,len))
  {
    case 0:
      /* Wrong size, convert */
      s=begin_shared_string(len);
      convert_2_to_0(STR0(s),str,len);
      return end_shared_string(s);

    case 1:
      /* Wrong size, convert */
      s=begin_wide_shared_string(len,1);
      convert_2_to_1(STR1(s),str,len);
      return end_shared_string(s); /* not entirely optimal */
  }

  h=low_do_hash(str, len, 2);

  s = internal_findstring((char *)str,len,2,h);
  if (!s) 
  {
    s=begin_wide_shared_string(len,2);
    memcpy(s->str, str, len<<2);
    link_pike_string(s, h);
  } else {
    add_ref(s);
  }

  return s;
}

PMOD_EXPORT struct pike_string *debug_make_shared_string(const char *str)
{
  return make_shared_binary_string(str, strlen(str));
}

PMOD_EXPORT struct pike_string *debug_make_shared_string0(const p_wchar0 *str)
{
  return debug_make_shared_string((const char *)str);
}

PMOD_EXPORT struct pike_string *debug_make_shared_string1(const p_wchar1 *str)
{
  INT32 len;
  for(len=0;str[len];len++);
  return debug_make_shared_binary_string1(str,len);
}

PMOD_EXPORT struct pike_string *debug_make_shared_string2(const p_wchar2 *str)
{
  INT32 len;
  for(len=0;str[len];len++);
  return debug_make_shared_binary_string2(str,len);
}

/*** Free strings ***/

PMOD_EXPORT void unlink_pike_string(struct pike_string *s)
{
  size_t h;
  h= HMODULO(s->hval);
  propagate_shared_string(s,h);
#ifdef PIKE_DEBUG
  if (base_table[h] != s) {
    Pike_fatal("propagate_shared_string() failed. Probably got bogus pike_string.\n");
  }
#endif /* PIKE_DEBUG */
  base_table[h]=s->next;
#ifdef PIKE_DEBUG
  s->next=(struct pike_string *)(ptrdiff_t)-1;
#endif
  num_strings--;
  s->flags |= STRING_NOT_SHARED;
}

PMOD_EXPORT void do_free_string(struct pike_string *s)
{
  if (s)
    free_string(s);
}

PMOD_EXPORT void do_free_unlinked_pike_string(struct pike_string *s)
{
  if (s)
    free_unlinked_pike_string(s);
}

PMOD_EXPORT void really_free_string(struct pike_string *s)
{
#ifdef PIKE_DEBUG
  if (s->refs) {
#ifdef DEBUG_MALLOC
    describe_something(s, T_STRING, 0,2,0, NULL);
#endif
    Pike_fatal("Freeing string with %d references.\n", s->refs);
  }
  if(d_flag > 2 && !(s->flags & STRING_NOT_SHARED))
  {
    if(s->next == (struct pike_string *)(ptrdiff_t)-1)
      Pike_fatal("Freeing shared string again!\n");

    if(((ptrdiff_t)s->next) & 1)
      Pike_fatal("Freeing shared string again, memory corrupt or other bug!\n");
  }
  if (s->size_shift > 2) {
    Pike_fatal("Freeing string with bad shift (0x%08x); could it be a type?\n",
	  s->size_shift);
  }
#endif
  if (!(s->flags & STRING_NOT_SHARED))
    unlink_pike_string(s);
  if (s->flags & STRING_CLEAR_ON_EXIT)
    guaranteed_memset(s->str, 0, s->len<<s->size_shift);
  free_unlinked_pike_string(s);
  GC_FREE_SIMPLE_BLOCK(s);
}

void do_really_free_string(struct pike_string *s)
{
  if (s)
    really_free_string(s);
}

PMOD_EXPORT void debug_free_string(struct pike_string *s)
{
  if(!sub_ref(s))
    really_free_string(s);
}


/*
 * String table status
 */
struct pike_string *add_string_status(int verbose)
{
  struct string_builder s;
  init_string_builder(&s, 0);

  if (verbose)
  {
    long alloced_strings[8] = {0,0,0,0,0,0,0,0};
    long alloced_bytes[8] = {0,0,0,0,0,0,0,0};
    long num_distinct_strings[8] = {0,0,0,0,0,0,0,0};
    long bytes_distinct_strings[8] = {0,0,0,0,0,0,0,0};
    long overhead_bytes[8] = {0,0,0,0,0,0,0,0};
    unsigned INT32 e;
    struct pike_string *p;
    for(e=0;e<htable_size;e++)
    {
      for(p=base_table[e];p;p=p->next)
      {
	int is_short = (p->len <= SHORT_STRING_THRESHOLD);
	int key = (is_short?0:4) | p->size_shift;
	num_distinct_strings[key]++;
	if (is_short) {
	  bytes_distinct_strings[key] +=
	    DO_ALIGN(SHORT_STRING_THRESHOLD << p->size_shift, sizeof(void *));
	} else {
	  bytes_distinct_strings[key] +=
	    DO_ALIGN(p->len << p->size_shift, sizeof(void *));
	}
	alloced_strings[key] += p->refs;
	alloced_bytes[key] +=
	  p->refs*DO_ALIGN((p->len+3) << p->size_shift,sizeof(void *));
      }
    }
    string_builder_sprintf(&s,
			   "\nShared string hash table:\n"
			   "-------------------------\n"
			   "\n"
			   "Type          Count Distinct    Bytes   Actual Overhead    %%\n"
			   "------------------------------------------------------------\n");
    for(e = 0; e < 8; e++) {
      int shift = e & 3;
      ptrdiff_t overhead;
      if (!num_distinct_strings[e]) continue;
      if (shift != 3) {
	if (e < 4) {
	  string_builder_sprintf(&s, "Short/%-2d   ", 8<<shift);
	} else {
	  string_builder_sprintf(&s, "Long/%-2d    ", 8<<shift);
	}

	overhead_bytes[e] =
	  DO_NOT_WARN((long)sizeof(struct pike_string_hdr) *
		      num_distinct_strings[e]);

	alloced_strings[e|3] += alloced_strings[e];
	alloced_bytes[e|3] += alloced_bytes[e];
	num_distinct_strings[e|3] += num_distinct_strings[e];
	bytes_distinct_strings[e|3] += bytes_distinct_strings[e];
	overhead_bytes[e|3] += overhead_bytes[e];
      } else {
	if (e < 4) {
	  string_builder_sprintf(&s, "Total short");
	} else {
	  string_builder_sprintf(&s, "Total long ");
	}
      }
      string_builder_sprintf(&s,
			     "%8ld %8ld %8ld %8ld %8ld ",
			     alloced_strings[e], num_distinct_strings[e],
			     alloced_bytes[e], bytes_distinct_strings[e],
			     overhead_bytes[e]);
      if (alloced_bytes[e]) {
	string_builder_sprintf(&s, "%4d\n",
			       (bytes_distinct_strings[e] +
				overhead_bytes[e]) * 100 /
			       alloced_bytes[e]);
      } else {
	string_builder_strcat(&s, "   -\n");
      }
    }
    alloced_strings[7] += alloced_strings[3];
    alloced_bytes[7] += alloced_bytes[3];
    num_distinct_strings[7] += num_distinct_strings[3];
    bytes_distinct_strings[7] += bytes_distinct_strings[3];
    overhead_bytes[7] += overhead_bytes[3];
    string_builder_sprintf(&s,
			   "------------------------------------------------------------\n"
			   "Total      %8ld %8ld %8ld %8ld %8ld ",
			   alloced_strings[7], num_distinct_strings[7],
			   alloced_bytes[7], bytes_distinct_strings[7],
			   overhead_bytes[7]);
    if (alloced_bytes[7]) {
      string_builder_sprintf(&s, "%4d\n",
			     (bytes_distinct_strings[7] +
			      overhead_bytes[7]) * 100 /
			     alloced_bytes[7]);
    } else {
      string_builder_strcat(&s, "   -\n");
    }
  }
/*
  sprintf(b,"Searches: %ld    Average search length: %6.3f\n",
      (long)num_str_searches, (double)search_len / num_str_searches);
  my_strcat(b);
*/
  return finish_string_builder(&s);
}

/*** PIKE_DEBUG ***/
#ifdef PIKE_DEBUG

static long last_stralloc_verify=0;
extern long current_do_debug_cycle;

PMOD_EXPORT void check_string(struct pike_string *s)
{
  if(current_do_debug_cycle == last_stralloc_verify)
  {
    if(debug_findstring(s) !=s)
      Pike_fatal("Shared string not shared.\n");
  }else{

    switch (s->size_shift) {
      case 0:
	break;
      case 1: {
	ptrdiff_t i;
	p_wchar1 *str = STR1 (s);
	for (i = 0; i < s->len; i++)
	  if (str[i] > 0xff)
	    goto size_shift_check_done;
	Pike_fatal ("Shared string is too wide.\n");
      }
      case 2: {
	ptrdiff_t i;
	p_wchar2 *str = STR2 (s);
	for (i = 0; i < s->len; i++)
	  if ((str[i] > 0xffff) || (str[i] < 0))
	    goto size_shift_check_done;
	Pike_fatal ("Shared string is too wide.\n");
      }
      default:
	Pike_fatal ("Invalid size shift %d.\n", s->size_shift);
    }
  size_shift_check_done:;

    if(do_hash(s) != s->hval)
    {
      locate_problem(wrong_hash);
      Pike_fatal("Hash value changed?\n");
    }

    if(debug_findstring(s) !=s)
      Pike_fatal("Shared string not shared.\n");

    if(index_shared_string(s,s->len))
    {
      locate_problem(improper_zero_termination);
      Pike_fatal("Shared string is not zero terminated properly.\n");
    }
  }
}

PMOD_EXPORT void verify_shared_strings_tables(void)
{
  unsigned INT32 e, h, num=0;
  struct pike_string *s;

  last_stralloc_verify=current_do_debug_cycle;

  for(e=0;e<htable_size;e++)
  {
    h=0;
    for(s=base_table[e];s;s=s->next)
    {
      num++;
      h++;

      if (bad_pointer(s)) {
	Pike_fatal("Odd string pointer in string table!\n");
      }

      if(s->len < 0)
	Pike_fatal("Shared string shorter than zero bytes.\n");

      if(s->refs <= 0)
      {
	locate_problem(has_zero_refs);
	Pike_fatal("Shared string had too few references.\n");
      }

      if(index_shared_string(s,s->len))
      {
	locate_problem(improper_zero_termination);
	Pike_fatal("Shared string didn't end with a zero.\n");
      }

      if(do_hash(s) != s->hval)
      {
	locate_problem(wrong_hash);
	Pike_fatal("Shared string hashed to other number.\n");
      }

      if(HMODULO(s->hval) != e)
      {
	locate_problem(wrong_hash);
	Pike_fatal("Shared string hashed to wrong place.\n");
      }

      if(h>10000)
      {
	struct pike_string *s2;
	for(s2=s;s2;s2=s2->next)
	  if(s2 == s)
	    Pike_fatal("Shared string table is cyclic.\n");
	h=0;
      }
    }
  }
  if(num != num_strings)
    Pike_fatal("Num strings is wrong %d!=%d\n",num,num_strings);
}

int safe_debug_findstring(const struct pike_string *foo)
{
  unsigned INT32 e;
  if(!base_table) return 0;
  for(e=0;e<htable_size;e++)
  {
    struct pike_string *p;
    for(p=base_table[e];p;p=p->next)
    {
      if(p==foo)
      {
	return 1;
      }
    }
  }
  return 0;
}

struct pike_string *debug_findstring(const struct pike_string *foo)
{
  struct pike_string *tmp;
  tmp=propagate_shared_string(foo, HMODULO(foo->hval));

#if 0
  if(!tmp)
  {
    unsigned INT32 e;
    struct pike_string *tmp2;
    fprintf(stderr,"String %p %ld %ld %s\n",
	    foo,
	    (long)foo->hval,
	    (long)foo->len,
	    foo->str);

    fprintf(stderr,"------ %p %ld\n",
	    base_table[HMODULO(foo->hval)],
	    foo->hval);
    for(tmp2=base_table[HMODULO(foo->hval)];tmp2;tmp2=tmp2->next)
    {
      if(tmp2 == tmp)
	fprintf(stderr,"!!%p!!->",tmp2);
      else
	fprintf(stderr,"%p->",tmp2);
    }
    fprintf(stderr,"0\n");

    for(e=0;e<htable_size;e++)
    {
      for(tmp2=base_table[e];tmp2;tmp2=tmp2->next)
      {
	if(tmp2 == tmp)
	  fprintf(stderr,"String found in hashbin %ld (not %ld)\n",
		  (long)e,
		  (long)HMODULO(foo->hval));
      }
    }
  }
#endif /* 0 */
  return tmp;
}

PMOD_EXPORT void debug_dump_pike_string(const struct pike_string *s, INT32 max)
{
  INT32 e;
  fprintf(stderr,"0x%p: %ld refs, len=%ld, size_shift=%d, hval=%lux (%lx)\n",
	  s,
	  (long)s->refs,
	  DO_NOT_WARN((long)s->len),
	  s->size_shift,
	  DO_NOT_WARN((unsigned long)s->hval),
	  DO_NOT_WARN((unsigned long)StrHash(s->str, s->len)));
  fprintf(stderr," \"");
  for(e=0;e<s->len && max>0;e++)
  {
    int c=EXTRACT_UCHAR(s->str+e);
    switch(c)
    {
      case '\t': fprintf(stderr,"\\t"); max-=2; break;
      case '\n': fprintf(stderr,"\\n"); max-=2; break;
      case '\r': fprintf(stderr,"\\r"); max-=2; break;
      case '\b': fprintf(stderr,"\\b"); max-=2; break;

      default:
	if(is8bitalnum(c) || c==' ' || isgraph(c))
	{
	  putc(c,stderr);
	  max--;
	}else{
	  fprintf(stderr,"\\%03o",c);
	  max-=4;
	}
    }
  }
  if(!max)
    fprintf(stderr,"...\n");
  else
    fprintf(stderr,"\"\n");
}

void dump_stralloc_strings(void)
{
  unsigned INT32 e;
  struct pike_string *p;
  for(e=0;e<htable_size;e++)
  {
    for(p=base_table[e];p;p=p->next) {
      debug_dump_pike_string(p, 70);
#ifdef DEBUG_MALLOC
      debug_malloc_dump_references (p, 2, 1, 0);
#endif
    }
  }
}

#endif /* PIKE_DEBUG */


/*** String compare functions ***/

/* does not take locale into account */
int low_quick_binary_strcmp(const char *a, ptrdiff_t alen,
			    const char *b, ptrdiff_t blen)
{
  int tmp;
  if(alen > blen)
  {
    tmp=memcmp(a, b, blen);
    if(tmp) return tmp;
    return 1;
  }else if(alen < blen){
    tmp=memcmp(a, b, alen);
    if(tmp) return tmp;
    return -1;
  }else{
    return memcmp(a, b, alen);
  }
}


/* does not take locale into account */
ptrdiff_t generic_quick_binary_strcmp(const char *a,
                                      ptrdiff_t alen, int asize,
                                      const char *b,
                                      ptrdiff_t blen, int bsize)
{
  ptrdiff_t pos;
  if(!asize && !bsize)
    return low_quick_binary_strcmp(a, alen, b, blen);

  for(pos=0;pos< MINIMUM(alen,blen) ;pos++)
  {
    p_wchar2 ac=generic_extract(a,asize,pos);
    p_wchar2 bc=generic_extract(b,bsize,pos);
    if(ac != bc) {
      if (ac < bc) return -1;
      return 1;
    }
  }
  return alen-blen;
}

/* Does not take locale into account
 *
 * Similar to (and could be used in place of) generic_quick_binary_strcmp(),
 * but returns +/- (offset + 1) to the first difference beween the strings.
 *
 * This can be used by eg replace_many() to speed up the comparisons.
 */
ptrdiff_t generic_find_binary_prefix(const char *a,
                                     ptrdiff_t alen, int asize,
                                     const char *b,
                                     ptrdiff_t blen, int bsize)
{
  ptrdiff_t pos;
  ptrdiff_t len = MINIMUM(alen, blen);
  switch(TWO_SIZES(asize, bsize)) {
#define CASE(AZ, BZ)				\
    case TWO_SIZES(AZ, BZ): {			\
      PIKE_CONCAT(p_wchar, AZ) *a_arr =		\
	(PIKE_CONCAT(p_wchar, AZ) *)a;		\
      PIKE_CONCAT(p_wchar, BZ) *b_arr =		\
	(PIKE_CONCAT(p_wchar, BZ) *)b;		\
      for (pos=0; pos<len; pos++) {		\
	if (a_arr[pos] == b_arr[pos])		\
	  continue;				\
	if (a_arr[pos] < b_arr[pos])		\
	  return ~pos;				\
	return pos+1;				\
      }						\
    } break
    CASE(0,0);
    CASE(0,1);
    CASE(0,2);
    CASE(1,0);
    CASE(1,1);
    CASE(1,2);
    CASE(2,0);
    CASE(2,1);
    CASE(2,2);
#undef CASE
  }
  if (alen == blen) return 0;
  if (alen < blen) return ~alen;
  return blen+1;
}

PMOD_EXPORT int c_compare_string(const struct pike_string *s,
                                 const char *foo, int len)
{
  return s->len == len && s->size_shift == 0 && !memcmp(s->str,foo,len);
}

/* Does not take locale into account */
PMOD_EXPORT ptrdiff_t my_quick_strcmp(const struct pike_string *a,
				      const struct pike_string *b)
{
  if(a==b) return 0;

  return generic_quick_binary_strcmp(a->str, a->len, a->size_shift,
				     b->str, b->len, b->size_shift);
}

struct pike_string *realloc_unlinked_string(struct pike_string *a,
					    ptrdiff_t size)
{
  struct pike_string *r = NULL;

  if (a->flags & STRING_IS_SHORT ) {
    if (size <= SHORT_STRING_THRESHOLD/(1<<a->size_shift))
      r = a;				/* There's already space enough. */
  } else
    r=realloc(a, sizeof(struct pike_string_hdr)+((size+1)<<a->size_shift));

  if(!r) {
    r=begin_wide_shared_string(size, a->size_shift);
    r->flags |= a->flags & ~15;
    r->min = a->min;
    r->max = a->max;
    memcpy(r->str, a->str, (a->len <= size ? a->len : size)<<a->size_shift);
    free_string(a);
  }

  r->len=size;
  low_set_index(r,size,0);		/* Enforce \0 termination */
  return r;
}

/* Returns an unlinked string ready for end_shared_string */
static struct pike_string *realloc_shared_string(struct pike_string *a,
                                                 ptrdiff_t size)
{
  struct pike_string *r;
  if(a->refs==1)
  {
    unlink_pike_string(a);
    return realloc_unlinked_string(a, size);
  }else{
    r=begin_wide_shared_string(size,a->size_shift);
    memcpy(r->str, a->str, a->len<<a->size_shift);
    r->flags |= a->flags & ~15;
    r->min = a->min;
    r->max = a->max;
    free_string(a);
    return r;
  }
}

struct pike_string *new_realloc_shared_string(struct pike_string *a, INT32 size, int shift)
{
  struct pike_string *r;
  if(shift == a->size_shift) return realloc_shared_string(a,size);

  r=begin_wide_shared_string(size,shift);
  pike_string_cpy(MKPCHARP_STR(r),a);
  r->flags |= (a->flags & ~15);
  r->min = a->min;
  r->max = a->max;
  free_string(a);
  return r;
}


/* Modify one index in a shared string
 * Not suitable for building new strings or changing multiple characters
 * within a string!
 *
 * Phew, this function become complicated when I inserted magic for wide
 * characters...
 */
struct pike_string *modify_shared_string(struct pike_string *a,
					 INT32 index,
					 INT32 c)
{
  INT32 old_value;
#ifdef PIKE_DEBUG
  if(index<0 || index>=a->len)
    Pike_fatal("Index out of range in modify_shared_string()\n");
#endif


  old_value=index_shared_string(a,index);
  if(old_value==c) return a;

  /* First test if the string needs to be grown:
   * ie; the new value does not fit in the char size of
   * the old string
   */

  if(min_magnitude(c) > a->size_shift)
  {
    /* String must be grown */
    struct pike_string *b;

    switch(TWO_SIZES(min_magnitude(c),a->size_shift))
    {
      case TWO_SIZES(1,0):
	b=begin_wide_shared_string(a->len,1);
	convert_0_to_1(STR1(b),(p_wchar0 *)a->str,a->len);
	STR1(b)[index]=c;
	free_string(a);
	return end_shared_string(b);

      case TWO_SIZES(2,0):
	b=begin_wide_shared_string(a->len,2);
	convert_0_to_2(STR2(b),(p_wchar0 *)a->str,a->len);
	STR2(b)[index]=c;
	free_string(a);
	return end_shared_string(b);

      case TWO_SIZES(2,1):
	b=begin_wide_shared_string(a->len,2);
	convert_1_to_2(STR2(b),STR1(a),a->len);
	STR2(b)[index]=c;
	free_string(a);
	return end_shared_string(b);

#ifdef PIKE_DEBUG
      default:
	Pike_fatal("Odd wide string conversion!\n");
#endif
    }
  }


  /* Next we test if the new string can be shrunk
   * if all characters in the new string can fit in a string
   * of a lower magnitude, it must be shrunk
   */
     
  if(min_magnitude(old_value) == a->size_shift &&
     min_magnitude(c) < min_magnitude(old_value))
  {
    /* We *might* need to shrink the string */
    struct pike_string *b;
    unsigned int size,tmp;

    switch(a->size_shift)
    {
      case 0:
	Pike_fatal("Unshrinkable!\n");

      case 1:
	/* Test if we *actually* can shrink it.. */
	if(find_magnitude1(STR1(a),index)) break;
	if(find_magnitude1(STR1(a)+index+1,a->len-index-1))
	  break;
	
	b=begin_shared_string(a->len);
	convert_1_to_0((p_wchar0 *)b->str,STR1(a),a->len);
	b->str[index]=c;
	free_string(a);
	return end_shared_string(b);

      case 2:
	/* Test if we *actually* can shrink it.. */
	size=find_magnitude2(STR2(a),index);
	if(size==2) break; /* nope */
	tmp=find_magnitude2(STR2(a)+index+1,a->len-index-1);
	if(tmp==2) break; /* nope */
	size=MAXIMUM(MAXIMUM(size,tmp),min_magnitude(c));

	switch(size)
	{
	  case 0:
	    b=begin_shared_string(a->len);
	    convert_2_to_0((p_wchar0 *)b->str,STR2(a),a->len);
	    b->str[index]=c;
	    free_string(a);
	    return end_shared_string(b);

	  case 1:
	    b=begin_wide_shared_string(a->len,1);
	    convert_2_to_1((p_wchar1 *)b->str,STR2(a),a->len);
	    STR1(b)[index]=c;
	    free_string(a);
	    return end_shared_string(b);
	}
    }
  }
     

  /* We now know that the string has the right character size */
  if(a->refs==1)
  {
    /* One ref - destructive mode */

    unlink_pike_string(a);
    low_set_index(a, index, c);
    CLEAR_STRING_CHECKED(a);
    if((((unsigned int)index) >= hash_prefix_len) && (index < a->len-8))
    {
      struct pike_string *old;
      /* Doesn't change hash value - sneak it in there */
#ifdef PIKE_DEBUG
      if (wrong_hash(a)) {
	Pike_fatal("Broken hash optimization.\n");
      }
#endif
      old = internal_findstring(a->str, a->len, a->size_shift, a->hval);
      if (old) {
	/* The new string is equal to some old string. */
	free_string(a);
	add_ref(a = old);
      } else {
	link_pike_string(a, a->hval);
      }
    }else{
      a = end_shared_string(a);
    }
    return a;
  }else{
    struct pike_string *r;
    r=begin_wide_shared_string(a->len,a->size_shift);
    memcpy(r->str, a->str, a->len << a->size_shift);
    low_set_index(r,index,c);
    free_string(a);
    return end_shared_string(r);
  }
}

static void set_flags_for_add( struct pike_string *ret,
                               unsigned char aflags,
                               unsigned char amin,
                               unsigned char amax,
                               const struct pike_string *b)
{
  if( !b->len ) {
    ret->flags |= aflags & ~15;
    ret->min = amin;
    ret->max = amax;
    return;
  }
  if( aflags & b->flags & STRING_CONTENT_CHECKED )
  {
    ret->min = MINIMUM( amin, b->min );
    ret->max = MAXIMUM( amax, b->max );
    ret->flags |= STRING_CONTENT_CHECKED;
  }
  else
    ret->flags &= ~STRING_CONTENT_CHECKED;

  ret->flags &= ~(STRING_IS_LOWERCASE | STRING_IS_UPPERCASE);
  ret->flags |= (aflags & b->flags & (STRING_IS_LOWERCASE | STRING_IS_UPPERCASE));
}

void update_flags_for_add( struct pike_string *a, const struct pike_string *b)
{
  if( !b->len ) return;
  if( a->flags & STRING_CONTENT_CHECKED )
  {
    if(b->flags & STRING_CONTENT_CHECKED)
    {
      if( b->min < a->min ) a->min = b->min;
      if( b->max > a->max ) a->max = b->max;
    }
    else
      a->flags &= ~STRING_CONTENT_CHECKED;
  }

  a->flags &= ~(STRING_IS_LOWERCASE | STRING_IS_UPPERCASE) | b->flags;
}

/*** Add strings ***/
PMOD_EXPORT struct pike_string *add_shared_strings(const struct pike_string *a,
                                                   const struct pike_string *b)
{
  struct pike_string *ret;
  PCHARP tmp;
  int target_size=MAXIMUM(a->size_shift,b->size_shift);

  ret=begin_wide_shared_string(a->len+b->len,target_size);
  tmp=MKPCHARP_STR(ret);
  pike_string_cpy(tmp,a);
  INC_PCHARP(tmp,a->len);
  pike_string_cpy(tmp,b);
  set_flags_for_add( ret, a->flags, a->min, a->max, b );
  return low_end_shared_string(ret);
}

PMOD_EXPORT struct pike_string *add_and_free_shared_strings(struct pike_string *a,
                                                            struct pike_string *b)
{
  ptrdiff_t alen = a->len;
  if(a->size_shift == b->size_shift)
  {
    a = realloc_shared_string(a, alen + b->len);
    update_flags_for_add( a, b );
    memcpy(a->str+(alen<<a->size_shift),b->str,b->len<<b->size_shift);
    free_string(b);
    a->flags |= STRING_NOT_HASHED;
    return end_shared_string(a);
  }else{
    struct pike_string *ret=add_shared_strings(a,b);
    free_string(a);
    free_string(b);
    return ret;
  }
}


PMOD_EXPORT ptrdiff_t string_search(struct pike_string *haystack,
				    struct pike_string *needle,
				    ptrdiff_t start)
{
  SearchMojt mojt;
  char *r;

  if( !string_range_contains_string( haystack, needle ) )
    return -1;

  if(start + needle->len > haystack->len)
    return -1;

  if(!needle->len) return start;

  mojt=compile_memsearcher(MKPCHARP_STR(needle),
			   needle->len,
			   haystack->len,
			   needle);

  r = (char *)mojt.vtab->funcN(mojt.data,
			       ADD_PCHARP(MKPCHARP_STR(haystack), start),
			       haystack->len - start).ptr;

  if (mojt.container) free_object (mojt.container);

  if(!r) return -1;
#ifdef PIKE_DEBUG
  if((r < haystack->str) ||
     (r - haystack->str)>>haystack->size_shift > haystack->len)
    Pike_fatal("string_search did a bobo!\n");
#endif
  return (r-haystack->str)>>haystack->size_shift;
}

PMOD_EXPORT struct pike_string *string_slice(struct pike_string *s,
					     ptrdiff_t start,
					     ptrdiff_t len)
{
#ifdef PIKE_DEBUG
  if(start < 0 || len<0 || start+len>s->len )
  {
    Pike_fatal("string_slice, start = %ld, len = %ld, s->len = %ld\n",
	  DO_NOT_WARN((long)start),
	  DO_NOT_WARN((long)len),
	  DO_NOT_WARN((long)s->len));
  }
#endif
  if( len == 0)
  {
    add_ref(empty_pike_string);
    return empty_pike_string;
  }

  if(start==0 && len==s->len)
  {
    add_ref(s);
    return s;
  }

  switch(s->size_shift)
  {
    case 0:
      return make_shared_binary_string((char *)STR0(s)+start,len);

    case 1:
      return make_shared_binary_string1(STR1(s)+start,len);

    case 2:
      return make_shared_binary_string2(STR2(s)+start,len);
  }
#ifdef PIKE_DEBUG
  Pike_fatal("Illegal shift size!\n");
#endif
  return 0;
}

/*** replace function ***/
typedef char *(* replace_searchfunc)(void *,void *,size_t);
PMOD_EXPORT struct pike_string *string_replace(struct pike_string *str,
                                               struct pike_string *del,
                                               struct pike_string *to)
{
  struct pike_string *ret;
  char *s,*tmp,*end;
  PCHARP r;
  int shift;
  SearchMojt mojt;
  ONERROR mojt_uwp;
  replace_searchfunc f = (replace_searchfunc)0;

  if(!str->len || !string_range_contains_string(str, del))
  {
    add_ref(str);
    return str;
  }

  shift=MAXIMUM(str->size_shift,to->size_shift);

  if(!del->len)
  {
    int e,pos;
    ret=begin_wide_shared_string(str->len + to->len * (str->len -1),shift);
    low_set_index(ret,0,index_shared_string(str,0));
    for(pos=e=1;e<str->len;e++)
    {
      pike_string_cpy(MKPCHARP_STR_OFF(ret,pos),to);
      pos+=to->len;
      low_set_index(ret,pos++,index_shared_string(str,e));
    }
    return end_shared_string(ret);
  }

  s=str->str;
  end=s+(str->len<<str->size_shift);

  if(del->len == to->len)
  {
    mojt=compile_memsearcher(MKPCHARP_STR(del),
			     del->len,
			     str->len,
			     del);
    SET_ONERROR (mojt_uwp, do_free_object, mojt.container);

    ret=begin_wide_shared_string(str->len,shift);
    switch(str->size_shift)
    {
      case 0: f=(replace_searchfunc)mojt.vtab->func0; break;
      case 1: f=(replace_searchfunc)mojt.vtab->func1; break;
      case 2: f=(replace_searchfunc)mojt.vtab->func2; break;
#ifdef PIKE_DEBUG
      default: Pike_fatal("Illegal shift.\n");
#endif
    }

  }else{
    INT32 delimeters=0;
    mojt=compile_memsearcher(MKPCHARP_STR(del),
			     del->len,
			     str->len*2,
			     del);
    SET_ONERROR (mojt_uwp, do_free_object, mojt.container);

    switch(str->size_shift)
    {
      case 0: f=(replace_searchfunc)mojt.vtab->func0; break;
      case 1: f=(replace_searchfunc)mojt.vtab->func1; break;
      case 2: f=(replace_searchfunc)mojt.vtab->func2; break;
#ifdef PIKE_DEBUG
      default: Pike_fatal("Illegal shift.\n");
#endif
    }

    while((s = f(mojt.data, s, (end-s)>>str->size_shift)))
    {
      delimeters++;
      s+=del->len << str->size_shift;
    }
    
    if(!delimeters)
    {
      CALL_AND_UNSET_ONERROR (mojt_uwp);
      add_ref(str);
      return str;
    }

    ret=begin_wide_shared_string(str->len + (to->len-del->len)*delimeters, shift);
  }
  s=str->str;
  r=MKPCHARP_STR(ret);

  while((tmp = f(mojt.data, s, (end-s)>>str->size_shift)))
  {
#ifdef PIKE_DEBUG
    if(tmp + (del->len << str->size_shift) > end)
      Pike_fatal("SearchMojt found a match beyond end of string!!!\n");
#endif
    generic_memcpy(r,MKPCHARP(s,str->size_shift),(tmp-s)>>str->size_shift);
    INC_PCHARP(r,(tmp-s)>>str->size_shift);
    pike_string_cpy(r,to);
    INC_PCHARP(r,to->len);
    s=tmp+(del->len << str->size_shift);
  }
  generic_memcpy(r,MKPCHARP(s,str->size_shift),(end-s)>>str->size_shift);

  CALL_AND_UNSET_ONERROR (mojt_uwp);
  return end_shared_string(ret);
}

/*** init/exit memory ***/
void init_shared_string_table(void)
{
  SET_HSIZE(BEGIN_HASH_SIZE);
  base_table=xcalloc(sizeof(struct pike_string *), htable_size);

  empty_pike_string = make_shared_string("");
  empty_pike_string->flags |= STRING_IS_LOWERCASE | STRING_IS_UPPERCASE;
}

#ifdef DO_PIKE_CLEANUP
PMOD_EXPORT struct shared_string_location *all_shared_string_locations;
#endif


void cleanup_shared_string_table(void)
{
  unsigned INT32 e;
  struct pike_string *s,*next;

  if (empty_pike_string) {
    free_string(empty_pike_string);
    empty_pike_string = 0;
  }

#ifdef DO_PIKE_CLEANUP
  while(all_shared_string_locations)
  {
    struct shared_string_location *x=all_shared_string_locations;
    all_shared_string_locations=x->next;
    free_string(x->s);
    x->s=0;
  }

  if (exit_with_cleanup)
  {
    size_t num,size;
    count_memory_in_strings(&num,&size);
    if(num)
    {
      fprintf(stderr,"Strings left: %"PRINTSIZET"d "
	      "(%"PRINTSIZET"d bytes) (zapped)\n",num,size);
#ifdef PIKE_DEBUG
      dump_stralloc_strings();
#endif
    }
  }
#endif

  for(e=0;e<htable_size;e++)
  {
    for(s=base_table[e];s;s=next)
    {
      next=s->next;
      s->next=0;
    }
    base_table[e]=0;
  }
  free(base_table);
  base_table=0;
  num_strings=0;

#ifdef DO_PIKE_CLEANUP
  ba_destroy(&string_allocator);
#endif /* DO_PIKE_CLEANUP */
}

static INLINE size_t memory_in_string (const struct pike_string *s)
{
  if (s->flags & STRING_IS_SHORT )
    return sizeof (struct short_pike_string0);
  else
    return sizeof (struct pike_string_hdr) + ((s->len + 1) << s->size_shift);
}

void count_memory_in_short_pike_strings(size_t *num, size_t *size)
{
  ba_count_all(&string_allocator, num, size);
}

void count_memory_in_strings(size_t *num, size_t *size)
{
  unsigned INT32 e;
  size_t num_=0, size_=0;
  if(!base_table)
  {
    *num=*size=0;
    return;
  }
  size_+=htable_size * sizeof(struct pike_string *);
  for(e=0;e<htable_size;e++)
  {
    struct pike_string *p;
    for(p=base_table[e];p;p=p->next)
    {
      num_++;
      size_ += memory_in_string (p);
    }
  }
#ifdef PIKE_DEBUG
  if(num_strings != num_)
    Pike_fatal("Num strings is wrong! %d!=%d.\n",num_strings, num_);
#endif
  num[0]=num_;
  size[0]=size_;
}

PMOD_EXPORT void visit_string (struct pike_string *s, int action, void *extra)
{
  visit_enter(s, T_STRING, extra);
  switch (action) {
#ifdef PIKE_DEBUG
    default:
      Pike_fatal ("Unknown visit action %d.\n", action);
    case VISIT_NORMAL:
    case VISIT_COMPLEX_ONLY:
      break;
#endif
    case VISIT_COUNT_BYTES:
      mc_counted_bytes += memory_in_string (s);
      break;
  }
  visit_leave(s, T_STRING, extra);
}

#ifdef PIKE_DEBUG
unsigned gc_touch_all_strings(void)
{
  unsigned INT32 e;
  unsigned n = 0;
  if (!base_table) return 0;
  for(e=0;e<htable_size;e++)
  {
    struct pike_string *p;
    for(p=base_table[e];p;p=p->next) debug_gc_touch(p), n++;
  }
  return n;
}

void gc_mark_all_strings(void)
{
  unsigned INT32 e;
  if(!base_table) return;
  for(e=0;e<htable_size;e++)
  {
    struct pike_string *p;
    for(p=base_table[e];p;p=p->next) gc_is_referenced(p);
  }
}
#endif

struct pike_string *next_pike_string (const struct pike_string *s)
{
  struct pike_string *next = s->next;
  if (!next) {
    size_t h = s->hval;
    do {
      h++;
      h = HMODULO(h);
      next = base_table[h];
    } while (!next);
  }
  return next;
}

PMOD_EXPORT void init_string_builder_alloc(struct string_builder *s, ptrdiff_t length, int mag)
{
  s->s=begin_wide_shared_string(length,mag);
  s->malloced=length;
  s->known_shift=0;
  s->s->len=0;
  low_set_index (s->s, 0, 0);
}

PMOD_EXPORT void init_string_builder(struct string_builder *s, int mag)
{
  init_string_builder_alloc(s, 256, mag);
}

PMOD_EXPORT void init_string_builder_copy(struct string_builder *to,
					  const struct string_builder *from)
{
  to->malloced = from->malloced;
  to->s = begin_wide_shared_string (from->malloced, from->s->size_shift);
  to->s->len = from->s->len;
  memcpy (to->s->str, from->s->str, (from->s->len + 1) << from->s->size_shift);
  to->known_shift = from->known_shift;
}

PMOD_EXPORT void string_build_mkspace(struct string_builder *s,
				      ptrdiff_t chars, int mag)
/* Doesn't touch or sanity check s->known_shift. */
{
  if(mag > s->s->size_shift)
  {
    struct pike_string *n;
    ptrdiff_t l = s->s->len + chars;
    if (l < s->malloced)
      l = s->malloced;
    n=begin_wide_shared_string(l,mag);
    pike_string_cpy(MKPCHARP_STR(n),s->s);
    n->len=s->s->len;
    s->s->len = s->malloced;	/* Restore the real length */
    s->malloced=l;
    free_string(s->s);
    s->s=n;
  }
  else if(s->s->len+chars > s->malloced)
  {
    ptrdiff_t newlen = MAXIMUM(s->malloced*2,
			       s->s->len + chars);
    ptrdiff_t oldlen = s->s->len;

    s->s->len = s->malloced;	/* Restore the real length */
    s->s = realloc_unlinked_string(s->s, newlen);
    s->s->len = oldlen;
    s->malloced = newlen;
  }
}

PMOD_EXPORT void *string_builder_allocate(struct string_builder *s, ptrdiff_t chars, int mag)
{
  void *ret;
  string_build_mkspace(s, chars, mag);
  if(chars<0) s->known_shift=0;
  ret = s->s->str + (s->s->len<<s->s->size_shift);
  s->s->len += chars;
  return ret;
}

PMOD_EXPORT void string_builder_putchar(struct string_builder *s, int ch)
{
  ptrdiff_t i;
  int mag = min_magnitude(ch);

  string_build_mkspace(s, 1, mag);
  if (mag > s->known_shift) {
    s->known_shift = mag;
  }
  i = s->s->len++;
  low_set_index(s->s,i,ch);
}

PMOD_EXPORT void string_builder_putchars(struct string_builder *s, int ch,
					 ptrdiff_t count)
{
  ptrdiff_t len = s->s->len;
  int mag = min_magnitude(ch);

  /* This is not really expected to happen. But since we are doing
   * memset here, a negative argument should be avoided. */
  if (count < 0)
    Pike_fatal("Non-positive count in call to string_builder_putchars().\n");
  if (!count) return;

  string_build_mkspace(s, count, mag);
  if (mag > s->known_shift) {
    s->known_shift = mag;
  }

  switch (s->s->size_shift) {
    case 0:
      memset (STR0 (s->s) + s->s->len, ch, count);
      break;
    case 1: {
      int i;
      for (i = 0; i < count; i++)
	(STR1 (s->s) + s->s->len)[i] = ch;
      break;
    }
    case 2: {
      int i;
      for (i = 0; i < count; i++)
	(STR2 (s->s) + s->s->len)[i] = ch;
      break;
    }
  }

  s->s->len += count;
}


PMOD_EXPORT void string_builder_binary_strcat0(struct string_builder *s,
					       const p_wchar0 *str, ptrdiff_t len)
{
  string_build_mkspace(s,len,0);
  switch(s->s->size_shift)
  {
    case 0: convert_0_to_0(STR0(s->s)+s->s->len,str,len); break;
    case 1: convert_0_to_1(STR1(s->s)+s->s->len,str,len); break;
    case 2: convert_0_to_2(STR2(s->s)+s->s->len,str,len); break;
#ifdef PIKE_DEBUG
    default:
      Pike_fatal ("Illegal magnitude! (%d)\n", s->s->size_shift);
#endif
  }
  s->s->len+=len;
}

PMOD_EXPORT void string_builder_binary_strcat1(struct string_builder *s,
					       const p_wchar1 *str, ptrdiff_t len)
{
  if (s->s->size_shift == 0) {
    if (find_magnitude1 (str, len) == 0) {
      string_build_mkspace (s, len, 0);
      convert_1_to_0 (STR0(s->s) + s->s->len, str, len);
      s->s->len += len;
      return;
    }
    s->known_shift = 1;
  }

  string_build_mkspace (s, len, 1);
  if (s->s->size_shift == 1)
    convert_1_to_1 (STR1(s->s)+s->s->len, str, len);
  else {
#ifdef PIKE_DEBUG
    if (s->s->size_shift != 2)
      Pike_fatal ("I aint got no clue 'bout nothing, dude. (%d)\n",
		  s->s->size_shift);
#endif
    convert_1_to_2 (STR2(s->s)+s->s->len, str, len);
  }
  s->s->len += len;
}

PMOD_EXPORT void string_builder_binary_strcat2(struct string_builder *s,
					       const p_wchar2 *str, ptrdiff_t len)
{
  if (s->s->size_shift < 2) {
    int shift = find_magnitude2 (str, len);

    if (shift > s->s->size_shift) {
      string_build_mkspace (s, len, shift);
      if (shift == 1)
	convert_2_to_1 (STR1(s->s) + s->s->len, str, len);
      else {
#ifdef PIKE_DEBUG
	if (shift != 2) Pike_fatal ("Uhh.. Like, what? (%d)\n", shift);
#endif
	convert_2_to_2 (STR2(s->s) + s->s->len, str, len);
      }
      s->known_shift = shift;
    }

    else {
      string_build_mkspace (s, len, 0);
      if (s->s->size_shift == 0)
	convert_2_to_0 (STR0(s->s) + s->s->len, str, len);
      else {
#ifdef PIKE_DEBUG
	if (s->s->size_shift != 1)
	  Pike_fatal ("This is soo way bogus, man. (%d)\n", s->s->size_shift);
#endif
	convert_2_to_1 (STR1(s->s) + s->s->len, str, len);
      }
    }
  }

  else {
    string_build_mkspace (s, len, 2);
    convert_2_to_2 (STR2(s->s) + s->s->len, str, len);
  }

  s->s->len += len;
}

PMOD_EXPORT void string_builder_append(struct string_builder *s,
				       const PCHARP from,
				       ptrdiff_t len)
{
  int shift = from.shift;
  if (shift > s->s->size_shift) {
    if (shift == 1) {
      shift = find_magnitude1((p_wchar1 *)from.ptr, len);
    } else {
      shift = find_magnitude2((p_wchar2 *)from.ptr, len);
    }
    if (shift > s->known_shift)
      s->known_shift = shift;
  }
  string_build_mkspace(s, len, shift);
  generic_memcpy(MKPCHARP_STR_OFF(s->s,s->s->len), from, len);
  s->s->len+=len;
}

PMOD_EXPORT void string_builder_fill(struct string_builder *s,
				     ptrdiff_t howmany,
				     const PCHARP from,
				     ptrdiff_t len,
				     ptrdiff_t offset)
{
  ptrdiff_t tmp;
  int shift;

#ifdef PIKE_DEBUG
  if(len<=0)
    Pike_fatal("Cannot fill with zero length strings!\n");
#endif
  if(howmany<=0) return;

  if(!s->s->size_shift &&
     len == 1 &&
     (!from.shift || !min_magnitude(EXTRACT_PCHARP(from))))
  {
    memset(string_builder_allocate(s,howmany,0),
	   EXTRACT_PCHARP(from),
	   howmany);
    return;
  }

  if ((shift = from.shift) > s->s->size_shift) {
    /* Check if we really need the extra magnitude. */
    /* FIXME: What about offset? */
    if (shift == 1) {
      shift = find_magnitude1((p_wchar1 *)from.ptr, len);
    } else {
      shift = find_magnitude2((p_wchar2 *)from.ptr, len);
    }
  }

  string_build_mkspace(s, howmany, shift);
  tmp = MINIMUM(howmany, len - offset);

  generic_memcpy(MKPCHARP_STR_OFF(s->s,s->s->len),
		 ADD_PCHARP(from,offset),
		 tmp);
  s->s->len+=tmp;
  howmany-=tmp;
  if(howmany > 0)
  {
    PCHARP to;
    tmp=MINIMUM(howmany, len);
    to=MKPCHARP_STR_OFF(s->s,s->s->len);
    generic_memcpy(to,from, tmp);
    s->s->len+=tmp;
    howmany-=tmp;

    while(howmany > 0)
    {
      tmp = MINIMUM(len, howmany);
      memcpy(s->s->str + (s->s->len << s->s->size_shift),
	     to.ptr,
	     tmp << s->s->size_shift);
      len+=tmp;
      howmany-=tmp;
      s->s->len+=tmp;
    }
  }
}

/* Append a NUL-terminated UTF16 string possibly containing surrogates. */
PMOD_EXPORT void string_builder_utf16_strcat(struct string_builder *s,
					     const p_wchar1 *utf16str)
{
  p_wchar1 uc;
  while ((uc = *(utf16str++))) {
    if ((uc & 0xf800) == 0xd800) {
      /* Surrogate. */
      p_wchar2 wchar = uc & 0x03ff;
      if (!(uc & 0x0400)) {
	/* High order 10 bits. */
	wchar <<= 10;
      }
      uc = *(utf16str++);
      if (uc & 0x0400) {
	/* Low order 10 bits. */
	wchar |= (uc & 0x3ff);
      } else {
	/* High order 10 bits. */
	wchar |= (uc & 0x3ff) << 10;
      }
      string_builder_putchar(s, wchar + 0x00010000);
    } else {
      string_builder_putchar(s, uc);
    }
  }
}

PMOD_EXPORT void string_builder_strcat(struct string_builder *s, const char *str)
{
  string_builder_binary_strcat(s,str,strlen(str));
}

PMOD_EXPORT void string_builder_shared_strcat(struct string_builder *s,
                                              const struct pike_string *str)
{
  string_build_mkspace(s,str->len,str->size_shift);

  pike_string_cpy(MKPCHARP_STR_OFF(s->s,s->s->len), str);
  s->known_shift=MAXIMUM(s->known_shift,str->size_shift);
  s->s->len+=str->len;
}

PMOD_EXPORT ptrdiff_t string_builder_quote_string(struct string_builder *buf,
						  const struct pike_string *str,
						  ptrdiff_t i,
						  ptrdiff_t max_len,
						  int flags)
{
  ptrdiff_t old_len = buf->s->len;

  for (; i < str->len; i++) {
    p_wchar2 ch = index_shared_string(str, i);
    if (ch < 0 || ch > 0xffff) {
      /* Huge character. */
      string_builder_binary_strcat(buf, "\\U", 2);
      string_builder_append_integer(buf, (unsigned INT32)ch, 16, APPEND_ZERO_PAD, 8, 8);
    } else if (ch > 0xff) {
      /* Unicode character. */
      string_builder_binary_strcat(buf, "\\u", 2);
      string_builder_append_integer(buf, ch, 16, APPEND_ZERO_PAD, 4, 4);
    } else if (ch & 0x60) {
      /* Printable character or DEL. */
      if (ch == '\177') {
	/* DEL */
	goto ctrl_char;
      } else if ((ch == '"') || (ch == '\\')) {
	string_builder_putchar(buf, '\\');
      }
      string_builder_putchar(buf, ch);
    } else {
      p_wchar2 next_ch;
    ctrl_char:
      /* Control character. */
      string_builder_putchar(buf, '\\');
      if ((ch > 6) && (ch < 14)) {
	string_builder_putchar(buf, "0123456abtnvfr"[ch]);
	if ((ch == 10) && (flags & QUOTE_BREAK_AT_LF)) {
	  if (buf->s->len > max_len) {
	    /* Too bad; no place for the lf. */
	    buf->s->len = old_len;
	    return i;
	  }
	  return i+1;
	}
	goto next;
      }
      if (ch == 27) {
	string_builder_putchar(buf, 'e');
	goto next;
      }
      /* Check if we can use an octal escape. */
      if ((i+1 < str->len) &&
	  ((next_ch = index_shared_string(str, i+1)) >= '0') &&
	  (next_ch <= '7')) {
	/* No. */
	if (flags & QUOTE_NO_STRING_CONCAT) {
	  string_builder_putchar(buf, 'u');
	  string_builder_append_integer(buf, ch, 16, APPEND_ZERO_PAD, 4, 4);
	} else {
	  string_builder_append_integer(buf, ch, 8, 0, 1, 1);
	  string_builder_binary_strcat(buf, "\"\"", 2);
	}
	goto next;
      }
      string_builder_append_integer(buf, ch, 8, 0, 1, 1);
    }
  next:
    if (buf->s->len > max_len) {
      buf->s->len = old_len;
      return i;
    }
    old_len = buf->s->len;
  }
  return i;
}

PMOD_EXPORT void string_builder_append_integer(struct string_builder *s,
					       LONGEST val,
					       unsigned int base,
					       int flags,
					       size_t min_width,
					       size_t precision)
{
  unsigned LONGEST tmp;
  size_t len = 1;
  const char *numbers = "0123456789abcdef";
  if ((base < 2) || (base > 16)) {
    Pike_fatal("string_builder_append_int(): Unsupported base %u.\n", base);
  }
  if (flags & APPEND_UPPER_CASE) {
    numbers = "0123456789ABCDEF";
  }
  if ((flags & APPEND_SIGNED) && (val < 0)) {
    string_builder_putchar(s, '-');
    val = -val;
  } else if (flags & APPEND_POSITIVE) {
    string_builder_putchar(s, '+');
  }
  if ((flags & APPEND_ZERO_PAD) && (precision < min_width)) {
    precision = min_width;
  }

  tmp = val;
  if (base & (base - 1)) {
    size_t cnt;
    /* Calculate the output length.
     * Use do-while to ensure that zero isn't output as an empty string.
     */
    len = 0;
    do {
      len++;
      tmp /= base;
    } while (tmp);

    /* Precision is minimum number of digits. */
    if (len < precision) len = precision;

    /* Perform padding. */
    if (!(flags & APPEND_LEFT)) {
      if (len < min_width) {
	string_builder_fill(s, min_width - len, MKPCHARP("    ", 0),
			    4, 0);
      }
      min_width = 0;
    }

    cnt = len;

    tmp = val;
    switch(s->s->size_shift) {
    case 0:
      {
	p_wchar0 *p = string_builder_allocate(s, len, 0);
	do {
	  p[--cnt] = numbers[tmp%base];
	  tmp /= base;
	} while (cnt);
      }
      break;
    case 1:
      {
	p_wchar1 *p = string_builder_allocate(s, len, 0);
	do {
	  p[--cnt] = numbers[tmp%base];
	  tmp /= base;
	} while (cnt);
      }
      break;
    case 2:
      {
	p_wchar2 *p = string_builder_allocate(s, len, 0);
	do {
	  p[--cnt] = numbers[tmp%base];
	  tmp /= base;
	} while (cnt);
      }
      break;
    }
  } else {
    /* base is a power of two, so we can do without
     * the division and modulo operations.
     */
    int delta;
    size_t shift;
    unsigned int mask;

    for(delta = 1; (base>>delta) > 1; delta++)
      ;

    mask = (1<<delta)-1;	/* Usually base-1. */

    /* Precision is minimum number of digits. */
    if (precision) shift = (len = precision) * delta;
    else shift = delta;

    /* Calculate actual number of digits and initial shift. */
    for (; shift < SIZEOF_LONGEST * 8 && tmp >> shift; shift += delta, len++)
      ;

    if ((len < min_width) && !(flags & APPEND_LEFT)) {
      /* Perform padding.
       * Note that APPEND_ZERO_PAD can not be active here, since
       * len is at least min_width in that case.
       */
      string_builder_fill(s, min_width - len, MKPCHARP("    ", 0),
			  4, 0);
      min_width = 0;
    }

    while(shift) {
      shift -= delta;
      string_builder_putchar(s, numbers[(tmp>>shift) & mask]);
    }
  }
  if (len < min_width) {
    /* Perform padding.
     * Note that APPEND_ZERO_PAD can not be active here, since
     * len is at least min_width in that case.
     * Note that APPEND_LEFT is always active here, since
     * min_width isn't zero.
     */
    string_builder_fill(s, min_width - len, MKPCHARP("    ", 0),
			4, 0);
  }
}

/* Kludge around brokeness of gcc/x86_64 */
#ifdef VA_LIST_IS_STATE_PTR
#define VA_LIST_PTR		va_list
#define VA_LIST_ADDR(X)		(X)
#define VA_LIST_DEREF(X)	(X)
#else
#define VA_LIST_PTR		va_list *
#define VA_LIST_ADDR(X)		(&(X))
#define VA_LIST_DEREF(X)	(*(X))
#endif

static LONGEST pike_va_int(VA_LIST_PTR args, int flags)
{
  switch (flags & (APPEND_WIDTH_MASK|APPEND_SIGNED)) {
  case APPEND_WIDTH_HALF:
    return va_arg(VA_LIST_DEREF(args), unsigned int) & 0xffff;
  case APPEND_WIDTH_HALF|APPEND_SIGNED:
    return (short)va_arg(VA_LIST_DEREF(args), int);
  case 0:
    return va_arg(VA_LIST_DEREF(args), unsigned int);
  case APPEND_SIGNED:
    return va_arg(VA_LIST_DEREF(args), int);
  case APPEND_WIDTH_LONG:
    return va_arg(VA_LIST_DEREF(args), unsigned long);
  case APPEND_WIDTH_LONG|APPEND_SIGNED:
    return va_arg(VA_LIST_DEREF(args), long);
#ifdef INT64
  case APPEND_WIDTH_LONG_LONG:
    return va_arg(VA_LIST_DEREF(args), unsigned INT64);
  case APPEND_WIDTH_LONG_LONG|APPEND_SIGNED:
    return va_arg(VA_LIST_DEREF(args), INT64);
#endif /* INT64 */
  }
  Pike_fatal("string_builder_append_integerv(): Unsupported flags: 0x%04x\n",
	     flags);
  return 0;
}

/* Values used internally in string_builder_vsprintf() */
#define STATE_MIN_WIDTH	1
#define STATE_PRECISION 2

PMOD_EXPORT void string_builder_vsprintf(struct string_builder *s,
					 const char *fmt,
					 va_list args)
{
  while (*fmt) {
    if (*fmt == '%') {
      int flags = 0;
      size_t min_width = 0;
      size_t precision = 0;
      int state = 0;
      
      fmt++;
      while (1) {
	switch (*(fmt++)) {
	case '%':
	  string_builder_putchar(s, '%');
	  break;

	case '+':
	  flags |= APPEND_POSITIVE;
	  continue;
	case '-':
	  flags |= APPEND_LEFT;
	  continue;

	case '0':
	  if (!state) {
	    flags |= APPEND_ZERO_PAD;
	  }
	  /* FALL_THROUGH */
	case '1': case '2': case '3':
	case '4': case '5': case '6':
	case '7': case '8': case '9':
	  if (state == STATE_PRECISION) {
	    precision = precision * 10 + fmt[-1] - '0';
	  } else {
	    state = STATE_MIN_WIDTH;
	    min_width = min_width * 10 + fmt[-1] - '0';
	  }
	  continue;

	case '.':
	  state = STATE_PRECISION;
	  continue;

	case 'h':
	  flags |= APPEND_WIDTH_HALF;
	  continue;

	case 'w':	/* Same as l, but old-style, and only for %s. */
	case 'l':
	  if (flags & APPEND_WIDTH_LONG) {
	    flags |= APPEND_WIDTH_LONG_LONG;
	  } else {
	    flags |= APPEND_WIDTH_LONG;
	  }
	  continue;

	case 't':	/* ptrdiff_t */
	case 'z':	/* size_t */
	  flags = (flags & ~APPEND_WIDTH_MASK) | APPEND_WIDTH_PTR;
	  continue;
	  
	case 'O':
	  {
	    /* FIXME: Doesn't care about field or integer widths yet. */
	    dynamic_buffer old_buf;
	    init_buf(&old_buf);
	    describe_svalue(va_arg(args, struct svalue *), 0, NULL);
	    string_builder_binary_strcat(s, pike_global_buffer.s.str,
					 pike_global_buffer.s.len);
	    toss_buffer(&pike_global_buffer);
	    restore_buffer(&old_buf);
	  }
	  break;
	case 'S':
	  /* Note: On some platforms this is an alias for %ls, so if you
	   *       want to output wchar_t strings, use %ls instead!
	   */
	  {
	    struct pike_string *str = va_arg(args, struct pike_string *);
	    size_t len = str->len;
	    if (precision && (precision < len)) len = precision;
	    if (min_width > len) {
	      if (flags & APPEND_LEFT) {
		string_builder_append(s, MKPCHARP_STR(str), len);
		string_builder_fill(s, min_width - len, MKPCHARP("    ", 0),
				    4, 0);
	      } else {
		string_builder_fill(s, min_width - len, MKPCHARP("    ", 0),
				    4, 0);
		string_builder_append(s, MKPCHARP_STR(str), len);
	      }
	    } else {
	      string_builder_append(s, MKPCHARP_STR(str), len);
	    }
	  }
	  break;
	case 's':
	  if (flags & APPEND_WIDTH_LONG) {
	    /* Wide string: %ws, %ls or %lls
	     */
	    PCHARP str;
	    size_t len;
	    if ((flags & APPEND_WIDTH_LONG)== APPEND_WIDTH_LONG) {
	      str = MKPCHARP(va_arg(args, p_wchar1 *), 1);
	    } else {
	      str = MKPCHARP(va_arg(args, p_wchar2 *), 2);
	    }
	    len = pcharp_strlen(str);
	    if (precision && precision < len) len = precision;
	    if (min_width > len) {
	      if (flags & APPEND_LEFT) {
		string_builder_append(s, str, len);
		string_builder_fill(s, min_width - len, MKPCHARP("    ", 0),
				    4, 0);
	      } else {
		string_builder_fill(s, min_width - len, MKPCHARP("    ", 0),
				    4, 0);
		string_builder_append(s, str, len);
	      }
	    } else {
	      string_builder_append(s, str, len);
	    }
	  } else {
	    const char *str = va_arg(args, char *);
	    size_t len = strlen(str);
	    if (precision && precision < len) len = precision;
	    if (min_width > len) {
	      if (flags & APPEND_LEFT) {
		string_builder_binary_strcat(s, str, len);
		string_builder_fill(s, min_width - len, MKPCHARP("    ", 0),
				    4, 0);
	      } else {
		string_builder_fill(s, min_width - len, MKPCHARP("    ", 0),
				    4, 0);
		string_builder_binary_strcat(s, str, len);
	      }
	    } else {
	      string_builder_binary_strcat(s, str, len);
	    }
	  }
	  break;
	case 'c':
	  /* FIXME: Doesn't care about field or integer widths yet. */
	  string_builder_putchar(s, va_arg(args, int));
	  break;
	case 'b':
	  string_builder_append_integer(s, pike_va_int(VA_LIST_ADDR(args), flags), 2,
					flags, min_width, precision);
	  break;
	case 'o':
	  string_builder_append_integer(s, pike_va_int(VA_LIST_ADDR(args), flags), 8,
					flags, min_width, precision);
	  break;
	case 'x':
	  string_builder_append_integer(s, pike_va_int(VA_LIST_ADDR(args), flags), 16,
					flags, min_width, precision);
	  break;
	case 'X':
	  string_builder_append_integer(s, pike_va_int(VA_LIST_ADDR(args), flags), 16,
					flags | APPEND_UPPER_CASE,
					min_width, precision);
	  break;
	case 'u':
	  string_builder_append_integer(s, pike_va_int(VA_LIST_ADDR(args), flags), 10,
					flags, min_width, precision);
	  break;
	case 'd':
	  flags |= APPEND_SIGNED;
	  string_builder_append_integer(s, pike_va_int(VA_LIST_ADDR(args), flags), 10,
					flags, min_width, precision);
	  break;

	  /* %f used in modules/Image/colors.c. */
	case 'a':
	case 'e':
	case 'E':
	case 'f':
	case 'g':
	case 'G':
	  {
	    double val = va_arg(args, double);
	    size_t bytes;
	    char nfmt[] = { '%', fmt[-1], 0 };

	    if (PIKE_ISNAN(val)) {
	      /* NaN */
	      string_builder_strcat(s, "nan");
	      break;
	    }
	    if (val < 0.0) {
	      string_builder_putchar(s, '-');
	      val = -val;
	    } else if (flags & APPEND_SIGNED) {
	      string_builder_putchar(s, '+');
	    }
	    if ((val+val == val) && (val > 0.0)) {
	      /* Infinity */
	      string_builder_strcat(s, "inf");
	      break;
	    }
	    /* FIXME: Field lengths and precision. */
	    if ((bytes = snprintf(NULL, 0, nfmt, val))) {
	      p_wchar0 *p = string_builder_allocate(s, bytes, 0);
	      size_t check = snprintf((char*)p, bytes+1, nfmt, val);
	      if (check != bytes) {
		Pike_fatal("string_builder_vsprintf(): snprintf(\"%s\", %f) "
			   "is not trustworthy: "
			   "%"PRINTSIZET"u != %"PRINTSIZET"u\n",
			   nfmt, val, bytes, check);
	      }
	      if (s->s->size_shift) {
		/* We need to widen the string we just wrote. */
		if (s->s->size_shift == 1) {
		  p_wchar1 *p1 = (p_wchar1 *)p;
		  while (bytes--) {
		    p1[bytes] = p[bytes];
		  }
		} else {
		  p_wchar2 *p2 = (p_wchar2 *)p;
		  while (bytes--) {
		    p2[bytes] = p[bytes];
		  }
		}
	      }
	    }
	  }
	  break;

	default:
	  Pike_fatal("string_builder_vsprintf(): Invalid formatting method: "
		     "\"%%%c\" 0x%x.\n", (fmt[-1] & 0xff), fmt[-1]);
	}
	break;
      }
    } else {
      const char *start = fmt;
      while (*fmt && (*fmt != '%'))
	fmt++;
      string_builder_binary_strcat(s, start, fmt-start);
    }
  }
}


PMOD_EXPORT void string_builder_sprintf(struct string_builder *s,
					const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  string_builder_vsprintf(s, fmt, args);
  va_end(args);
}


PMOD_EXPORT void reset_string_builder(struct string_builder *s)
{
  s->known_shift=0;
  s->s->len=0;
}

PMOD_EXPORT void free_string_builder(struct string_builder *s)
{
  s->s->len = s->malloced;
  free_string(s->s);
}

PMOD_EXPORT struct pike_string *finish_string_builder(struct string_builder *s)
{
  ptrdiff_t len = s->s->len;
  if (len != s->malloced) {
    s->s->len = s->malloced;	/* Restore the allocated length. */
    s->s = realloc_unlinked_string(s->s, len);
  }
  if(s->known_shift == s->s->size_shift)
    return low_end_shared_string(s->s);
  return end_shared_string(s->s);
}

PMOD_EXPORT PCHARP MEMCHR_PCHARP(const PCHARP ptr, int chr, ptrdiff_t len)
{
  switch(ptr.shift)
  {
    case 0: return MKPCHARP(memchr(ptr.ptr,chr,len),0);
    case 1: return MKPCHARP(MEMCHR1((p_wchar1 *)ptr.ptr,chr,len),1);
    case 2: return MKPCHARP(MEMCHR2((p_wchar2 *)ptr.ptr,chr,len),2);
  }
#ifdef PIKE_DEBUG
  Pike_fatal("Illegal shift in MEMCHR_PCHARP.\n");
#endif
  return MKPCHARP(0,0); /* make wcc happy */
}

#define DIGIT(x)	(WIDE_ISDIGIT(x) ? (x) - '0' : \
			 WIDE_ISLOWER(x) ? (x) + 10 - 'a' : (x) + 10 - 'A')
#define MBASE	('z' - 'a' + 1 + 10)

PMOD_EXPORT long STRTOL_PCHARP(PCHARP str, PCHARP *ptr, int base)
{
  /* Note: Code duplication in strtol and pcharp_to_svalue_inumber. */

  unsigned long val, mul_limit;
  int c;
  int xx, neg = 0, add_limit, overflow = 0;

  if (ptr)  *ptr = str;
  if (base < 0 || base > MBASE)  return 0;
  if (!WIDE_ISALNUM(c = EXTRACT_PCHARP(str)))
  {
    while (WIDE_ISSPACE(c))
    {
      INC_PCHARP(str,1);
      c=EXTRACT_PCHARP(str);
    }
    switch (c)
    {
    case '-':
      neg++;
      /* FALL_THROUGH */
    case '+':
      INC_PCHARP(str,1);
      c=EXTRACT_PCHARP(str);
    }
  }

  if (!base)
  {
    if (c != '0')
      base = 10;
    else if (INDEX_PCHARP(str,1) == 'x' || INDEX_PCHARP(str,1) == 'X')
      base = 16;
    else
      base = 8;
  }

  if (!WIDE_ISALNUM(c) || (xx = DIGIT(c)) >= base)
    return 0;			/* no number formed */
  if (base == 16 && c == '0' && isxdigit(INDEX_PCHARP(str,2)) &&
      (INDEX_PCHARP(str,1) == 'x' || INDEX_PCHARP(str,1) == 'X'))
  {
    INC_PCHARP(str,2);
    c = EXTRACT_PCHARP(str);		/* skip over leading "0x" or "0X" */
  }

  mul_limit = ((unsigned long)LONG_MAX)/base;
  add_limit = (int) (LONG_MAX % base);
  if (neg) {
    if (++add_limit == base) {
      add_limit = 0;
      mul_limit++;
    }
  }

  val=DIGIT(c);
  while(1)
  {
    INC_PCHARP(str,1);
    c=EXTRACT_PCHARP(str);
    if(!(WIDE_ISALNUM(c)  && (xx=DIGIT(c)) < base)) break;
    if (val > mul_limit || (val == mul_limit && xx > add_limit)) {
      overflow = 1;
    } else
      val = base * val + xx;
  }

  if (ptr) *ptr = str;
  if (overflow) {
    errno = ERANGE;
    return neg ? LONG_MIN : LONG_MAX;
  }
  else {
    if (neg)
      return (long)(~val + 1);
    else
      return (long) val;
  }
}

int wide_string_to_svalue_inumber(struct svalue *r,
                                  void * str,
                                  void *ptr,
                                  int base,
                                  ptrdiff_t maxlength,
                                  int shift)
{
  PCHARP tmp;
  int ret=pcharp_to_svalue_inumber(r,
				   MKPCHARP(str,shift),
				   &tmp,
				   base,
				   maxlength);
  if(ptr) *(p_wchar0 **)ptr=tmp.ptr;
  return ret;
}

int safe_wide_string_to_svalue_inumber(struct svalue *r,
				       void * str,
				       void *ptr,
				       int base,
				       ptrdiff_t maxlength,
				       int shift)
/* For use from the lexer where we can't let errors be thrown. */
{
  PCHARP tmp;
  JMP_BUF recovery;
  int ret = 0;

  free_svalue (&throw_value);
  mark_free_svalue (&throw_value);

  if (SETJMP (recovery)) {
    /* We know that pcharp_to_svalue_inumber has initialized the
     * svalue before any error might be thrown. */
    call_handle_error();
    ret = 0;
  }
  else
    ret = pcharp_to_svalue_inumber(r,
				   MKPCHARP(str,shift),
				   &tmp,
				   base,
				   maxlength);
  UNSETJMP (recovery);
  if(ptr) *(p_wchar0 **)ptr=tmp.ptr;
  return ret;
}

PMOD_EXPORT int pcharp_to_svalue_inumber(struct svalue *r,
					 PCHARP str,
					 PCHARP *ptr,
					 int base,
					 ptrdiff_t maxlength)
{
  /* Note: Code duplication in strtol and STRTOL_PCHARP. */

  PCHARP str_start;
  
  unsigned INT_TYPE val, mul_limit;
  int c;
  int xx, neg = 0, add_limit, overflow = 0;

  maxlength--;   /* max_length <= 0 means no max length. */
  str_start = str;

  /* In case no number is formed. */
  SET_SVAL(*r, T_INT, NUMBER_NUMBER, integer, 0);
  if(ptr != 0)
    *ptr = str;
  
  if(base < 0 || MBASE < base)
    return 0;
  
  if(!WIDE_ISALNUM(c = EXTRACT_PCHARP(str)))
  {
    while(WIDE_ISSPACE(c))
    {
      INC_PCHARP(str,1);
      c = EXTRACT_PCHARP(str);
    }
    
    switch (c)
    {
    case '-':
      neg++;
      /* Fall-through. */
    case '+':
      INC_PCHARP(str,1);
      c = EXTRACT_PCHARP(str);
    }
  }
  
  if(base == 0)
  {
    if(c != '0')
      base = 10;
    else if(INDEX_PCHARP(str,1) == 'x' || INDEX_PCHARP(str,1) == 'X')
      base = 16;
    else if(INDEX_PCHARP(str,1) == 'b' || INDEX_PCHARP(str,1) == 'B')
      base = 2;
    else
      base = 8;
  }
  
  /*
   * For any base > 10, the digits incrementally following
   * 9 are assumed to be "abc...z" or "ABC...Z".
   */
  if(!WIDE_ISALNUM(c) || (xx = DIGIT(c)) >= base)
    return 0;   /* No number formed. */

  if(c == '0' &&
     ((base==16 && (INDEX_PCHARP(str,1)=='x' || INDEX_PCHARP(str,1)=='X')) ||
      (base==2 && (INDEX_PCHARP(str,1)=='b' || INDEX_PCHARP(str,1)=='B'))) &&
     INDEX_PCHARP(str,2) < 256 && /* Don't trust isxdigit... */
     isxdigit(INDEX_PCHARP(str,2)))
  {
    /* Skip over leading "0x", "0X", "0b" or "0B". */
    INC_PCHARP(str,2);
    c=EXTRACT_PCHARP(str);
  }
  str_start=str;

  if (neg) {
    mul_limit = (unsigned INT_TYPE) MIN_INT_TYPE / base;
    add_limit = (int) ((unsigned INT_TYPE) MIN_INT_TYPE % base);
  }
  else {
    mul_limit = MAX_INT_TYPE / base;
    add_limit = (int) (MAX_INT_TYPE % base);
  }
  
  for(val = DIGIT(c);
      (INC_PCHARP(str,1), WIDE_ISALNUM(c = EXTRACT_PCHARP(str) )) &&
	(xx = DIGIT(c)) < base &&
	0 != maxlength--; )
  {
    if (val > mul_limit || (val == mul_limit && xx > add_limit))
      overflow = 1;
    else
      val = base * val + xx;
  }
  
  if(ptr != 0)
    *ptr = str;

  if (overflow) {
    push_string(make_shared_binary_pcharp(str_start,
					  SUBTRACT_PCHARP(str,str_start)));
    /* Note that this can conceivably throw errors()
     * in some situations that might not be desirable...
     * take care.
     * /Hubbe
     *
     * It could probably also be faster...
     */
    push_int(base);
    convert_stack_top_with_base_to_bignum();
    if(neg) o_negate();
    
    *r = *--Pike_sp;
    dmalloc_touch_svalue (r);
  }
  else {
    if (neg)
      r->u.integer = val > (unsigned INT_TYPE) MAX_INT_TYPE ?
	-(INT_TYPE) (val - (unsigned INT_TYPE) MAX_INT_TYPE) - MAX_INT_TYPE :
	-(INT_TYPE) val;
    else
      r->u.integer = (INT_TYPE) val;
  }

  return 1;
}

PMOD_EXPORT int convert_stack_top_string_to_inumber(int base)
{
  struct svalue r;
  int i;

  if(TYPEOF(Pike_sp[-1]) != T_STRING)
    Pike_error("Cannot convert stack top to integer number.\n");
  
  i=pcharp_to_svalue_inumber(&r, MKPCHARP_STR(Pike_sp[-1].u.string), 0, base, 0);
  
  free_string(Pike_sp[-1].u.string);
  Pike_sp[-1] = r;

  return i;
}

/* Convert PCHARP to a double.  If ENDPTR is not NULL, a pointer to the
   character after the last one used in the number is put in *ENDPTR.  */
PMOD_EXPORT double STRTOD_PCHARP(const PCHARP nptr, PCHARP *endptr)
{
  /* Note: Code duplication in strtod. */

  PCHARP s;
  short int sign;

  /* The number so far.  */
  double num;

  int got_dot;      /* Found a decimal point.  */
  int got_digit;    /* Seen any digits.  */

  /* The exponent of the number.  */
  long int exponent;

  if (nptr.ptr == NULL)
  {
    errno = EINVAL;
    goto noconv;
  }

  s = nptr;

  /* Eat whitespace.  */
  while (EXTRACT_PCHARP(s) <256 && WIDE_ISSPACE(EXTRACT_PCHARP(s))) INC_PCHARP(s,1);

  /* Get the sign.  */
  sign = EXTRACT_PCHARP(s) == '-' ? -1 : 1;
  if (EXTRACT_PCHARP(s) == '-' || EXTRACT_PCHARP(s) == '+')
    INC_PCHARP(s,1);

  num = 0.0;
  got_dot = 0;
  got_digit = 0;
  exponent = 0;
  for (;; INC_PCHARP(s,1))
  {
    if (EXTRACT_PCHARP(s)<256 && WIDE_ISDIGIT (EXTRACT_PCHARP(s)))
    {
      got_digit = 1;

      /* Make sure that multiplication by 10 will not overflow.  */
      if (num > DBL_MAX * 0.1)
	/* The value of the digit doesn't matter, since we have already
	   gotten as many digits as can be represented in a `double'.
	   This doesn't necessarily mean the result will overflow.
	   The exponent may reduce it to within range.
	   
	   We just need to record that there was another
	   digit so that we can multiply by 10 later.  */
	++exponent;
      else
	num = (num * 10.0) + (EXTRACT_PCHARP(s) - '0');

      /* Keep track of the number of digits after the decimal point.
	 If we just divided by 10 here, we would lose precision.  */
      if (got_dot)
	--exponent;
    }
    else if (!got_dot && (char) EXTRACT_PCHARP(s) == '.')
      /* Record that we have found the decimal point.  */
      got_dot = 1;
    else
      /* Any other character terminates the number.  */
      break;
  }

  if (!got_digit)
    goto noconv;

  if (EXTRACT_PCHARP(s) <256 && tolower(EXTRACT_PCHARP(s)) == 'e')
    {
      /* Get the exponent specified after the `e' or `E'.  */
      int save = errno;
      PCHARP end;
      long int exp;

      errno = 0;
      INC_PCHARP(s,1);
      exp = STRTOL_PCHARP(s, &end, 10);
      if (errno == ERANGE)
      {
	/* The exponent overflowed a `long int'.  It is probably a safe
	   assumption that an exponent that cannot be represented by
	   a `long int' exceeds the limits of a `double'.  */
	/* NOTE: Don't trust the value returned from strtol.
	 * We need to find the sign of the exponent by hand.
	 */
	p_wchar2 c;
	while(WIDE_ISSPACE(c = EXTRACT_PCHARP(s))) {
	  INC_PCHARP(s, 1);
	}
	if (endptr != NULL)
	  *endptr = end;
	if (c == '-')
	  goto underflow;
	else
	  goto overflow;
      }
      else if (COMPARE_PCHARP(end,==,s))
	/* There was no exponent.  Reset END to point to
	   the 'e' or 'E', so *ENDPTR will be set there.  */
	end = ADD_PCHARP(s,-1);
      errno = save;
      s = end;
      exponent += exp;
    }

  if(got_dot && INDEX_PCHARP(s,-1)=='.') INC_PCHARP(s,-1);
  if (endptr != NULL)
    *endptr = s;

  if (num == 0.0)
    return 0.0;

  /* Multiply NUM by 10 to the EXPONENT power,
     checking for overflow and underflow.  */

  if (exponent < 0)
  {
    if (num < DBL_MIN * pow(10.0, (double) -exponent))
      goto underflow;
  }
  else if (exponent > 0)
  {
    if (num > DBL_MAX * pow(10.0, (double) -exponent))
      goto overflow;
  }

  if(exponent < 0 && exponent >-100) /* make sure we don't underflow */
    num /= pow(10.0, (double) -exponent);
  else
    num *= pow(10.0, (double) exponent);

  return num * sign;

 overflow:
  /* Return an overflow error.  */
  errno = ERANGE;
  return HUGE_VAL * sign;

 underflow:
  /* Return an underflow error.  */
  errno = ERANGE;
  return 0.0;
  
 noconv:
  /* There was no number.  */
  if (endptr != NULL)
    *endptr = nptr;
  return 0.0;
}


PMOD_EXPORT p_wchar0 *require_wstring0(const struct pike_string *s,
                                       char **to_free)
{
  switch(s->size_shift)
  {
    case 0:
      *to_free=0;
      return STR0(s);
    case 1:
    case 2:
      return 0;

#ifdef PIKE_DEBUG
    default:
      Pike_fatal("Illegal shift size in string.\n");
#endif
  }
  return 0;
}

PMOD_EXPORT p_wchar1 *require_wstring1(const struct pike_string *s,
                                       char **to_free)
{
  switch(s->size_shift)
  {
    case 0:
      *to_free=xalloc((s->len+1)*2);
      convert_0_to_1((p_wchar1 *)*to_free, STR0(s),s->len+1);
      return (p_wchar1 *)*to_free;

    case 1:
      *to_free=0;
      return STR1(s);

    case 2:
      return 0;

#ifdef PIKE_DEBUG
    default:
      Pike_fatal("Illegal shift size in string.\n");
#endif
  }
  return 0;
}


PMOD_EXPORT p_wchar2 *require_wstring2(const struct pike_string *s,
                                       char **to_free)
{
  switch(s->size_shift)
  {
    case 0:
      *to_free=xalloc((s->len+1)*4);
      convert_0_to_2((p_wchar2 *)*to_free, STR0(s),s->len+1);
      return (p_wchar2 *)*to_free;

    case 1:
      *to_free=xalloc((s->len+1)*4);
      convert_1_to_2((p_wchar2 *)*to_free, STR1(s),s->len+1);
      return (p_wchar2 *)*to_free;

    case 2:
      *to_free=0;
      return STR2(s);

#ifdef PIKE_DEBUG
    default:
      Pike_fatal("Illegal shift size in string.\n");
#endif
  }
  return 0;
}

/*-------------------------------------------------------------------------
 *
 * tsvector_op.c
 *	  operations over tsvector
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsvector_op.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"


typedef struct
{
	WordEntry  *arrb;
	WordEntry  *arre;
	char	   *values;
	char	   *operand;
} CHKVAL;


typedef struct StatEntry
{
	uint32		ndoc;			/* zero indicates that we were already here
								 * while walking through the tree */
	uint32		nentry;
	struct StatEntry *left;
	struct StatEntry *right;
	uint32		lenlexeme;
	char		lexeme[FLEXIBLE_ARRAY_MEMBER];
} StatEntry;

#define STATENTRYHDRSZ	(offsetof(StatEntry, lexeme))

typedef struct
{
	int32		weight;

	uint32		maxdepth;

	StatEntry **stack;
	uint32		stackpos;

	StatEntry  *root;
} TSVectorStat;

#define STATHDRSIZE (offsetof(TSVectorStat, data))

static Datum tsvector_update_trigger(PG_FUNCTION_ARGS, bool config_column);
static int tsvector_bsearch(const TSVector tsv, char *lexeme, int lexeme_len);

/*
 * Order: haspos, len, word, for all positions (pos, weight)
 */
static int
silly_cmp_tsvector(const TSVector a, const TSVector b)
{
	if (VARSIZE(a) < VARSIZE(b))
		return -1;
	else if (VARSIZE(a) > VARSIZE(b))
		return 1;
	else if (a->size < b->size)
		return -1;
	else if (a->size > b->size)
		return 1;
	else
	{
		WordEntry  *aptr = ARRPTR(a);
		WordEntry  *bptr = ARRPTR(b);
		int			i = 0;
		int			res;


		for (i = 0; i < a->size; i++)
		{
			if (aptr->haspos != bptr->haspos)
			{
				return (aptr->haspos > bptr->haspos) ? -1 : 1;
			}
			else if ((res = tsCompareString(STRPTR(a) + aptr->pos, aptr->len, STRPTR(b) + bptr->pos, bptr->len, false)) != 0)
			{
				return res;
			}
			else if (aptr->haspos)
			{
				WordEntryPos *ap = POSDATAPTR(a, aptr);
				WordEntryPos *bp = POSDATAPTR(b, bptr);
				int			j;

				if (POSDATALEN(a, aptr) != POSDATALEN(b, bptr))
					return (POSDATALEN(a, aptr) > POSDATALEN(b, bptr)) ? -1 : 1;

				for (j = 0; j < POSDATALEN(a, aptr); j++)
				{
					if (WEP_GETPOS(*ap) != WEP_GETPOS(*bp))
					{
						return (WEP_GETPOS(*ap) > WEP_GETPOS(*bp)) ? -1 : 1;
					}
					else if (WEP_GETWEIGHT(*ap) != WEP_GETWEIGHT(*bp))
					{
						return (WEP_GETWEIGHT(*ap) > WEP_GETWEIGHT(*bp)) ? -1 : 1;
					}
					ap++, bp++;
				}
			}

			aptr++;
			bptr++;
		}
	}

	return 0;
}

#define TSVECTORCMPFUNC( type, action, ret )			\
Datum													\
tsvector_##type(PG_FUNCTION_ARGS)						\
{														\
	TSVector	a = PG_GETARG_TSVECTOR(0);				\
	TSVector	b = PG_GETARG_TSVECTOR(1);				\
	int			res = silly_cmp_tsvector(a, b);			\
	PG_FREE_IF_COPY(a,0);								\
	PG_FREE_IF_COPY(b,1);								\
	PG_RETURN_##ret( res action 0 );					\
}	\
/* keep compiler quiet - no extra ; */					\
extern int no_such_variable

TSVECTORCMPFUNC(lt, <, BOOL);
TSVECTORCMPFUNC(le, <=, BOOL);
TSVECTORCMPFUNC(eq, ==, BOOL);
TSVECTORCMPFUNC(ge, >=, BOOL);
TSVECTORCMPFUNC(gt, >, BOOL);
TSVECTORCMPFUNC(ne, !=, BOOL);
TSVECTORCMPFUNC(cmp, +, INT32);

Datum
tsvector_strip(PG_FUNCTION_ARGS)
{
	TSVector	in = PG_GETARG_TSVECTOR(0);
	TSVector	out;
	int			i,
				len = 0;
	WordEntry  *arrin = ARRPTR(in),
			   *arrout;
	char	   *cur;

	for (i = 0; i < in->size; i++)
		len += arrin[i].len;

	len = CALCDATASIZE(in->size, len);
	out = (TSVector) palloc0(len);
	SET_VARSIZE(out, len);
	out->size = in->size;
	arrout = ARRPTR(out);
	cur = STRPTR(out);
	for (i = 0; i < in->size; i++)
	{
		memcpy(cur, STRPTR(in) + arrin[i].pos, arrin[i].len);
		arrout[i].haspos = 0;
		arrout[i].len = arrin[i].len;
		arrout[i].pos = cur - STRPTR(out);
		cur += arrout[i].len;
	}

	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_POINTER(out);
}

Datum
tsvector_length(PG_FUNCTION_ARGS)
{
	TSVector	in = PG_GETARG_TSVECTOR(0);
	int32		ret = in->size;

	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_INT32(ret);
}

Datum
tsvector_setweight(PG_FUNCTION_ARGS)
{
	TSVector	in = PG_GETARG_TSVECTOR(0);
	char		cw = PG_GETARG_CHAR(1);
	TSVector	out;
	int			i,
				j;
	WordEntry  *entry;
	WordEntryPos *p;
	int			w = 0;

	switch (cw)
	{
		case 'A':
		case 'a':
			w = 3;
			break;
		case 'B':
		case 'b':
			w = 2;
			break;
		case 'C':
		case 'c':
			w = 1;
			break;
		case 'D':
		case 'd':
			w = 0;
			break;
		default:
			/* internal error */
			elog(ERROR, "unrecognized weight: %d", cw);
	}

	out = (TSVector) palloc(VARSIZE(in));
	memcpy(out, in, VARSIZE(in));
	entry = ARRPTR(out);
	i = out->size;
	while (i--)
	{
		if ((j = POSDATALEN(out, entry)) != 0)
		{
			p = POSDATAPTR(out, entry);
			while (j--)
			{
				WEP_SETWEIGHT(*p, w);
				p++;
			}
		}
		entry++;
	}

	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_POINTER(out);
}

/*
 * setweight(tsin tsvector, char_weight "char", lexemes "text"[])
 *
 * Assign weight w to elements of tsin that are listed in lexemes.
 */
Datum
tsvector_setweight_by_filter(PG_FUNCTION_ARGS)
{
	TSVector	tsin = PG_GETARG_TSVECTOR(0);
	char		char_weight = PG_GETARG_CHAR(1);
	ArrayType  *lexemes = PG_GETARG_ARRAYTYPE_P(2);

	TSVector	tsout;
	int			i,
				j,
				nlexemes,
				weight;
	WordEntry  *entry;
	Datum	   *dlexemes;
	bool	   *nulls;

	switch (char_weight)
	{
		case 'A': case 'a':
			weight = 3;
			break;
		case 'B': case 'b':
			weight = 2;
			break;
		case 'C': case 'c':
			weight = 1;
			break;
		case 'D': case 'd':
			weight = 0;
			break;
		default:
			/* internal error */
			elog(ERROR, "unrecognized weight: %c", char_weight);
	}

	tsout = (TSVector) palloc(VARSIZE(tsin));
	memcpy(tsout, tsin, VARSIZE(tsin));
	entry = ARRPTR(tsout);

	deconstruct_array(lexemes, TEXTOID, -1, false, 'i',
					  &dlexemes, &nulls, &nlexemes);

	/*
	 * Assuming that lexemes array is significantly shorter than tsvector
	 * we can iterate through lexemes performing binary search
	 * of each lexeme from lexemes in tsvector.
	 */
	for (i = 0; i < nlexemes; i++)
	{
		char   *lex;
		int		lex_len,
				lex_pos;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("lexeme array may not contain nulls")));

		lex = VARDATA(dlexemes[i]);
		lex_len = VARSIZE_ANY_EXHDR(dlexemes[i]);
		lex_pos = tsvector_bsearch(tsout, lex, lex_len);

		if (lex_pos >= 0 && (j = POSDATALEN(tsout, entry + lex_pos)) != 0)
		{
			WordEntryPos *p = POSDATAPTR(tsout, entry + lex_pos);
			while (j--)
			{
				WEP_SETWEIGHT(*p, weight);
				p++;
			}
		}
	}

	PG_FREE_IF_COPY(tsin, 0);
	PG_FREE_IF_COPY(lexemes, 2);

	PG_RETURN_POINTER(tsout);
}

#define compareEntry(pa, a, pb, b) \
	tsCompareString((pa) + (a)->pos, (a)->len,	\
					(pb) + (b)->pos, (b)->len,	\
					false)

/*
 * Add positions from src to dest after offsetting them by maxpos.
 * Return the number added (might be less than expected due to overflow)
 */
static int32
add_pos(TSVector src, WordEntry *srcptr,
		TSVector dest, WordEntry *destptr,
		int32 maxpos)
{
	uint16	   *clen = &_POSVECPTR(dest, destptr)->npos;
	int			i;
	uint16		slen = POSDATALEN(src, srcptr),
				startlen;
	WordEntryPos *spos = POSDATAPTR(src, srcptr),
			   *dpos = POSDATAPTR(dest, destptr);

	if (!destptr->haspos)
		*clen = 0;

	startlen = *clen;
	for (i = 0;
		 i < slen && *clen < MAXNUMPOS &&
		 (*clen == 0 || WEP_GETPOS(dpos[*clen - 1]) != MAXENTRYPOS - 1);
		 i++)
	{
		WEP_SETWEIGHT(dpos[*clen], WEP_GETWEIGHT(spos[i]));
		WEP_SETPOS(dpos[*clen], LIMITPOS(WEP_GETPOS(spos[i]) + maxpos));
		(*clen)++;
	}

	if (*clen != startlen)
		destptr->haspos = 1;
	return *clen - startlen;
}

/*
 * Perform binary search of given lexeme in TSVector.
 * Returns lexeme position in TSVector's entry array or -1 if lexeme wasn't
 * found.
 */
static int
tsvector_bsearch(const TSVector tsv, char *lexeme, int lexeme_len)
{
	WordEntry  *arrin = ARRPTR(tsv);
	int			StopLow = 0,
				StopHigh = tsv->size,
				StopMiddle,
				cmp;

	while (StopLow < StopHigh)
	{
		StopMiddle = (StopLow + StopHigh)/2;

		cmp = tsCompareString(lexeme, lexeme_len,
			STRPTR(tsv) + arrin[StopMiddle].pos,
			arrin[StopMiddle].len,
			false);

		if (cmp < 0)
			StopHigh = StopMiddle;
		else if (cmp > 0)
			StopLow = StopMiddle + 1;
		else /* found it */
			return StopMiddle;
	}

	return -1;
}

static int
compareint(const void *va, const void *vb)
{
	int32		a = *((const int32 *) va);
	int32		b = *((const int32 *) vb);

	if (a == b)
		return 0;
	return (a > b) ? 1 : -1;
}

/*
 * Internal routine to delete lexemes from TSVector by array of offsets.
 *
 * int *indices_to_delete -- array of lexeme offsets to delete
 * int indices_count -- size of that array
 *
 * Returns new TSVector without given lexemes along with their positions
 * and weights.
 */
static TSVector
tsvector_delete_by_indices(TSVector tsv, int *indices_to_delete,
						   int indices_count)
{
	TSVector	tsout;
	WordEntry  *arrin = ARRPTR(tsv),
			   *arrout;
	char	   *data = STRPTR(tsv),
			   *dataout;
	int			i, j, k,
				curoff;

	/*
	 * Here we overestimates tsout size, since we don't know exact size
	 * occupied by positions and weights. We will set exact size later
	 * after a pass through TSVector.
	 */
	tsout = (TSVector) palloc0(VARSIZE(tsv));
	arrout = ARRPTR(tsout);
	tsout->size = tsv->size - indices_count;

	/* Sort our filter array to simplify membership check later. */
	if (indices_count > 1)
		qsort(indices_to_delete, indices_count, sizeof(int), compareint);

	/*
	 * Copy tsv to tsout skipping lexemes that enlisted in indices_to_delete.
	 */
	curoff = 0;
	dataout = STRPTR(tsout);
	for (i = j = k = 0; i < tsv->size; i++)
	{
		/*
		 * Here we should check whether current i is present in
		 * indices_to_delete or not. Since indices_to_delete is already
		 * sorted we can advance it index only when we have match.
		 */
		if (k < indices_count && i == indices_to_delete[k]){
			k++;
			continue;
		}

		/* Copy lexeme, it's positions and weights */
		memcpy(dataout + curoff, data + arrin[i].pos, arrin[i].len);
		arrout[j].haspos = arrin[i].haspos;
		arrout[j].len = arrin[i].len;
		arrout[j].pos = curoff;
		curoff += arrin[i].len;
		if (arrin[i].haspos)
		{
			int len = POSDATALEN(tsv, arrin+i) * sizeof(WordEntryPos) +
					  sizeof(uint16);
			curoff = SHORTALIGN(curoff);
			memcpy(dataout + curoff,
				   STRPTR(tsv) + SHORTALIGN(arrin[i].pos + arrin[i].len),
				   len);
			curoff += len;
		}

		j++;
	}

	/*
	 * After the pass through TSVector k should equals exactly to indices_count.
	 * If it isn't then the caller provided us with indices outside of
	 * [0, tsv->size) range and estimation of tsout's size is wrong.
	 */
	Assert(k == indices_count);

	SET_VARSIZE(tsout, CALCDATASIZE(tsout->size, curoff));
	return tsout;
}

/*
 * Delete given lexeme from tsvector.
 * Implementation of user-level ts_delete(tsvector, text).
 */
Datum
tsvector_delete_str(PG_FUNCTION_ARGS)
{
	TSVector	tsin = PG_GETARG_TSVECTOR(0),
				tsout;
	text	   *tlexeme = PG_GETARG_TEXT_P(1);
	char	   *lexeme = VARDATA(tlexeme);
	int			lexeme_len = VARSIZE_ANY_EXHDR(tlexeme),
				skip_index;

	if ((skip_index = tsvector_bsearch(tsin, lexeme, lexeme_len)) == -1)
		PG_RETURN_POINTER(tsin);

	tsout = tsvector_delete_by_indices(tsin, &skip_index, 1);

	PG_FREE_IF_COPY(tsin, 0);
	PG_FREE_IF_COPY(tlexeme, 1);
	PG_RETURN_POINTER(tsout);
}

/*
 * Delete given array of lexemes from tsvector.
 * Implementation of user-level ts_delete(tsvector, text[]).
 */
Datum
tsvector_delete_arr(PG_FUNCTION_ARGS)
{
	TSVector	tsin = PG_GETARG_TSVECTOR(0),
				tsout;
	ArrayType  *lexemes = PG_GETARG_ARRAYTYPE_P(1);
	int			i, nlex,
				skip_count,
			   *skip_indices;
	Datum	   *dlexemes;
	bool	   *nulls;

	deconstruct_array(lexemes, TEXTOID, -1, false, 'i',
					  &dlexemes, &nulls, &nlex);

	/*
	 * In typical use case array of lexemes to delete is relatively small.
	 * So here we optimizing things for that scenario: iterate through lexarr
	 * performing binary search of each lexeme from lexarr in tsvector.
	 */
	skip_indices = palloc0(nlex * sizeof(int));
	for (i = skip_count = 0; i < nlex; i++)
	{
		char *lex;
		int lex_len,
			lex_pos;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("lexeme array may not contain nulls")));

		lex = VARDATA(dlexemes[i]);
		lex_len = VARSIZE_ANY_EXHDR(dlexemes[i]);
		lex_pos = tsvector_bsearch(tsin, lex, lex_len);

		if (lex_pos >= 0)
			skip_indices[skip_count++] = lex_pos;
	}

	tsout = tsvector_delete_by_indices(tsin, skip_indices, skip_count);

	pfree(skip_indices);
	PG_FREE_IF_COPY(tsin, 0);
	PG_FREE_IF_COPY(lexemes, 1);

	PG_RETURN_POINTER(tsout);
}

/*
 * Expand tsvector as table with following columns:
 *     lexeme: lexeme text
 *     positions: integer array of lexeme positions
 *     weights: char array of weights corresponding to positions
 */
Datum
tsvector_unnest(PG_FUNCTION_ARGS)
{
	FuncCallContext	   *funcctx;
	TSVector			tsin;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(3, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "lexeme",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "positions",
						   INT2ARRAYOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "weights",
						   TEXTARRAYOID, -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		funcctx->user_fctx = PG_GETARG_TSVECTOR_COPY(0);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	tsin = (TSVector) funcctx->user_fctx;

	if (funcctx->call_cntr < tsin->size)
	{
		WordEntry  *arrin = ARRPTR(tsin);
		char	   *data = STRPTR(tsin);
		HeapTuple	tuple;
		int			j,
					i = funcctx->call_cntr;
		bool		nulls[] = {false, false, false};
		Datum		values[3];

		values[0] = PointerGetDatum(
				cstring_to_text_with_len(data + arrin[i].pos, arrin[i].len)
		);

		if (arrin[i].haspos)
		{
			WordEntryPosVector *posv;
			Datum	   *positions;
			Datum	   *weights;
			char		weight;

			/*
			 * Internally tsvector stores position and weight in the same
			 * uint16 (2 bits for weight, 14 for position). Here we extract that
			 * in two separate arrays.
			 */
			posv = _POSVECPTR(tsin, arrin + i);
			positions = palloc(posv->npos * sizeof(Datum));
			weights   = palloc(posv->npos * sizeof(Datum));
			for (j = 0; j < posv->npos; j++)
			{
				positions[j] = Int16GetDatum(WEP_GETPOS(posv->pos[j]));
				weight = 'D' - WEP_GETWEIGHT(posv->pos[j]);
				weights[j] = PointerGetDatum(
									cstring_to_text_with_len(&weight, 1)
							);
			}

			values[1] = PointerGetDatum(
				construct_array(positions, posv->npos, INT2OID, 2, true, 's'));
			values[2] = PointerGetDatum(
				construct_array(weights, posv->npos, TEXTOID, -1, false, 'i'));
		}
		else
		{
			nulls[1] = nulls[2] = true;
		}

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		pfree(tsin);
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * Convert tsvector to array of lexemes.
 */
Datum
tsvector_to_array(PG_FUNCTION_ARGS)
{
	TSVector			tsin  = PG_GETARG_TSVECTOR(0);
	WordEntry		   *arrin = ARRPTR(tsin);
	Datum				*elements;
	int					i;
	ArrayType		   *array;

	elements = palloc(tsin->size * sizeof(Datum));

	for (i = 0; i < tsin->size; i++)
	{
		elements[i] = PointerGetDatum(
			cstring_to_text_with_len(STRPTR(tsin) + arrin[i].pos, arrin[i].len)
		);
	}

	array = construct_array(elements, tsin->size, TEXTOID, -1, false, 'i');

	pfree(elements);
	PG_FREE_IF_COPY(tsin, 0);
	PG_RETURN_POINTER(array);
}

/*
 * Build tsvector from array of lexemes.
 */
Datum
array_to_tsvector(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	TSVector	tsout;
	Datum	   *dlexemes;
	WordEntry  *arrout;
	bool	   *nulls;
	int			nitems,
				i,
				tslen,
				datalen = 0;
	char	   *cur;

	deconstruct_array(v, TEXTOID, -1, false, 'i', &dlexemes, &nulls, &nitems);

	for (i = 0; i < nitems; i++)
	{
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("lexeme array may not contain nulls")));

		datalen += VARSIZE_ANY_EXHDR(dlexemes[i]);
	}

	tslen = CALCDATASIZE(nitems, datalen);
	tsout = (TSVector) palloc0(tslen);
	SET_VARSIZE(tsout, tslen);
	tsout->size = nitems;
	arrout = ARRPTR(tsout);
	cur = STRPTR(tsout);

	for (i = 0; i < nitems; i++)
	{
		char *lex = VARDATA(dlexemes[i]);
		int lex_len = VARSIZE_ANY_EXHDR(dlexemes[i]);

		memcpy(cur, lex, lex_len);
		arrout[i].haspos = 0;
		arrout[i].len = lex_len;
		arrout[i].pos = cur - STRPTR(tsout);
		cur += lex_len;
	}

	PG_FREE_IF_COPY(v, 0);
	PG_RETURN_POINTER(tsout);
}

/*
 * ts_filter(): keep only lexemes with given weights in tsvector.
 */
Datum
tsvector_filter(PG_FUNCTION_ARGS)
{
	TSVector	tsin = PG_GETARG_TSVECTOR(0),
				tsout;
	ArrayType  *weights = PG_GETARG_ARRAYTYPE_P(1);
	WordEntry  *arrin = ARRPTR(tsin),
			   *arrout;
	char	   *datain = STRPTR(tsin),
			   *dataout;
	Datum	   *dweights;
	bool	   *nulls;
	int			nweights;
	int			i, j;
	int			cur_pos = 0;
	char		mask = 0;

	deconstruct_array(weights, CHAROID, 1, true, 'c',
					  &dweights, &nulls, &nweights);

	for (i = 0; i < nweights; i++)
	{
		char char_weight;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("weight array may not contain nulls")));

		char_weight = DatumGetChar(dweights[i]);
		switch (char_weight)
		{
			case 'A': case 'a':
				mask = mask | 8;
				break;
			case 'B': case 'b':
				mask = mask | 4;
				break;
			case 'C': case 'c':
				mask = mask | 2;
				break;
			case 'D': case 'd':
				mask = mask | 1;
				break;
			default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized weight: \"%c\"", char_weight)));
		}
	}

	tsout = (TSVector) palloc0(VARSIZE(tsin));
	tsout->size = tsin->size;
	arrout = ARRPTR(tsout);
	dataout = STRPTR(tsout);

	for (i = j = 0; i < tsin->size; i++)
	{
		WordEntryPosVector *posvin,
						   *posvout;
		int npos = 0;
		int k;

		if (!arrin[i].haspos)
			continue;

		posvin  = _POSVECPTR(tsin, arrin + i);
		posvout = (WordEntryPosVector *)
						(dataout + SHORTALIGN(cur_pos + arrin[i].len));

		for (k = 0; k < posvin->npos; k++)
		{
			if (mask & (1 << WEP_GETWEIGHT(posvin->pos[k])))
				posvout->pos[npos++] = posvin->pos[k];
		}

		/* if no satisfactory positions found, skip lexeme */
		if (!npos)
			continue;

		arrout[j].haspos = true;
		arrout[j].len = arrin[i].len;
		arrout[j].pos = cur_pos;

		memcpy(dataout + cur_pos, datain + arrin[i].pos, arrin[i].len);
		posvout->npos = npos;
		cur_pos += SHORTALIGN(arrin[i].len);
		cur_pos += POSDATALEN(tsout, arrout+j) * sizeof(WordEntryPos) +
				   sizeof(uint16);
		j++;
	}

	tsout->size = j;
	if (dataout != STRPTR(tsout))
		memmove(STRPTR(tsout), dataout, cur_pos);

	SET_VARSIZE(tsout, CALCDATASIZE(tsout->size, cur_pos));

	PG_FREE_IF_COPY(tsin, 0);
	PG_RETURN_POINTER(tsout);
}

Datum
tsvector_concat(PG_FUNCTION_ARGS)
{
	TSVector	in1 = PG_GETARG_TSVECTOR(0);
	TSVector	in2 = PG_GETARG_TSVECTOR(1);
	TSVector	out;
	WordEntry  *ptr;
	WordEntry  *ptr1,
			   *ptr2;
	WordEntryPos *p;
	int			maxpos = 0,
				i,
				j,
				i1,
				i2,
				dataoff,
				output_bytes,
				output_size;
	char	   *data,
			   *data1,
			   *data2;

	/* Get max position in in1; we'll need this to offset in2's positions */
	ptr = ARRPTR(in1);
	i = in1->size;
	while (i--)
	{
		if ((j = POSDATALEN(in1, ptr)) != 0)
		{
			p = POSDATAPTR(in1, ptr);
			while (j--)
			{
				if (WEP_GETPOS(*p) > maxpos)
					maxpos = WEP_GETPOS(*p);
				p++;
			}
		}
		ptr++;
	}

	ptr1 = ARRPTR(in1);
	ptr2 = ARRPTR(in2);
	data1 = STRPTR(in1);
	data2 = STRPTR(in2);
	i1 = in1->size;
	i2 = in2->size;

	/*
	 * Conservative estimate of space needed.  We might need all the data in
	 * both inputs, and conceivably add a pad byte before position data for
	 * each item where there was none before.
	 */
	output_bytes = VARSIZE(in1) + VARSIZE(in2) + i1 + i2;

	out = (TSVector) palloc0(output_bytes);
	SET_VARSIZE(out, output_bytes);

	/*
	 * We must make out->size valid so that STRPTR(out) is sensible.  We'll
	 * collapse out any unused space at the end.
	 */
	out->size = in1->size + in2->size;

	ptr = ARRPTR(out);
	data = STRPTR(out);
	dataoff = 0;
	while (i1 && i2)
	{
		int			cmp = compareEntry(data1, ptr1, data2, ptr2);

		if (cmp < 0)
		{						/* in1 first */
			ptr->haspos = ptr1->haspos;
			ptr->len = ptr1->len;
			memcpy(data + dataoff, data1 + ptr1->pos, ptr1->len);
			ptr->pos = dataoff;
			dataoff += ptr1->len;
			if (ptr->haspos)
			{
				dataoff = SHORTALIGN(dataoff);
				memcpy(data + dataoff, _POSVECPTR(in1, ptr1), POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16));
				dataoff += POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16);
			}

			ptr++;
			ptr1++;
			i1--;
		}
		else if (cmp > 0)
		{						/* in2 first */
			ptr->haspos = ptr2->haspos;
			ptr->len = ptr2->len;
			memcpy(data + dataoff, data2 + ptr2->pos, ptr2->len);
			ptr->pos = dataoff;
			dataoff += ptr2->len;
			if (ptr->haspos)
			{
				int			addlen = add_pos(in2, ptr2, out, ptr, maxpos);

				if (addlen == 0)
					ptr->haspos = 0;
				else
				{
					dataoff = SHORTALIGN(dataoff);
					dataoff += addlen * sizeof(WordEntryPos) + sizeof(uint16);
				}
			}

			ptr++;
			ptr2++;
			i2--;
		}
		else
		{
			ptr->haspos = ptr1->haspos | ptr2->haspos;
			ptr->len = ptr1->len;
			memcpy(data + dataoff, data1 + ptr1->pos, ptr1->len);
			ptr->pos = dataoff;
			dataoff += ptr1->len;
			if (ptr->haspos)
			{
				if (ptr1->haspos)
				{
					dataoff = SHORTALIGN(dataoff);
					memcpy(data + dataoff, _POSVECPTR(in1, ptr1), POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16));
					dataoff += POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16);
					if (ptr2->haspos)
						dataoff += add_pos(in2, ptr2, out, ptr, maxpos) * sizeof(WordEntryPos);
				}
				else	/* must have ptr2->haspos */
				{
					int			addlen = add_pos(in2, ptr2, out, ptr, maxpos);

					if (addlen == 0)
						ptr->haspos = 0;
					else
					{
						dataoff = SHORTALIGN(dataoff);
						dataoff += addlen * sizeof(WordEntryPos) + sizeof(uint16);
					}
				}
			}

			ptr++;
			ptr1++;
			ptr2++;
			i1--;
			i2--;
		}
	}

	while (i1)
	{
		ptr->haspos = ptr1->haspos;
		ptr->len = ptr1->len;
		memcpy(data + dataoff, data1 + ptr1->pos, ptr1->len);
		ptr->pos = dataoff;
		dataoff += ptr1->len;
		if (ptr->haspos)
		{
			dataoff = SHORTALIGN(dataoff);
			memcpy(data + dataoff, _POSVECPTR(in1, ptr1), POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16));
			dataoff += POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16);
		}

		ptr++;
		ptr1++;
		i1--;
	}

	while (i2)
	{
		ptr->haspos = ptr2->haspos;
		ptr->len = ptr2->len;
		memcpy(data + dataoff, data2 + ptr2->pos, ptr2->len);
		ptr->pos = dataoff;
		dataoff += ptr2->len;
		if (ptr->haspos)
		{
			int			addlen = add_pos(in2, ptr2, out, ptr, maxpos);

			if (addlen == 0)
				ptr->haspos = 0;
			else
			{
				dataoff = SHORTALIGN(dataoff);
				dataoff += addlen * sizeof(WordEntryPos) + sizeof(uint16);
			}
		}

		ptr++;
		ptr2++;
		i2--;
	}

	/*
	 * Instead of checking each offset individually, we check for overflow of
	 * pos fields once at the end.
	 */
	if (dataoff > MAXSTRPOS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("string is too long for tsvector (%d bytes, max %d bytes)", dataoff, MAXSTRPOS)));

	/*
	 * Adjust sizes (asserting that we didn't overrun the original estimates)
	 * and collapse out any unused array entries.
	 */
	output_size = ptr - ARRPTR(out);
	Assert(output_size <= out->size);
	out->size = output_size;
	if (data != STRPTR(out))
		memmove(STRPTR(out), data, dataoff);
	output_bytes = CALCDATASIZE(out->size, dataoff);
	Assert(output_bytes <= VARSIZE(out));
	SET_VARSIZE(out, output_bytes);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_POINTER(out);
}

/*
 * Compare two strings by tsvector rules.
 *
 * if isPrefix = true then it returns zero value iff b has prefix a
 */
int32
tsCompareString(char *a, int lena, char *b, int lenb, bool prefix)
{
	int			cmp;

	if (lena == 0)
	{
		if (prefix)
			cmp = 0;			/* empty string is prefix of anything */
		else
			cmp = (lenb > 0) ? -1 : 0;
	}
	else if (lenb == 0)
	{
		cmp = (lena > 0) ? 1 : 0;
	}
	else
	{
		cmp = memcmp(a, b, Min(lena, lenb));

		if (prefix)
		{
			if (cmp == 0 && lena > lenb)
				cmp = 1;		/* a is longer, so not a prefix of b */
		}
		else if (cmp == 0 && lena != lenb)
		{
			cmp = (lena < lenb) ? -1 : 1;
		}
	}

	return cmp;
}

/*
 * Check weight info or/and fill 'data' with the required positions
 */
static bool
checkclass_str(CHKVAL *chkval, WordEntry *entry, QueryOperand *val,
			   ExecPhraseData *data)
{
	bool result = false;

	if (entry->haspos && (val->weight || data))
	{
		WordEntryPosVector	*posvec;

		/*
		 * We can't use the _POSVECPTR macro here because the pointer to the
		 * tsvector's lexeme storage is already contained in chkval->values.
		 */
		posvec = (WordEntryPosVector *)
			(chkval->values + SHORTALIGN(entry->pos + entry->len));

		if (val->weight && data)
		{
			WordEntryPos	*posvec_iter = posvec->pos;
			WordEntryPos	*dptr;

			/*
			 * Filter position information by weights
			 */
			dptr = data->pos = palloc(sizeof(WordEntryPos) * posvec->npos);
			data->allocated = true;

			/* Is there a position with a matching weight? */
			while (posvec_iter < posvec->pos + posvec->npos)
			{
				/* If true, append this position to the data->pos */
				if (val->weight & (1 << WEP_GETWEIGHT(*posvec_iter)))
				{
					*dptr = WEP_GETPOS(*posvec_iter);
					dptr++;
				}

				posvec_iter++;
			}

			data->npos = dptr - data->pos;

			if (data->npos > 0)
				result = true;
		}
		else if (val->weight)
		{
			WordEntryPos	*posvec_iter = posvec->pos;

			/* Is there a position with a matching weight? */
			while (posvec_iter < posvec->pos + posvec->npos)
			{
				if (val->weight & (1 << WEP_GETWEIGHT(*posvec_iter)))
				{
					result = true;
					break; /* no need to go further */
				}

				posvec_iter++;
			}
		}
		else /* data != NULL */
		{
			data->npos = posvec->npos;
			data->pos  = posvec->pos;
			data->allocated = false;
			result = true;
		}
	}
	else
	{
		result = true;
	}

	return result;
}

/*
 * Removes duplicate pos entries. We can't use uniquePos() from
 * tsvector.c because array might be longer than MAXENTRYPOS
 *
 * Returns new length.
 */
static int
uniqueLongPos(WordEntryPos *pos, int npos)
{
	WordEntryPos *pos_iter,
				 *result;

	if (npos <= 1)
		return npos;

	qsort((void *) pos, npos, sizeof(WordEntryPos), compareWordEntryPos);

	result = pos;
	pos_iter = pos + 1;
	while (pos_iter < pos + npos)
	{
		if (WEP_GETPOS(*pos_iter) != WEP_GETPOS(*result))
		{
			result++;
			*result = WEP_GETPOS(*pos_iter);
		}

		pos_iter++;
	}

	return result + 1 - pos;
}

/*
 * is there value 'val' in array or not ?
 */
static bool
checkcondition_str(void *checkval, QueryOperand *val, ExecPhraseData *data)
{
	CHKVAL	   *chkval = (CHKVAL *) checkval;
	WordEntry  *StopLow = chkval->arrb;
	WordEntry  *StopHigh = chkval->arre;
	WordEntry  *StopMiddle = StopHigh;
	int			difference = -1;
	bool		res = false;

	/* Loop invariant: StopLow <= val < StopHigh */
	while (StopLow < StopHigh)
	{
		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		difference = tsCompareString(chkval->operand + val->distance,
									 val->length,
									 chkval->values + StopMiddle->pos,
									 StopMiddle->len,
									 false);

		if (difference == 0)
		{
			/* Check weight info & fill 'data' with positions */
			res = checkclass_str(chkval, StopMiddle, val, data);
			break;
		}
		else if (difference > 0)
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle;
	}

	if ((!res || data) && val->prefix)
	{
		WordEntryPos	   *allpos = NULL;
		int					npos = 0,
							totalpos = 0;
		/*
		 * there was a failed exact search, so we should scan further to find
		 * a prefix match. We also need to do so if caller needs position info
		 */
		if (StopLow >= StopHigh)
			StopMiddle = StopHigh;

		while ((!res || data) && StopMiddle < chkval->arre &&
			   tsCompareString(chkval->operand + val->distance,
							   val->length,
							   chkval->values + StopMiddle->pos,
							   StopMiddle->len,
							   true) == 0)
		{
			if (data)
			{
				/*
				 * We need to join position information
				 */
				res = checkclass_str(chkval, StopMiddle, val, data);

				if (res)
				{
					while (npos + data->npos >= totalpos)
					{
						if (totalpos == 0)
						{
							totalpos = 256;
							allpos = palloc(sizeof(WordEntryPos) * totalpos);
						}
						else
						{
							totalpos *= 2;
							allpos = repalloc(allpos, sizeof(WordEntryPos) * totalpos);
						}
					}

					memcpy(allpos + npos, data->pos, sizeof(WordEntryPos) * data->npos);
					npos += data->npos;
				}
			}
			else
			{
				res = checkclass_str(chkval, StopMiddle, val, NULL);
			}

			StopMiddle++;
		}

		if (res && data)
		{
			/* Sort and make unique array of found positions */
			data->pos = allpos;
			data->npos = uniqueLongPos(allpos, npos);
			data->allocated = true;
		}
	}

	return res;
}

/*
 * Check for phrase condition. Fallback to the AND operation
 * if there is no positional information.
 */
static bool
TS_phrase_execute(QueryItem *curitem,
				  void *checkval, bool calcnot, ExecPhraseData *data,
				  bool (*chkcond) (void *, QueryOperand *, ExecPhraseData *))
{
	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	if (curitem->type == QI_VAL)
	{
		return chkcond(checkval, (QueryOperand *) curitem, data);
	}
	else
	{
		ExecPhraseData  Ldata = {0, false, NULL},
						Rdata = {0, false, NULL};
		WordEntryPos   *Lpos,
					   *Rpos,
					   *pos_iter = NULL;

		Assert(curitem->qoperator.oper == OP_PHRASE);

		if (!TS_phrase_execute(curitem + curitem->qoperator.left,
							   checkval, calcnot, &Ldata, chkcond))
			return false;

		if (!TS_phrase_execute(curitem + 1, checkval, calcnot, &Rdata, chkcond))
			return false;

		/*
		 * if at least one of the operands has no position
		 * information, fallback to AND operation.
		 */
		if (Ldata.npos == 0 || Rdata.npos == 0)
			return true;

		/*
		 * Result of the operation is a list of the
		 * corresponding positions of RIGHT operand.
		 */
		if (data)
		{
			if (!Rdata.allocated)
				/*
				 * OP_PHRASE is based on the OP_AND, so the number of resulting
				 * positions could not be greater than the total amount of operands.
				 */
				data->pos = palloc(sizeof(WordEntryPos) * Min(Ldata.npos, Rdata.npos));
			else
				data->pos = Rdata.pos;

			data->allocated = true;
			data->npos = 0;
			pos_iter = data->pos;
		}

		Lpos = Ldata.pos;
		Rpos = Rdata.pos;

		/*
		 * Find matches by distance, WEP_GETPOS() is needed because
		 * ExecPhraseData->data can point to the tsvector's WordEntryPosVector
		 */

		while (Rpos < Rdata.pos + Rdata.npos)
		{
			while (Lpos < Ldata.pos + Ldata.npos)
			{
				if (WEP_GETPOS(*Lpos) <= WEP_GETPOS(*Rpos))
				{
					/*
					 * Lpos is behind the Rpos, so we have to check the
					 * distance condition
					 */
					if (WEP_GETPOS(*Rpos) - WEP_GETPOS(*Lpos) <= curitem->qoperator.distance)
					{
						/* MATCH! */
						if (data)
						{
							*pos_iter = WEP_GETPOS(*Rpos);
							pos_iter++;

							break; /* We need to build a unique result
									* array, so go to the next Rpos */
						}
						else
						{
							/*
							 * We are in the root of the phrase tree and hence
							 * we don't have to store the resulting positions
							 */
							return true;
						}
					}
				}
				else
				{
					/*
					 * Go to the next Rpos, because Lpos
					 * is ahead of the current Rpos
					 */
					break;
				}

				Lpos++;
			}

			Rpos++;
		}

		if (data)
		{
			data->npos = pos_iter - data->pos;

			if (data->npos > 0)
				return true;
		}
	}

	return false;
}


/*
 * Evaluate tsquery boolean expression.
 *
 * chkcond is a callback function used to evaluate each VAL node in the query.
 * checkval can be used to pass information to the callback. TS_execute doesn't
 * do anything with it.
 * if calcnot is false, NOT expressions are always evaluated to be true. This
 * is used in ranking.
 * It believes that ordinary operators are always closier to root than phrase
 * operator, so, TS_execute() may not take care of lexeme's position at all.
 */
bool
TS_execute(QueryItem *curitem, void *checkval, bool calcnot,
		   bool (*chkcond) (void *checkval, QueryOperand *val, ExecPhraseData *data))
{
	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	if (curitem->type == QI_VAL)
		return chkcond(checkval, (QueryOperand *) curitem,
					   NULL /* we don't need position info */);

	switch (curitem->qoperator.oper)
	{
		case OP_NOT:
			if (calcnot)
				return !TS_execute(curitem + 1, checkval, calcnot, chkcond);
			else
				return true;

		case OP_AND:
			if (TS_execute(curitem + curitem->qoperator.left, checkval, calcnot, chkcond))
				return TS_execute(curitem + 1, checkval, calcnot, chkcond);
			else
				return false;

		case OP_OR:
			if (TS_execute(curitem + curitem->qoperator.left, checkval, calcnot, chkcond))
				return true;
			else
				return TS_execute(curitem + 1, checkval, calcnot, chkcond);

		case OP_PHRASE:
			return TS_phrase_execute(curitem, checkval, calcnot, NULL, chkcond);

		default:
			elog(ERROR, "unrecognized operator: %d", curitem->qoperator.oper);
	}

	/* not reachable, but keep compiler quiet */
	return false;
}

/*
 * Detect whether a tsquery boolean expression requires any positive matches
 * to values shown in the tsquery.
 *
 * This is needed to know whether a GIN index search requires full index scan.
 * For example, 'x & !y' requires a match of x, so it's sufficient to scan
 * entries for x; but 'x | !y' could match rows containing neither x nor y.
 */
bool
tsquery_requires_match(QueryItem *curitem)
{
	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	if (curitem->type == QI_VAL)
		return true;

	switch (curitem->qoperator.oper)
	{
		case OP_NOT:

			/*
			 * Assume there are no required matches underneath a NOT.  For
			 * some cases with nested NOTs, we could prove there's a required
			 * match, but it seems unlikely to be worth the trouble.
			 */
			return false;

		case OP_PHRASE:
			/*
			 * Treat OP_PHRASE as OP_AND here
			 */
		case OP_AND:
			/* If either side requires a match, we're good */
			if (tsquery_requires_match(curitem + curitem->qoperator.left))
				return true;
			else
				return tsquery_requires_match(curitem + 1);

		case OP_OR:
			/* Both sides must require a match */
			if (tsquery_requires_match(curitem + curitem->qoperator.left))
				return tsquery_requires_match(curitem + 1);
			else
				return false;

		default:
			elog(ERROR, "unrecognized operator: %d", curitem->qoperator.oper);
	}

	/* not reachable, but keep compiler quiet */
	return false;
}

/*
 * boolean operations
 */
Datum
ts_match_qv(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall2(ts_match_vq,
										PG_GETARG_DATUM(1),
										PG_GETARG_DATUM(0)));
}

Datum
ts_match_vq(PG_FUNCTION_ARGS)
{
	TSVector	val = PG_GETARG_TSVECTOR(0);
	TSQuery		query = PG_GETARG_TSQUERY(1);
	CHKVAL		chkval;
	bool		result;

	if (!val->size || !query->size)
	{
		PG_FREE_IF_COPY(val, 0);
		PG_FREE_IF_COPY(query, 1);
		PG_RETURN_BOOL(false);
	}

	chkval.arrb = ARRPTR(val);
	chkval.arre = chkval.arrb + val->size;
	chkval.values = STRPTR(val);
	chkval.operand = GETOPERAND(query);
	result = TS_execute(
						GETQUERY(query),
						&chkval,
						true,
						checkcondition_str
		);

	PG_FREE_IF_COPY(val, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(result);
}

Datum
ts_match_tt(PG_FUNCTION_ARGS)
{
	TSVector	vector;
	TSQuery		query;
	bool		res;

	vector = DatumGetTSVector(DirectFunctionCall1(to_tsvector,
												  PG_GETARG_DATUM(0)));
	query = DatumGetTSQuery(DirectFunctionCall1(plainto_tsquery,
												PG_GETARG_DATUM(1)));

	res = DatumGetBool(DirectFunctionCall2(ts_match_vq,
										   TSVectorGetDatum(vector),
										   TSQueryGetDatum(query)));

	pfree(vector);
	pfree(query);

	PG_RETURN_BOOL(res);
}

Datum
ts_match_tq(PG_FUNCTION_ARGS)
{
	TSVector	vector;
	TSQuery		query = PG_GETARG_TSQUERY(1);
	bool		res;

	vector = DatumGetTSVector(DirectFunctionCall1(to_tsvector,
												  PG_GETARG_DATUM(0)));

	res = DatumGetBool(DirectFunctionCall2(ts_match_vq,
										   TSVectorGetDatum(vector),
										   TSQueryGetDatum(query)));

	pfree(vector);
	PG_FREE_IF_COPY(query, 1);

	PG_RETURN_BOOL(res);
}

/*
 * ts_stat statistic function support
 */


/*
 * Returns the number of positions in value 'wptr' within tsvector 'txt',
 * that have a weight equal to one of the weights in 'weight' bitmask.
 */
static int
check_weight(TSVector txt, WordEntry *wptr, int8 weight)
{
	int			len = POSDATALEN(txt, wptr);
	int			num = 0;
	WordEntryPos *ptr = POSDATAPTR(txt, wptr);

	while (len--)
	{
		if (weight & (1 << WEP_GETWEIGHT(*ptr)))
			num++;
		ptr++;
	}
	return num;
}

#define compareStatWord(a,e,t)							\
	tsCompareString((a)->lexeme, (a)->lenlexeme,		\
					STRPTR(t) + (e)->pos, (e)->len,		\
					false)

static void
insertStatEntry(MemoryContext persistentContext, TSVectorStat *stat, TSVector txt, uint32 off)
{
	WordEntry  *we = ARRPTR(txt) + off;
	StatEntry  *node = stat->root,
			   *pnode = NULL;
	int			n,
				res = 0;
	uint32		depth = 1;

	if (stat->weight == 0)
		n = (we->haspos) ? POSDATALEN(txt, we) : 1;
	else
		n = (we->haspos) ? check_weight(txt, we, stat->weight) : 0;

	if (n == 0)
		return;					/* nothing to insert */

	while (node)
	{
		res = compareStatWord(node, we, txt);

		if (res == 0)
		{
			break;
		}
		else
		{
			pnode = node;
			node = (res < 0) ? node->left : node->right;
		}
		depth++;
	}

	if (depth > stat->maxdepth)
		stat->maxdepth = depth;

	if (node == NULL)
	{
		node = MemoryContextAlloc(persistentContext, STATENTRYHDRSZ + we->len);
		node->left = node->right = NULL;
		node->ndoc = 1;
		node->nentry = n;
		node->lenlexeme = we->len;
		memcpy(node->lexeme, STRPTR(txt) + we->pos, node->lenlexeme);

		if (pnode == NULL)
		{
			stat->root = node;
		}
		else
		{
			if (res < 0)
				pnode->left = node;
			else
				pnode->right = node;
		}

	}
	else
	{
		node->ndoc++;
		node->nentry += n;
	}
}

static void
chooseNextStatEntry(MemoryContext persistentContext, TSVectorStat *stat, TSVector txt,
					uint32 low, uint32 high, uint32 offset)
{
	uint32		pos;
	uint32		middle = (low + high) >> 1;

	pos = (low + middle) >> 1;
	if (low != middle && pos >= offset && pos - offset < txt->size)
		insertStatEntry(persistentContext, stat, txt, pos - offset);
	pos = (high + middle + 1) >> 1;
	if (middle + 1 != high && pos >= offset && pos - offset < txt->size)
		insertStatEntry(persistentContext, stat, txt, pos - offset);

	if (low != middle)
		chooseNextStatEntry(persistentContext, stat, txt, low, middle, offset);
	if (high != middle + 1)
		chooseNextStatEntry(persistentContext, stat, txt, middle + 1, high, offset);
}

/*
 * This is written like a custom aggregate function, because the
 * original plan was to do just that. Unfortunately, an aggregate function
 * can't return a set, so that plan was abandoned. If that limitation is
 * lifted in the future, ts_stat could be a real aggregate function so that
 * you could use it like this:
 *
 *	 SELECT ts_stat(vector_column) FROM vector_table;
 *
 *	where vector_column is a tsvector-type column in vector_table.
 */

static TSVectorStat *
ts_accum(MemoryContext persistentContext, TSVectorStat *stat, Datum data)
{
	TSVector	txt = DatumGetTSVector(data);
	uint32		i,
				nbit = 0,
				offset;

	if (stat == NULL)
	{							/* Init in first */
		stat = MemoryContextAllocZero(persistentContext, sizeof(TSVectorStat));
		stat->maxdepth = 1;
	}

	/* simple check of correctness */
	if (txt == NULL || txt->size == 0)
	{
		if (txt && txt != (TSVector) DatumGetPointer(data))
			pfree(txt);
		return stat;
	}

	i = txt->size - 1;
	for (; i > 0; i >>= 1)
		nbit++;

	nbit = 1 << nbit;
	offset = (nbit - txt->size) / 2;

	insertStatEntry(persistentContext, stat, txt, (nbit >> 1) - offset);
	chooseNextStatEntry(persistentContext, stat, txt, 0, nbit, offset);

	return stat;
}

static void
ts_setup_firstcall(FunctionCallInfo fcinfo, FuncCallContext *funcctx,
				   TSVectorStat *stat)
{
	TupleDesc	tupdesc;
	MemoryContext oldcontext;
	StatEntry  *node;

	funcctx->user_fctx = (void *) stat;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

	stat->stack = palloc0(sizeof(StatEntry *) * (stat->maxdepth + 1));
	stat->stackpos = 0;

	node = stat->root;
	/* find leftmost value */
	if (node == NULL)
		stat->stack[stat->stackpos] = NULL;
	else
		for (;;)
		{
			stat->stack[stat->stackpos] = node;
			if (node->left)
			{
				stat->stackpos++;
				node = node->left;
			}
			else
				break;
		}
	Assert(stat->stackpos <= stat->maxdepth);

	tupdesc = CreateTemplateTupleDesc(3, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "word",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "ndoc",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "nentry",
					   INT4OID, -1, 0);
	funcctx->tuple_desc = BlessTupleDesc(tupdesc);
	funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);

	MemoryContextSwitchTo(oldcontext);
}

static StatEntry *
walkStatEntryTree(TSVectorStat *stat)
{
	StatEntry  *node = stat->stack[stat->stackpos];

	if (node == NULL)
		return NULL;

	if (node->ndoc != 0)
	{
		/* return entry itself: we already was at left sublink */
		return node;
	}
	else if (node->right && node->right != stat->stack[stat->stackpos + 1])
	{
		/* go on right sublink */
		stat->stackpos++;
		node = node->right;

		/* find most-left value */
		for (;;)
		{
			stat->stack[stat->stackpos] = node;
			if (node->left)
			{
				stat->stackpos++;
				node = node->left;
			}
			else
				break;
		}
		Assert(stat->stackpos <= stat->maxdepth);
	}
	else
	{
		/* we already return all left subtree, itself and  right subtree */
		if (stat->stackpos == 0)
			return NULL;

		stat->stackpos--;
		return walkStatEntryTree(stat);
	}

	return node;
}

static Datum
ts_process_call(FuncCallContext *funcctx)
{
	TSVectorStat *st;
	StatEntry  *entry;

	st = (TSVectorStat *) funcctx->user_fctx;

	entry = walkStatEntryTree(st);

	if (entry != NULL)
	{
		Datum		result;
		char	   *values[3];
		char		ndoc[16];
		char		nentry[16];
		HeapTuple	tuple;

		values[0] = palloc(entry->lenlexeme + 1);
		memcpy(values[0], entry->lexeme, entry->lenlexeme);
		(values[0])[entry->lenlexeme] = '\0';
		sprintf(ndoc, "%d", entry->ndoc);
		values[1] = ndoc;
		sprintf(nentry, "%d", entry->nentry);
		values[2] = nentry;

		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);
		result = HeapTupleGetDatum(tuple);

		pfree(values[0]);

		/* mark entry as already visited */
		entry->ndoc = 0;

		return result;
	}

	return (Datum) 0;
}

static TSVectorStat *
ts_stat_sql(MemoryContext persistentContext, text *txt, text *ws)
{
	char	   *query = text_to_cstring(txt);
	TSVectorStat *stat;
	bool		isnull;
	Portal		portal;
	SPIPlanPtr	plan;

	if ((plan = SPI_prepare(query, 0, NULL)) == NULL)
		/* internal error */
		elog(ERROR, "SPI_prepare(\"%s\") failed", query);

	if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, true)) == NULL)
		/* internal error */
		elog(ERROR, "SPI_cursor_open(\"%s\") failed", query);

	SPI_cursor_fetch(portal, true, 100);

	if (SPI_tuptable == NULL ||
		SPI_tuptable->tupdesc->natts != 1 ||
		!IsBinaryCoercible(SPI_gettypeid(SPI_tuptable->tupdesc, 1),
						  TSVECTOROID))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ts_stat query must return one tsvector column")));

	stat = MemoryContextAllocZero(persistentContext, sizeof(TSVectorStat));
	stat->maxdepth = 1;

	if (ws)
	{
		char	   *buf;

		buf = VARDATA(ws);
		while (buf - VARDATA(ws) < VARSIZE(ws) - VARHDRSZ)
		{
			if (pg_mblen(buf) == 1)
			{
				switch (*buf)
				{
					case 'A':
					case 'a':
						stat->weight |= 1 << 3;
						break;
					case 'B':
					case 'b':
						stat->weight |= 1 << 2;
						break;
					case 'C':
					case 'c':
						stat->weight |= 1 << 1;
						break;
					case 'D':
					case 'd':
						stat->weight |= 1;
						break;
					default:
						stat->weight |= 0;
				}
			}
			buf += pg_mblen(buf);
		}
	}

	while (SPI_processed > 0)
	{
		uint64		i;

		for (i = 0; i < SPI_processed; i++)
		{
			Datum		data = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);

			if (!isnull)
				stat = ts_accum(persistentContext, stat, data);
		}

		SPI_freetuptable(SPI_tuptable);
		SPI_cursor_fetch(portal, true, 100);
	}

	SPI_freetuptable(SPI_tuptable);
	SPI_cursor_close(portal);
	SPI_freeplan(plan);
	pfree(query);

	return stat;
}

Datum
ts_stat1(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Datum		result;

	if (SRF_IS_FIRSTCALL())
	{
		TSVectorStat *stat;
		text	   *txt = PG_GETARG_TEXT_P(0);

		funcctx = SRF_FIRSTCALL_INIT();
		SPI_connect();
		stat = ts_stat_sql(funcctx->multi_call_memory_ctx, txt, NULL);
		PG_FREE_IF_COPY(txt, 0);
		ts_setup_firstcall(fcinfo, funcctx, stat);
		SPI_finish();
	}

	funcctx = SRF_PERCALL_SETUP();
	if ((result = ts_process_call(funcctx)) != (Datum) 0)
		SRF_RETURN_NEXT(funcctx, result);
	SRF_RETURN_DONE(funcctx);
}

Datum
ts_stat2(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Datum		result;

	if (SRF_IS_FIRSTCALL())
	{
		TSVectorStat *stat;
		text	   *txt = PG_GETARG_TEXT_P(0);
		text	   *ws = PG_GETARG_TEXT_P(1);

		funcctx = SRF_FIRSTCALL_INIT();
		SPI_connect();
		stat = ts_stat_sql(funcctx->multi_call_memory_ctx, txt, ws);
		PG_FREE_IF_COPY(txt, 0);
		PG_FREE_IF_COPY(ws, 1);
		ts_setup_firstcall(fcinfo, funcctx, stat);
		SPI_finish();
	}

	funcctx = SRF_PERCALL_SETUP();
	if ((result = ts_process_call(funcctx)) != (Datum) 0)
		SRF_RETURN_NEXT(funcctx, result);
	SRF_RETURN_DONE(funcctx);
}


/*
 * Triggers for automatic update of a tsvector column from text column(s)
 *
 * Trigger arguments are either
 *		name of tsvector col, name of tsconfig to use, name(s) of text col(s)
 *		name of tsvector col, name of regconfig col, name(s) of text col(s)
 * ie, tsconfig can either be specified by name, or indirectly as the
 * contents of a regconfig field in the row.  If the name is used, it must
 * be explicitly schema-qualified.
 */
Datum
tsvector_update_trigger_byid(PG_FUNCTION_ARGS)
{
	return tsvector_update_trigger(fcinfo, false);
}

Datum
tsvector_update_trigger_bycolumn(PG_FUNCTION_ARGS)
{
	return tsvector_update_trigger(fcinfo, true);
}

static Datum
tsvector_update_trigger(PG_FUNCTION_ARGS, bool config_column)
{
	TriggerData *trigdata;
	Trigger    *trigger;
	Relation	rel;
	HeapTuple	rettuple = NULL;
	int			tsvector_attr_num,
				i;
	ParsedText	prs;
	Datum		datum;
	bool		isnull;
	text	   *txt;
	Oid			cfgId;

	/* Check call context */
	if (!CALLED_AS_TRIGGER(fcinfo))		/* internal error */
		elog(ERROR, "tsvector_update_trigger: not fired by trigger manager");

	trigdata = (TriggerData *) fcinfo->context;
	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		elog(ERROR, "tsvector_update_trigger: must be fired for row");
	if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		elog(ERROR, "tsvector_update_trigger: must be fired BEFORE event");

	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		rettuple = trigdata->tg_trigtuple;
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		rettuple = trigdata->tg_newtuple;
	else
		elog(ERROR, "tsvector_update_trigger: must be fired for INSERT or UPDATE");

	trigger = trigdata->tg_trigger;
	rel = trigdata->tg_relation;

	if (trigger->tgnargs < 3)
		elog(ERROR, "tsvector_update_trigger: arguments must be tsvector_field, ts_config, text_field1, ...)");

	/* Find the target tsvector column */
	tsvector_attr_num = SPI_fnumber(rel->rd_att, trigger->tgargs[0]);
	if (tsvector_attr_num == SPI_ERROR_NOATTRIBUTE)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("tsvector column \"%s\" does not exist",
						trigger->tgargs[0])));
	if (!IsBinaryCoercible(SPI_gettypeid(rel->rd_att, tsvector_attr_num),
						  TSVECTOROID))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" is not of tsvector type",
						trigger->tgargs[0])));

	/* Find the configuration to use */
	if (config_column)
	{
		int			config_attr_num;

		config_attr_num = SPI_fnumber(rel->rd_att, trigger->tgargs[1]);
		if (config_attr_num == SPI_ERROR_NOATTRIBUTE)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("configuration column \"%s\" does not exist",
							trigger->tgargs[1])));
		if (!IsBinaryCoercible(SPI_gettypeid(rel->rd_att, config_attr_num),
							  REGCONFIGOID))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" is not of regconfig type",
							trigger->tgargs[1])));

		datum = SPI_getbinval(rettuple, rel->rd_att, config_attr_num, &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("configuration column \"%s\" must not be null",
							trigger->tgargs[1])));
		cfgId = DatumGetObjectId(datum);
	}
	else
	{
		List	   *names;

		names = stringToQualifiedNameList(trigger->tgargs[1]);
		/* require a schema so that results are not search path dependent */
		if (list_length(names) < 2)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("text search configuration name \"%s\" must be schema-qualified",
							trigger->tgargs[1])));
		cfgId = get_ts_config_oid(names, false);
	}

	/* initialize parse state */
	prs.lenwords = 32;
	prs.curwords = 0;
	prs.pos = 0;
	prs.words = (ParsedWord *) palloc(sizeof(ParsedWord) * prs.lenwords);

	/* find all words in indexable column(s) */
	for (i = 2; i < trigger->tgnargs; i++)
	{
		int			numattr;

		numattr = SPI_fnumber(rel->rd_att, trigger->tgargs[i]);
		if (numattr == SPI_ERROR_NOATTRIBUTE)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist",
							trigger->tgargs[i])));
		if (!IsBinaryCoercible(SPI_gettypeid(rel->rd_att, numattr), TEXTOID))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" is not of a character type",
							trigger->tgargs[i])));

		datum = SPI_getbinval(rettuple, rel->rd_att, numattr, &isnull);
		if (isnull)
			continue;

		txt = DatumGetTextP(datum);

		parsetext(cfgId, &prs, VARDATA(txt), VARSIZE(txt) - VARHDRSZ);

		if (txt != (text *) DatumGetPointer(datum))
			pfree(txt);
	}

	/* make tsvector value */
	if (prs.curwords)
	{
		datum = PointerGetDatum(make_tsvector(&prs));
		rettuple = SPI_modifytuple(rel, rettuple, 1, &tsvector_attr_num,
								   &datum, NULL);
		pfree(DatumGetPointer(datum));
	}
	else
	{
		TSVector	out = palloc(CALCDATASIZE(0, 0));

		SET_VARSIZE(out, CALCDATASIZE(0, 0));
		out->size = 0;
		datum = PointerGetDatum(out);
		rettuple = SPI_modifytuple(rel, rettuple, 1, &tsvector_attr_num,
								   &datum, NULL);
		pfree(prs.words);
	}

	if (rettuple == NULL)		/* internal error */
		elog(ERROR, "tsvector_update_trigger: %d returned by SPI_modifytuple",
			 SPI_result);

	return PointerGetDatum(rettuple);
}

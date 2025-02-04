#include "postgres.h"
#include "fmgr.h"
#include "utils/tuplesort.h"
#include "nodes/execnodes.h"
#include "funcapi.h"
#include "access/htup_details.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_proc.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"
#include "utils/palloc.h"
#include "catalog/pg_operator.h"

/* Safeguard against overflows when finding mean of two numeric values */
#define INT_MEAN(i1, i2) (i1/2 + i2/2 + (i1%2 + i2%2)/2)
#define FLOAT_MEAN(f1, f2) (f1/2 + f2/2)

/* We need comparison operators for TIMESTAMP and numeric types */
#define TIMESTAMPTZOID_LESS_OP 1322
#define NUMERICOID_LESS_OP 1754

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(median_transfn);
PG_FUNCTION_INFO_V1(median_finalfn);

/*
 * The number of entries participating in the sort.
 */
static int64 numberOfRows = 0;

/*
 * Median state transfer function.
 *
 * This function is called for every value in the set that we are calculating
 * the median for. On first call, the aggregate state, if any, needs to be
 * initialized.
 */
Datum
median_transfn(PG_FUNCTION_ARGS)
{
	Tuplesortstate *state;
	Datum		datum;
	Oid			arg_type,
				sortOp = InvalidOid;
	MemoryContext agg_context,
				prev_context;;


	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "median_transfn called in non-aggregate context");

	prev_context = MemoryContextSwitchTo(agg_context);

	/* SortState has been created? */
	if (PG_ARGISNULL(0))
	{
		/* If we cant determine the input data type, report an error */
		arg_type = get_fn_expr_argtype(fcinfo->flinfo, 1);

		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine input data type")));

		/* Set the comparison operator for each supported data type */
		switch (arg_type)
		{
			case TEXTOID:
				sortOp = TextLessOperator;
				break;

			case VARCHAROID:
				sortOp = BpcharPatternLessOperator;
				break;

			case INT2OID:
			case INT4OID:
				sortOp = Int4LessOperator;
				break;

			case INT8OID:
				sortOp = Int8LessOperator;
				break;

			case FLOAT4OID:
			case FLOAT8OID:
				sortOp = Float8LessOperator;
				break;

			case NUMERICOID:
				sortOp = NUMERICOID_LESS_OP;
				break;

			case DATEOID:
			case TIMEOID:
			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
				sortOp = TIMESTAMPTZOID_LESS_OP;
				break;

			default:
				ereport(ERROR,
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("Computing Median is not supported on %s data type.",
							   format_type_be(arg_type)));
		}

		state = tuplesort_begin_datum(arg_type,
									  sortOp,
									  fcinfo->fncollation,
									  true,
									  work_mem,
									  NULL,
									  TUPLESORT_NONE);
		numberOfRows = 0;
	}
	else
	{
		state = (Tuplesortstate *) PG_GETARG_POINTER(0);
	}

	/* fast path for NULLs */
	if (PG_ARGISNULL(1))
	{
		MemoryContextSwitchTo(prev_context);
		PG_RETURN_POINTER(state);
	}

	datum = PG_GETARG_DATUM(1);
	if (datum)
	{
		tuplesort_putdatum(state, datum, false);
		numberOfRows++;
	}

	MemoryContextSwitchTo(prev_context);
	PG_RETURN_POINTER(state);
}

/*
 * Median final function.
 *
 * This function is called after all values in the median set has been
 * processed by the state transfer function. It should perform any necessary
 * post processing and clean up any temporary state.
 */
Datum
median_finalfn(PG_FUNCTION_ARGS)
{
	MemoryContext agg_context;
	Tuplesortstate *state;
	Datum		datum,
				mid1,
				mid2;
	bool		isnull,
				isnull1,
				isnull2;
	Oid			arg_type;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "median_finalfn called in non-aggregate context");

	state = (Tuplesortstate *) PG_GETARG_POINTER(0);

	if (!state)
		PG_RETURN_NULL();

	/* Sort the values so that the median value(s) will be in the middle */
	tuplesort_performsort(state);

	arg_type = get_fn_expr_argtype(fcinfo->flinfo, 1);

	/*
	 * If we have odd number of entries, we just have to find the middle
	 * value. If we have even number of entries, but the data is of CHAR
	 * types, then the median can be either of the middle values.
	 */
	if (numberOfRows % 2 == 1 ||
		(numberOfRows % 2 == 0 && (arg_type == TEXTOID || arg_type == VARCHAROID)))
	{
		tuplesort_skiptuples(state, (numberOfRows - 1) / 2, true);
		tuplesort_getdatum(state, true, true, &datum, &isnull, NULL);
		tuplesort_end(state);
		PG_RETURN_DATUM(datum);
	}
	else if (numberOfRows % 2 == 0)
	{
		tuplesort_skiptuples(state, (numberOfRows / 2) - 1, true);

		tuplesort_getdatum(state, true, true, &mid1, &isnull1, NULL);
		tuplesort_getdatum(state, true, true, &mid2, &isnull2, NULL);
		tuplesort_end(state);

		/*
		 * Do we want to include nulls when computing median? If yes, the
		 * following code will safeguard us against any operation on nulls.
		 *
		 * if (isnull1) PG_RETURN_DATUM(mid2); if (isnull2)
		 * PG_RETURN_DATUM(mid1);
		 */

		switch (arg_type)
		{
			case INT2OID:
				{
					int16		i1,
								i2,
								iretval;

					i1 = DatumGetInt16(mid1);
					i2 = DatumGetInt16(mid2);
					iretval = INT_MEAN(i1, i2);
					PG_RETURN_INT16(iretval);
				}
			case INT4OID:
				{
					int32		i1,
								i2,
								iretval;

					i1 = DatumGetInt32(mid1);
					i2 = DatumGetInt32(mid2);
					iretval = INT_MEAN(i1, i2);
					PG_RETURN_INT32(iretval);
				}
			case INT8OID:
				{
					int64		i1,
								i2,
								iretval;

					i1 = DatumGetInt64(mid1);
					i2 = DatumGetInt64(mid2);
					iretval = INT_MEAN(i1, i2);
					PG_RETURN_INT64(iretval);
				}
			case FLOAT4OID:
				{
					float4		f1,
								f2,
								fretval;

					f1 = DatumGetFloat4(mid1);
					f2 = DatumGetFloat4(mid2);
					fretval = FLOAT_MEAN(f1, f2);
					PG_RETURN_FLOAT4(fretval);
				}
			case FLOAT8OID:
				{
					float8		f1,
								f2,
								fretval;

					f1 = DatumGetFloat8(mid1);
					f2 = DatumGetFloat8(mid2);
					fretval = FLOAT_MEAN(f1, f2);
					PG_RETURN_FLOAT8(fretval);
				}
			case NUMERICOID:
				{
					Datum		div1,
								div2,
								retval,
								divisor;

					divisor = DirectFunctionCall1(int4_numeric, 2);
					div1 = DirectFunctionCall2(numeric_div, mid1, divisor);
					div2 = DirectFunctionCall2(numeric_div, mid2, divisor);
					retval = DirectFunctionCall2(numeric_add, div1, div2);
					PG_RETURN_DATUM(retval);
				}
			case DATEOID:
			case TIMEOID:
			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
				{
					Datum		diff,
								halfdiff,
								retval;

					diff = DirectFunctionCall2(timestamp_mi, mid2, mid1);
					halfdiff = DirectFunctionCall2(interval_div, diff, Float8GetDatum((double) 2));
					retval = DirectFunctionCall2(timestamp_pl_interval, mid1, halfdiff);
					PG_RETURN_DATUM(retval);
				}
			default:
				{
					ereport(ERROR,
							errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("Computing Median is not supported on %s data type.",
								   format_type_be(arg_type)));
				}
		}
	}
	PG_RETURN_NULL();
}

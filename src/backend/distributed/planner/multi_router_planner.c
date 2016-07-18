/*-------------------------------------------------------------------------
 *
 * multi_router_planner.c
 *
 * This file contains functions to plan single shard queries
 * including distributed table modifications.
 *
 * Copyright (c) 2014-2016, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "c.h"

#include <stddef.h>

#if (PG_VERSION_NUM >= 90500 && PG_VERSION_NUM < 90600)
#include "access/stratnum.h"
#else
#include "access/skey.h"
#endif
#include "access/xact.h"
#include "distributed/citus_clauses.h"
#include "catalog/pg_type.h"
#include "distributed/citus_nodes.h"
#include "distributed/citus_nodefuncs.h"
#include "distributed/master_metadata_utility.h"
#include "distributed/metadata_cache.h"
#include "distributed/multi_join_order.h"
#include "distributed/multi_logical_planner.h"
#include "distributed/multi_physical_planner.h"
#include "distributed/multi_router_executor.h"
#include "distributed/multi_router_planner.h"
#include "distributed/listutils.h"
#include "distributed/citus_ruleutils.h"
#include "distributed/relay_utility.h"
#include "distributed/resource_lock.h"
#include "distributed/shardinterval_utils.h"
#include "executor/execdesc.h"
#include "lib/stringinfo.h"
#if (PG_VERSION_NUM >= 90500)
#include "nodes/makefuncs.h"
#endif
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "storage/lock.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "catalog/pg_proc.h"
#include "optimizer/planmain.h"


typedef struct WalkerState
{
	bool containsVar;
	bool varArgument;
	bool badCoalesce;
} WalkerState;


/* planner functions forward declarations */
static bool MasterIrreducibleExpression(Node *expression, bool *varArgument,
										bool *badCoalesce);
static bool MasterIrreducibleExpressionWalker(Node *expression, WalkerState *state);
static char MostPermissiveVolatileFlag(char left, char right);
static Task * RouterModifyTask(Query *originalQuery, Query *query,
							   RelationRestrictionContext *restrictionContext);
static ShardInterval * TargetShardIntervalForModify(Query *query, RelationRestrictionContext *restrictionContext);
static List * QueryRestrictList(Query *query, RelationRestriction *relationRestriction);
static bool FastShardPruningPossible(CmdType commandType, char partitionMethod);
static ShardInterval * FastShardPruning(Oid distributedTableId,
										Const *partionColumnValue);
static Oid ExtractFirstDistributedTableId(Query *query);
static RangeTblEntry * ExtractFirstDistributedTableRte(Query *query);
static Const * ExtractInsertPartitionValue(Query *query, Var *partitionColumn);
static Task * RouterSelectTask(Query *originalQuery, Query *query,
							   RelationRestrictionContext *restrictionContext,
							   List **placementList);
static List * TargetShardIntervalsForQuery(Query *query,
										   RelationRestrictionContext *restrictionContext);
static List * WorkersContainingAllShards(List *prunedShardIntervalsList);
static bool UpdateRelationNames(Node *node,
								RelationRestrictionContext *restrictionContext);
static Job * RouterQueryJob(Query *query, Task *task, List *placementList);
static bool MultiRouterPlannableQuery(Query *query, MultiExecutorType taskExecutorType,
									  RelationRestrictionContext *restrictionContext);


/*
 * MultiRouterPlanCreate creates a physical plan for given query. The created plan is
 * either a modify task that changes a single shard, or a router task that returns
 * query results from a single shard. Supported modify queries (insert/update/delete)
 * are router plannable by default. If query is not router plannable then the function
 * returns NULL.
 */
MultiPlan *
MultiRouterPlanCreate(Query *originalQuery, Query *query,
					  MultiExecutorType taskExecutorType,
					  RelationRestrictionContext *restrictionContext)
{
	Task *task = NULL;
	Job *job = NULL;
	MultiPlan *multiPlan = NULL;
	CmdType commandType = query->commandType;
	bool modifyTask = false;
	Query *jobQuery = NULL;
	List *placementList = NIL;

	bool routerPlannable = MultiRouterPlannableQuery(query, taskExecutorType,
													 restrictionContext);
	if (!routerPlannable)
	{
		return NULL;
	}


	if (commandType == CMD_INSERT || commandType == CMD_UPDATE ||
		commandType == CMD_DELETE)
	{
		modifyTask = true;
	}

	if (modifyTask)
	{
		ErrorIfModifyQueryNotSupported(query);
		task = RouterModifyTask(originalQuery, query, restrictionContext);
		jobQuery = originalQuery;
	}
	else
	{
		Assert(commandType == CMD_SELECT);

		task = RouterSelectTask(originalQuery, query, restrictionContext, &placementList);
		jobQuery = originalQuery;
	}

	if (task == NULL)
	{
		return NULL;
	}

	ereport(DEBUG2, (errmsg("Creating router plan")));

	job = RouterQueryJob(jobQuery, task, placementList);

	multiPlan = CitusMakeNode(MultiPlan);
	multiPlan->workerJob = job;
	multiPlan->masterQuery = NULL;
	multiPlan->masterTableName = NULL;

	return multiPlan;
}


/*
 * ErrorIfModifyQueryNotSupported checks if the query contains unsupported features,
 * and errors out if it does.
 */
void
ErrorIfModifyQueryNotSupported(Query *queryTree)
{
	Oid distributedTableId = ExtractFirstDistributedTableId(queryTree);
	uint32 rangeTableId = 1;
	Var *partitionColumn = PartitionColumn(distributedTableId, rangeTableId);
	List *rangeTableList = NIL;
	ListCell *rangeTableCell = NULL;
	bool hasValuesScan = false;
	uint32 queryTableCount = 0;
	bool specifiesPartitionValue = false;
#if (PG_VERSION_NUM >= 90500)
	ListCell *setTargetCell = NULL;
	List *onConflictSet = NIL;
	Node *arbiterWhere = NULL;
	Node *onConflictWhere = NULL;
#endif

	CmdType commandType = queryTree->commandType;
	Assert(commandType == CMD_INSERT || commandType == CMD_UPDATE ||
		   commandType == CMD_DELETE);

	/*
	 * Reject subqueries which are in SELECT or WHERE clause.
	 * Queries which include subqueries in FROM clauses are rejected below.
	 */
	if (queryTree->hasSubLinks == true)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot perform distributed planning for the given"
							   " modification"),
						errdetail("Subqueries are not supported in distributed"
								  " modifications.")));
	}

	/* reject queries which include CommonTableExpr */
	if (queryTree->cteList != NIL)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot perform distributed planning for the given"
							   " modification"),
						errdetail("Common table expressions are not supported in"
								  " distributed modifications.")));
	}

	/* extract range table entries */
	ExtractRangeTableEntryWalker((Node *) queryTree, &rangeTableList);

	foreach(rangeTableCell, rangeTableList)
	{
		RangeTblEntry *rangeTableEntry = (RangeTblEntry *) lfirst(rangeTableCell);
		if (rangeTableEntry->rtekind == RTE_RELATION)
		{
			queryTableCount++;
		}
		else if (rangeTableEntry->rtekind == RTE_VALUES)
		{
			hasValuesScan = true;
		}
		else
		{
			/*
			 * Error out for rangeTableEntries that we do not support.
			 * We do not explicitly specify "in FROM clause" in the error detail
			 * for the features that we do not support at all (SUBQUERY, JOIN).
			 * We do not need to check for RTE_CTE because all common table expressions
			 * are rejected above with queryTree->cteList check.
			 */
			char *rangeTableEntryErrorDetail = NULL;
			if (rangeTableEntry->rtekind == RTE_SUBQUERY)
			{
				rangeTableEntryErrorDetail = "Subqueries are not supported in"
											 " distributed modifications.";
			}
			else if (rangeTableEntry->rtekind == RTE_JOIN)
			{
				rangeTableEntryErrorDetail = "Joins are not supported in distributed"
											 " modifications.";
			}
			else if (rangeTableEntry->rtekind == RTE_FUNCTION)
			{
				rangeTableEntryErrorDetail = "Functions must not appear in the FROM"
											 " clause of a distributed modifications.";
			}
			else
			{
				rangeTableEntryErrorDetail = "Unrecognized range table entry.";
			}

			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot perform distributed planning for the given"
								   " modifications"),
							errdetail("%s", rangeTableEntryErrorDetail)));
		}
	}

	/*
	 * Reject queries which involve joins. Note that UPSERTs are exceptional for this case.
	 * Queries like "INSERT INTO table_name ON CONFLICT DO UPDATE (col) SET other_col = ''"
	 * contains two range table entries, and we have to allow them.
	 */
	if (commandType != CMD_INSERT && queryTableCount != 1)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot perform distributed planning for the given"
							   " modification"),
						errdetail("Joins are not supported in distributed "
								  "modifications.")));
	}

	/* reject queries which involve multi-row inserts */
	if (hasValuesScan)
	{
		/*
		 * NB: If you remove this check you must also change the checks further in this
		 * method and ensure that VOLATILE function calls aren't allowed in INSERT
		 * statements. Currently they're allowed but the function call is replaced
		 * with a constant, and if you're inserting multiple rows at once the function
		 * should return a different value for each row.
		 */
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot perform distributed planning for the given"
							   " modification"),
						errdetail("Multi-row INSERTs to distributed tables are not "
								  "supported.")));
	}

	if (commandType == CMD_INSERT || commandType == CMD_UPDATE ||
		commandType == CMD_DELETE)
	{
		bool hasVarArgument = false; /* A STABLE function is passed a Var argument */
		bool hasBadCoalesce = false; /* CASE/COALESCE passed a mutable function */
		FromExpr *joinTree = NULL;
		ListCell *targetEntryCell = NULL;

		foreach(targetEntryCell, queryTree->targetList)
		{
			TargetEntry *targetEntry = (TargetEntry *) lfirst(targetEntryCell);

			/* skip resjunk entries: UPDATE adds some for ctid, etc. */
			if (targetEntry->resjunk)
			{
				continue;
			}

			if (commandType == CMD_UPDATE &&
				contain_volatile_functions((Node *) targetEntry->expr))
			{
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("functions used in UPDATE queries on distributed "
									   "tables must not be VOLATILE")));
			}

			if (commandType == CMD_UPDATE &&
				targetEntry->resno == partitionColumn->varattno)
			{
				specifiesPartitionValue = true;
			}

			if (targetEntry->resno == partitionColumn->varattno &&
				!IsA(targetEntry->expr, Const))
			{
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("values given for the partition column must be"
									   " constants or constant expressions")));
			}

			if (commandType == CMD_UPDATE &&
				MasterIrreducibleExpression((Node *) targetEntry->expr,
											&hasVarArgument, &hasBadCoalesce))
			{
				Assert(hasVarArgument || hasBadCoalesce);
			}
		}

		joinTree = queryTree->jointree;
		if (joinTree != NULL)
		{
			if (contain_volatile_functions(joinTree->quals))
			{
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("functions used in the WHERE clause of "
									   "modification queries on distributed tables "
									   "must not be VOLATILE")));
			}
			else if (MasterIrreducibleExpression(joinTree->quals, &hasVarArgument,
												 &hasBadCoalesce))
			{
				Assert(hasVarArgument || hasBadCoalesce);
			}
		}

		if (hasVarArgument)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("STABLE functions used in UPDATE queries"
								   " cannot be called with column references")));
		}

		if (hasBadCoalesce)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("non-IMMUTABLE functions are not allowed in CASE or"
								   " COALESCE statements")));
		}

		if (contain_mutable_functions((Node *) queryTree->returningList))
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("non-IMMUTABLE functions are not allowed in the"
								   " RETURNING clause")));
		}
	}

#if (PG_VERSION_NUM >= 90500)
	if (commandType == CMD_INSERT && queryTree->onConflict != NULL)
	{
		onConflictSet = queryTree->onConflict->onConflictSet;
		arbiterWhere = queryTree->onConflict->arbiterWhere;
		onConflictWhere = queryTree->onConflict->onConflictWhere;
	}

	/*
	 * onConflictSet is expanded via expand_targetlist() on the standard planner.
	 * This ends up adding all the columns to the onConflictSet even if the user
	 * does not explicitly state the columns in the query.
	 *
	 * The following loop simply allows "DO UPDATE SET part_col = table.part_col"
	 * types of elements in the target list, which are added by expand_targetlist().
	 * Any other attempt to update partition column value is forbidden.
	 */
	foreach(setTargetCell, onConflictSet)
	{
		TargetEntry *setTargetEntry = (TargetEntry *) lfirst(setTargetCell);

		if (setTargetEntry->resno == partitionColumn->varattno)
		{
			Expr *setExpr = setTargetEntry->expr;
			if (IsA(setExpr, Var) &&
				((Var *) setExpr)->varattno == partitionColumn->varattno)
			{
				specifiesPartitionValue = false;
			}
			else
			{
				specifiesPartitionValue = true;
			}
		}
		else
		{
			/*
			 * Similarly, allow  "DO UPDATE SET col_1 = table.col_1" types of
			 * target list elements. Note that, the following check allows
			 * "DO UPDATE SET col_1 = table.col_2", which is not harmful.
			 */
			if (IsA(setTargetEntry->expr, Var))
			{
				continue;
			}
			else if (contain_mutable_functions((Node *) setTargetEntry->expr))
			{
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("functions used in the DO UPDATE SET clause of "
									   "INSERTs on distributed tables must be marked "
									   "IMMUTABLE")));
			}
		}
	}

	/* error if either arbiter or on conflict WHERE contains a mutable function */
	if (contain_mutable_functions((Node *) arbiterWhere) ||
		contain_mutable_functions((Node *) onConflictWhere))
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("functions used in the WHERE clause of the ON CONFLICT "
							   "clause of INSERTs on distributed tables must be marked "
							   "IMMUTABLE")));
	}
#endif

	if (specifiesPartitionValue)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("modifying the partition value of rows is not allowed")));
	}
}


/*
 * If the expression contains STABLE functions which accept any parameters derived from a
 * Var returns true and sets varArgument.
 *
 * If the expression contains a CASE or COALESCE which invoke non-IMMUTABLE functions
 * returns true and sets badCoalesce.
 *
 * Assumes the expression contains no VOLATILE functions.
 *
 * Var's are allowed, but only if they are passed solely to IMMUTABLE functions
 *
 * We special-case CASE/COALESCE because those are evaluated lazily. We could evaluate
 * CASE/COALESCE expressions which don't reference Vars, or partially evaluate some
 * which do, but for now we just error out. That makes both the code and user-education
 * easier.
 */
static bool
MasterIrreducibleExpression(Node *expression, bool *varArgument, bool *badCoalesce)
{
	bool result;
	WalkerState data;
	data.containsVar = data.varArgument = data.badCoalesce = false;

	result = MasterIrreducibleExpressionWalker(expression, &data);

	*varArgument |= data.varArgument;
	*badCoalesce |= data.badCoalesce;
	return result;
}


static bool
MasterIrreducibleExpressionWalker(Node *expression, WalkerState *state)
{
	char volatileFlag = 0;
	WalkerState childState = { false, false, false };
	bool containsDisallowedFunction = false;

	if (expression == NULL)
	{
		return false;
	}

	if (IsA(expression, CoalesceExpr))
	{
		CoalesceExpr *expr = (CoalesceExpr *) expression;

		if (contain_mutable_functions((Node *) (expr->args)))
		{
			state->badCoalesce = true;
			return true;
		}
		else
		{
			/*
			 * There's no need to recurse. Since there are no STABLE functions
			 * varArgument will never be set.
			 */
			return false;
		}
	}

	if (IsA(expression, CaseExpr))
	{
		if (contain_mutable_functions(expression))
		{
			state->badCoalesce = true;
			return true;
		}

		return false;
	}

	if (IsA(expression, Var))
	{
		state->containsVar = true;
		return false;
	}

	/*
	 * In order for statement replication to give us consistent results it's important
	 * that we either disallow or evaluate on the master anything which has a volatility
	 * category above IMMUTABLE. Newer versions of postgres might add node types which
	 * should be checked in this function.
	 *
	 * Look through contain_mutable_functions_walker or future PG's equivalent for new
	 * node types before bumping this version number to fix compilation.
	 *
	 * Once you've added them to this check, make sure you also evaluate them in the
	 * executor!
	 */
	StaticAssertStmt(PG_VERSION_NUM < 90600, "When porting to a newer PG this section"
											 " needs to be reviewed.");

	if (IsA(expression, OpExpr))
	{
		OpExpr *expr = (OpExpr *) expression;

		set_opfuncid(expr);
		volatileFlag = func_volatile(expr->opfuncid);
	}
	else if (IsA(expression, FuncExpr))
	{
		FuncExpr *expr = (FuncExpr *) expression;

		volatileFlag = func_volatile(expr->funcid);
	}
	else if (IsA(expression, DistinctExpr))
	{
		/*
		 * to exercise this, you need to create a custom type for which the '=' operator
		 * is STABLE/VOLATILE
		 */
		DistinctExpr *expr = (DistinctExpr *) expression;

		set_opfuncid((OpExpr *) expr);  /* rely on struct equivalence */
		volatileFlag = func_volatile(expr->opfuncid);
	}
	else if (IsA(expression, NullIfExpr))
	{
		/*
		 * same as above, exercising this requires a STABLE/VOLATILE '=' operator
		 */
		NullIfExpr *expr = (NullIfExpr *) expression;

		set_opfuncid((OpExpr *) expr);  /* rely on struct equivalence */
		volatileFlag = func_volatile(expr->opfuncid);
	}
	else if (IsA(expression, ScalarArrayOpExpr))
	{
		/*
		 * to exercise this you need to CREATE OPERATOR with a binary predicate
		 * and use it within an ANY/ALL clause.
		 */
		ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) expression;

		set_sa_opfuncid(expr);
		volatileFlag = func_volatile(expr->opfuncid);
	}
	else if (IsA(expression, CoerceViaIO))
	{
		/*
		 * to exercise this you need to use a type with a STABLE/VOLATILE intype or
		 * outtype.
		 */
		CoerceViaIO *expr = (CoerceViaIO *) expression;
		Oid iofunc;
		Oid typioparam;
		bool typisvarlena;

		/* check the result type's input function */
		getTypeInputInfo(expr->resulttype,
						 &iofunc, &typioparam);
		volatileFlag = MostPermissiveVolatileFlag(volatileFlag, func_volatile(iofunc));

		/* check the input type's output function */
		getTypeOutputInfo(exprType((Node *) expr->arg),
						  &iofunc, &typisvarlena);
		volatileFlag = MostPermissiveVolatileFlag(volatileFlag, func_volatile(iofunc));
	}
	else if (IsA(expression, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *expr = (ArrayCoerceExpr *) expression;

		if (OidIsValid(expr->elemfuncid))
		{
			volatileFlag = func_volatile(expr->elemfuncid);
		}
	}
	else if (IsA(expression, RowCompareExpr))
	{
		RowCompareExpr *rcexpr = (RowCompareExpr *) expression;
		ListCell *opid;

		foreach(opid, rcexpr->opnos)
		{
			volatileFlag = MostPermissiveVolatileFlag(volatileFlag,
													  op_volatile(lfirst_oid(opid)));
		}
	}
	else if (IsA(expression, Query))
	{
		/* subqueries aren't allowed and fail before control reaches this point */
		Assert(false);
	}

	if (volatileFlag == PROVOLATILE_VOLATILE)
	{
		/* the caller should have already checked for this */
		Assert(false);
	}
	else if (volatileFlag == PROVOLATILE_STABLE)
	{
		containsDisallowedFunction =
			expression_tree_walker(expression,
								   MasterIrreducibleExpressionWalker,
								   &childState);

		if (childState.containsVar)
		{
			state->varArgument = true;
		}

		state->badCoalesce |= childState.badCoalesce;
		state->varArgument |= childState.varArgument;

		return (containsDisallowedFunction || childState.containsVar);
	}

	/* keep traversing */
	return expression_tree_walker(expression,
								  MasterIrreducibleExpressionWalker,
								  state);
}


/*
 * Return the most-pessimistic volatility flag of the two params.
 *
 * for example: given two flags, if one is stable and one is volatile, an expression
 * involving both is volatile.
 */
char
MostPermissiveVolatileFlag(char left, char right)
{
	if (left == PROVOLATILE_VOLATILE || right == PROVOLATILE_VOLATILE)
	{
		return PROVOLATILE_VOLATILE;
	}
	else if (left == PROVOLATILE_STABLE || right == PROVOLATILE_STABLE)
	{
		return PROVOLATILE_STABLE;
	}
	else
	{
		return PROVOLATILE_IMMUTABLE;
	}
}


/*
 * RouterModifyTask builds a Task to represent a modification performed by
 * the provided query against the provided shard interval. This task contains
 * shard-extended deparsed SQL to be run during execution.
 */
static Task *
RouterModifyTask(Query *originalQuery, Query *query,
				 RelationRestrictionContext *restrictionContext)
{
	ShardInterval *shardInterval = TargetShardIntervalForModify(query, restrictionContext);
	uint64 shardId = shardInterval->shardId;
	StringInfo queryString = makeStringInfo();
	Task *modifyTask = NULL;
	bool upsertQuery = false;
	bool requiresMasterEvaluation = RequiresMasterEvaluation(originalQuery);

	/* grab shared metadata lock to stop concurrent placement additions */
	LockShardDistributionMetadata(shardId, ShareLock);

#if (PG_VERSION_NUM >= 90500)
	if (query->onConflict != NULL)
	{
		RangeTblEntry *rangeTableEntry = NULL;

		/* set the flag */
		upsertQuery = true;

		/* setting an alias simplifies deparsing of UPSERTs */
		rangeTableEntry = linitial(originalQuery->rtable);
		if (rangeTableEntry->alias == NULL)
		{
			Alias *alias = makeAlias(UPSERT_ALIAS, NIL);
			rangeTableEntry->alias = alias;
		}
	}
#else
	/* always set to false for PG_VERSION_NUM < 90500 */
	upsertQuery = false;
#endif


	deparse_shard_query(originalQuery, shardInterval->relationId, shardId, queryString);
	ereport(DEBUG4, (errmsg("distributed statement: %s", queryString->data)));

	modifyTask = CitusMakeNode(Task);
	modifyTask->jobId = INVALID_JOB_ID;
	modifyTask->taskId = INVALID_TASK_ID;
	modifyTask->taskType = MODIFY_TASK;
	modifyTask->queryString = queryString->data;
	modifyTask->anchorShardId = shardId;
	modifyTask->dependedTaskList = NIL;
	modifyTask->upsertQuery = upsertQuery;
	modifyTask->requiresMasterEvaluation = requiresMasterEvaluation;

	return modifyTask;
}



/*
 * TargetShardInterval determines the single shard targeted by a provided command.
 * If no matching shards exist, or if the modification targets more than one one
 * shard, this function raises an error depending on the command type.
 */
static ShardInterval *
TargetShardIntervalForModify(Query *query, RelationRestrictionContext *restrictionContext)
{
	CmdType commandType = query->commandType;
	bool selectTask = (commandType == CMD_SELECT);
	List *prunedShardList = NIL;
	int prunedShardCount = 0;
	int shardCount = 0;
	RangeTblEntry *distributedTableRte = ExtractFirstDistributedTableRte(query);
	Oid distributedTableId = distributedTableRte->relid;
	DistTableCacheEntry *cacheEntry = DistributedTableCacheEntry(distributedTableId);
	char partitionMethod = cacheEntry->partitionMethod;
	bool fastShardPruningPossible = false;
	RelationRestriction *relationRestriction = NULL;


	/*
	 * Restriction context does not contain any restriction for insert queries.
	 * It is only applicable to update/delete queries.
	 */
	if (commandType != CMD_INSERT)
	{
		List *relationRestrictionList = restrictionContext->relationRestrictionList;
		relationRestriction = (RelationRestriction *) linitial(relationRestrictionList);
	}


	/* error out if no shards exist for the table */
	shardCount = cacheEntry->shardIntervalArrayLength;
	if (shardCount == 0)
	{
		char *relationName = get_rel_name(distributedTableId);

		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("could not find any shards"),
						errdetail("No shards exist for distributed table \"%s\".",
								  relationName),
						errhint("Run master_create_worker_shards to create shards "
								"and try again.")));
	}

	fastShardPruningPossible = FastShardPruningPossible(query->commandType,
														partitionMethod);
	if (fastShardPruningPossible)
	{
		uint32 rangeTableId = 1;
		Var *partitionColumn = PartitionColumn(distributedTableId, rangeTableId);
		Const *partitionValue = ExtractInsertPartitionValue(query, partitionColumn);
		ShardInterval *shardInterval = FastShardPruning(distributedTableId,
														partitionValue);

		if (shardInterval != NULL)
		{
			prunedShardList = lappend(prunedShardList, shardInterval);
		}
	}
	else
	{
		List *restrictClauseList = QueryRestrictList(query, relationRestriction);
		Index tableId = 1;
		List *shardIntervalList = LoadShardIntervalList(distributedTableId);

		prunedShardList = PruneShardList(distributedTableId, tableId, restrictClauseList,
										 shardIntervalList);
	}

	prunedShardCount = list_length(prunedShardList);
	if (prunedShardCount != 1)
	{
		if (selectTask)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("router executor queries must target exactly one "
								   "shard")));
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("distributed modifications must target exactly one "
								   "shard")));
		}
	}

	return (ShardInterval *) linitial(prunedShardList);
}


/*
 * UseFastShardPruning returns true if the commandType is INSERT and partition method
 * is hash or range.
 */
static bool
FastShardPruningPossible(CmdType commandType, char partitionMethod)
{
	/* we currently only support INSERTs */
	if (commandType != CMD_INSERT)
	{
		return false;
	}

	/* fast shard pruning is only supported for hash and range partitioned tables */
	if (partitionMethod == DISTRIBUTE_BY_HASH || partitionMethod == DISTRIBUTE_BY_RANGE)
	{
		return true;
	}

	return false;
}


/*
 * FastShardPruning is a higher level API for FindShardInterval function. Given the
 * relationId of the distributed table and partitionValue, FastShardPruning function finds
 * the corresponding shard interval that the partitionValue should be in. FastShardPruning
 * returns NULL if no ShardIntervals exist for the given partitionValue.
 */
static ShardInterval *
FastShardPruning(Oid distributedTableId, Const *partitionValue)
{
	DistTableCacheEntry *cacheEntry = DistributedTableCacheEntry(distributedTableId);
	int shardCount = cacheEntry->shardIntervalArrayLength;
	ShardInterval **sortedShardIntervalArray = cacheEntry->sortedShardIntervalArray;
	bool useBinarySearch = false;
	char partitionMethod = cacheEntry->partitionMethod;
	FmgrInfo *shardIntervalCompareFunction = cacheEntry->shardIntervalCompareFunction;
	bool hasUniformHashDistribution = cacheEntry->hasUniformHashDistribution;
	FmgrInfo *hashFunction = NULL;
	ShardInterval *shardInterval = NULL;

	/* determine whether to use binary search */
	if (partitionMethod != DISTRIBUTE_BY_HASH || !hasUniformHashDistribution)
	{
		useBinarySearch = true;
	}

	/* we only need hash functions for hash distributed tables */
	if (partitionMethod == DISTRIBUTE_BY_HASH)
	{
		hashFunction = cacheEntry->hashFunction;
	}

	/*
	 * Call FindShardInterval to find the corresponding shard interval for the
	 * given partition value.
	 */
	shardInterval = FindShardInterval(partitionValue->constvalue,
									  sortedShardIntervalArray, shardCount,
									  partitionMethod,
									  shardIntervalCompareFunction, hashFunction,
									  useBinarySearch);

	return shardInterval;
}


/*
 * QueryRestrictList returns the restriction clauses for the query. For a SELECT
 * statement these are the where-clause expressions. For INSERT statements we
 * build an equality clause based on the partition-column and its supplied
 * insert value.
 */
static List *
QueryRestrictList(Query *query, RelationRestriction *relationRestriction)
{
	List *queryRestrictList = NIL;
	CmdType commandType = query->commandType;

	if (commandType == CMD_INSERT)
	{
		/* build equality expression based on partition column value for row */
		Oid distributedTableId = ExtractFirstDistributedTableId(query);
		uint32 rangeTableId = 1;
		Var *partitionColumn = PartitionColumn(distributedTableId, rangeTableId);
		Const *partitionValue = ExtractInsertPartitionValue(query, partitionColumn);

		OpExpr *equalityExpr = MakeOpExpression(partitionColumn, BTEqualStrategyNumber);

		Node *rightOp = get_rightop((Expr *) equalityExpr);
		Const *rightConst = (Const *) rightOp;
		Assert(IsA(rightOp, Const));

		rightConst->constvalue = partitionValue->constvalue;
		rightConst->constisnull = partitionValue->constisnull;
		rightConst->constbyval = partitionValue->constbyval;

		queryRestrictList = list_make1(equalityExpr);
	}
	else if (commandType == CMD_UPDATE || commandType == CMD_DELETE ||
			 commandType == CMD_SELECT)
	{
		List *baseRestrictList = NIL;

		Assert(relationRestriction != NULL);
		baseRestrictList = relationRestriction->relOptInfo->baserestrictinfo;
		queryRestrictList = get_all_actual_clauses(baseRestrictList);
	}

	return queryRestrictList;
}


/*
 * ExtractFirstDistributedTableId takes a given query, and finds the relationId
 * for the first distributed table in that query. If the function cannot find a
 * distributed table, it returns InvalidOid.
 */
static Oid
ExtractFirstDistributedTableId(Query *query)
{
	Oid distributedTableId = InvalidOid;
	RangeTblEntry *rangeTableEntry = ExtractFirstDistributedTableRte(query);

	if (rangeTableEntry != NULL)
	{
		distributedTableId = rangeTableEntry->relid;
	}

	return distributedTableId;
}


static RangeTblEntry *
ExtractFirstDistributedTableRte(Query *query)
{
	List *rangeTableList = NIL;
	ListCell *rangeTableCell = NULL;
	RangeTblEntry *firstRte = NULL;

	/* extract range table entries */
	ExtractRangeTableEntryWalker((Node *) query, &rangeTableList);

	foreach(rangeTableCell, rangeTableList)
	{
		RangeTblEntry *rangeTableEntry = (RangeTblEntry *) lfirst(rangeTableCell);

		if (IsDistributedTable(rangeTableEntry->relid))
		{
			firstRte = rangeTableEntry;
			break;
		}
	}

	return firstRte;
}


/*
 * ExtractPartitionValue extracts the partition column value from a the target
 * of an INSERT command. If a partition value is missing altogether or is
 * NULL, this function throws an error.
 */
static Const *
ExtractInsertPartitionValue(Query *query, Var *partitionColumn)
{
	Const *partitionValue = NULL;
	TargetEntry *targetEntry = get_tle_by_resno(query->targetList,
												partitionColumn->varattno);
	if (targetEntry != NULL)
	{
		Assert(IsA(targetEntry->expr, Const));

		partitionValue = (Const *) targetEntry->expr;
	}

	if (partitionValue == NULL || partitionValue->constisnull)
	{
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						errmsg("cannot plan INSERT using row with NULL value "
							   "in partition column")));
	}

	return partitionValue;
}


/* RouterSelectTask builds a Task to represent a single shard select query */
static Task *
RouterSelectTask(Query *originalQuery, Query *query,
				 RelationRestrictionContext *restrictionContext,
				 List **placementList)
{
	Task *task = NULL;
	List *prunedRelationShardList = TargetShardIntervalsForQuery(query,
																 restrictionContext);
	StringInfo queryString = makeStringInfo();
	uint64 shardId = INVALID_SHARD_ID;
	bool upsertQuery = false;
	CmdType commandType PG_USED_FOR_ASSERTS_ONLY = query->commandType;
	ListCell *prunedRelationShardListCell = NULL;
	List *workerList = NIL;

	*placementList = NIL;

	if (prunedRelationShardList == NULL)
	{
		return NULL;
	}

	Assert(commandType == CMD_SELECT);

	foreach(prunedRelationShardListCell, prunedRelationShardList)
	{
		List *prunedShardList = (List *) lfirst(prunedRelationShardListCell);
		ShardInterval *shardInterval = NULL;
		if (list_length(prunedShardList) != 1)
		{
			return NULL;
		}

		if (shardId == INVALID_SHARD_ID)
		{
			shardInterval = (ShardInterval *) linitial(prunedShardList);
			shardId = shardInterval->shardId;
		}
	}

	workerList = WorkersContainingAllShards(prunedRelationShardList);
	if (workerList == NIL)
	{
		ereport(DEBUG2, (errmsg("Found no worker with all shard placements")));
		return NULL;
	}

	UpdateRelationNames((Node *) originalQuery, restrictionContext);

	pg_get_query_def(originalQuery, queryString);

	task = CitusMakeNode(Task);
	task->jobId = INVALID_JOB_ID;
	task->taskId = INVALID_TASK_ID;
	task->taskType = ROUTER_TASK;
	task->queryString = queryString->data;
	task->anchorShardId = shardId;
	task->dependedTaskList = NIL;
	task->upsertQuery = upsertQuery;
	task->requiresMasterEvaluation = false;

	*placementList = workerList;

	return task;
}


/*
 * TargetShardIntervalsForQuery performs shard pruning for all referenced relations
 * in the query and returns list of shards per relation. Shard pruning is done based
 * on provided restriction context per relation. The function bails out and returns NULL
 * if any of the relations has no active shards. It also records pruned shard intervals
 * in relation restriction context to be used later on.
 */
static List *
TargetShardIntervalsForQuery(Query *query, RelationRestrictionContext *restrictionContext)
{
	List *prunedRelationShardList = NIL;
	ListCell *restrictionCell = NULL;

	Assert(query->commandType == CMD_SELECT);
	Assert(restrictionContext != NULL);

	foreach(restrictionCell, restrictionContext->relationRestrictionList)
	{
		RelationRestriction *relationResriction =
			(RelationRestriction *) lfirst(restrictionCell);
		Oid relationId = relationResriction->relationId;
		Index tableId = relationResriction->index;
		DistTableCacheEntry *cacheEntry = DistributedTableCacheEntry(relationId);
		int shardCount = cacheEntry->shardIntervalArrayLength;
		List *baseRestrictionList = relationResriction->relOptInfo->baserestrictinfo;
		List *restrictClauseList = get_all_actual_clauses(baseRestrictionList);
		List *shardIntervalList = NIL;
		List *prunedShardList = NIL;

		relationResriction->prunedShardIntervalList = NIL;

		/* error out if no shards exist for any of the relations */
		if (shardCount == 0)
		{
			return NULL;
		}

		shardIntervalList = LoadShardIntervalList(relationId);
		prunedShardList = PruneShardList(relationId, tableId,
										 restrictClauseList,
										 shardIntervalList);

		relationResriction->prunedShardIntervalList = prunedShardList;
		prunedRelationShardList = lappend(prunedRelationShardList, prunedShardList);
	}

	return prunedRelationShardList;
}


/*
 * WorkersContainingAllShards returns list of shard placements that contain all
 * shard intervals provided to the function. It returns NIL if no placement exists.
 */
static List *
WorkersContainingAllShards(List *prunedShardIntervalsList)
{
	ListCell *prunedShardIntervalCell = NULL;
	bool firstShard = true;
	List *workerNodeList = NIL;
	List *placementList = NIL;

	foreach(prunedShardIntervalCell, prunedShardIntervalsList)
	{
		List *shardIntervalList = (List *) lfirst(prunedShardIntervalCell);
		ShardInterval *shardInterval = NULL;
		uint64 shardId = INVALID_SHARD_ID;
		List *shardPlacementList = NIL;
		ListCell *shardPlacementCell = NULL;
		List *shardNodeList = NIL;

		Assert(list_length(shardIntervalList) == 1);

		shardInterval = (ShardInterval *) linitial(shardIntervalList);
		shardId = shardInterval->shardId;

		/* retrieve all active shard placements for this shard */
		shardPlacementList = FinalizedShardPlacementList(shardId);

		/* create nodeName:nodePort tuples for each placement */
		foreach(shardPlacementCell, shardPlacementList)
		{
			ShardPlacement *shardPlacement = (ShardPlacement *) lfirst(
				shardPlacementCell);
			StringInfo nodeInfo = makeStringInfo();
			appendStringInfo(nodeInfo, "%s:%u", shardPlacement->nodeName,
							 shardPlacement->nodePort);
			shardNodeList = lappend(shardNodeList, nodeInfo);
		}

		/*
		 * Perform placement pruning based on string matching on nodeName:nodePort
		 * tuples. We start pruning from all placements of the first relation's shard.
		 * Then for each relation's shard, we compute intersection of the new shards
		 * placement with existing placement list. This operation could have been
		 * done using other methods, but since we do not expect very high replication
		 * factor, iterating over a list and making string comparisons should be the
		 * fastest way around.
		 */
		if (firstShard)
		{
			firstShard = false;
			workerNodeList = shardNodeList;
			placementList = shardPlacementList;
		}
		else
		{
			List *existingWorkerNodeList = workerNodeList;
			ListCell *existingWorkerNodeCell = NULL;
			ListCell *shardNodeCell = NULL;

			/* reset active placement list and active worker node list */
			placementList = NIL;
			workerNodeList = NIL;

			forboth(shardNodeCell, shardNodeList, shardPlacementCell, shardPlacementList)
			{
				ShardPlacement *shardPlacement =
					(ShardPlacement *) lfirst(shardPlacementCell);

				foreach(existingWorkerNodeCell, existingWorkerNodeList)
				{
					StringInfo existingNodeInfo =
						(StringInfo) lfirst(existingWorkerNodeCell);
					StringInfo newNodeInfo = (StringInfo) lfirst(shardNodeCell);

					/*
					 * Append placement to placement list if it is already in existing
					 * placement list.
					 */
					if (strncmp(existingNodeInfo->data, newNodeInfo->data,
								existingNodeInfo->len) == 0)
					{
						placementList = lappend(placementList, shardPlacement);
						workerNodeList = lappend(workerNodeList, existingNodeInfo);
					}
				}
			}
		}

		/*
		 * Bail out if placement list becomes empty. This means there is no worker
		 * containing all shards referecend by the query, hence we can not forward
		 * this query directly to any worker.
		 */
		if (placementList == NIL)
		{
			break;
		}
	}

	return placementList;
}


static bool
UpdateRelationNames(Node *node, RelationRestrictionContext *restrictionContext)
{
	RangeTblEntry *newRte = NULL;
	uint64 shardId = INVALID_SHARD_ID;
	Oid relationId = InvalidOid;
	Oid schemaId = InvalidOid;
	char *relationName = NULL;
	char *schemaName = NULL;
	ListCell *relationRestrictionCell = NULL;
	RelationRestriction *relationRestriction = NULL;
	List *shardIntervalList = NIL;
	ShardInterval *shardInterval = NULL;

	if (node == NULL)
	{
		return false;
	}

	/* want to look at all RTEs, even in subqueries, CTEs and such */
	if (IsA(node, Query))
	{
		return query_tree_walker((Query *) node, UpdateRelationNames, restrictionContext,
								 QTW_EXAMINE_RTES);
	}

	if (!IsA(node, RangeTblEntry))
	{
		return expression_tree_walker(node, UpdateRelationNames, restrictionContext);
	}


	newRte = (RangeTblEntry *) node;

	if (newRte->rtekind != RTE_RELATION)
	{
		return false;
	}

	/*
	 * Search for the restrictions associated with the RTE. There better be
	 * some, otherwise this query wouldn't be elegible as a router query.
	 *
	 * FIXME: We should probably use a hashtable here, to do efficient
	 * lookup.
	 */
	foreach(relationRestrictionCell, restrictionContext->relationRestrictionList)
	{
		relationRestriction =
			(RelationRestriction *) lfirst(relationRestrictionCell);

		if (GetRTEIdentity(relationRestriction->rte) == GetRTEIdentity(newRte))
		{
			break;
		}

		relationRestriction = NULL;
	}

	if (relationRestriction == NULL)
	{
		Relation relation = heap_open(newRte->relid, AccessShareLock);
		TupleDesc tupleDescriptor = RelationGetDescr(relation);
		int columnCount = tupleDescriptor->natts;
		int columnIndex = 0;
		List *valuesList = NIL;
		List *collationList = NIL;

		newRte->rtekind = RTE_VALUES;

		for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
		{
			FormData_pg_attribute *attributeForm = tupleDescriptor->attrs[columnIndex];
			if (!attributeForm->attisdropped)
			{
				Const *constValue = makeNullConst(attributeForm->atttypid,
												  attributeForm->atttypmod,
												  attributeForm->attcollation);
				valuesList = lappend(valuesList, constValue);
				collationList = lappend_oid(collationList, attributeForm->attcollation);
			}
		}


		newRte->values_lists = NIL;
		newRte->values_lists = lappend(newRte->values_lists, valuesList);
		newRte->values_collations = collationList;
		newRte->alias = copyObject(newRte->eref);
		heap_close(relation, AccessShareLock);

		return false;
	}

	Assert(relationRestriction != NULL);

	shardIntervalList = relationRestriction->prunedShardIntervalList;

	Assert(list_length(shardIntervalList) == 1);
	shardInterval = (ShardInterval *) linitial(shardIntervalList);

	shardId = shardInterval->shardId;
	relationId = shardInterval->relationId;
	relationName = get_rel_name(relationId);
	AppendShardIdToName(&relationName, shardId);

	schemaId = get_rel_namespace(relationId);
	schemaName = get_namespace_name(schemaId);

	/* XXX: why is that a good idea? */
	if (strncmp(schemaName, "public", NAMEDATALEN) == 0)
	{
		schemaName = NULL;
	}

	ModifyRangeTblExtraData(newRte, CITUS_RTE_SHARD, schemaName,
							relationName,
							NIL);

	return false;
}


/*
 * RouterQueryJob creates a Job for the specified query to execute the
 * provided single shard select task.
 */
static Job *
RouterQueryJob(Query *query, Task *task, List *placementList)
{
	Job *job = NULL;
	List *taskList = NIL;
	TaskType taskType = task->taskType;

	/*
	 * We send modify task to the first replica, otherwise we choose the target shard
	 * according to task assignment policy. Placement list for select queries are
	 * provided as function parameter.
	 */
	if (taskType == MODIFY_TASK)
	{
		taskList = FirstReplicaAssignTaskList(list_make1(task));
	}
	else
	{
		Assert(placementList != NIL);

		task->taskPlacementList = placementList;
		taskList = list_make1(task);
	}

	job = CitusMakeNode(Job);
	job->dependedJobList = NIL;
	job->jobId = INVALID_JOB_ID;
	job->subqueryPushdown = false;
	job->jobQuery = query;
	job->taskList = taskList;

	return job;
}


/*
 * MultiRouterPlannableQuery returns true if given query can be router plannable.
 * The query is router plannable if it is a select query issued on a hash
 * partitioned distributed table, and it has a exact match comparison on the
 * partition column. This feature is enabled if task executor is set to real-time
 */
bool
MultiRouterPlannableQuery(Query *query, MultiExecutorType taskExecutorType,
						  RelationRestrictionContext *restrictionContext)
{
	CmdType commandType = query->commandType;
	ListCell *relationRestrictionContextCell = NULL;

	if (commandType == CMD_INSERT || commandType == CMD_UPDATE ||
		commandType == CMD_DELETE)
	{
		return true;
	}

	/* FIXME: I tend to think it's time to remove this */
	if (taskExecutorType != MULTI_EXECUTOR_REAL_TIME)
	{
		return false;
	}

	Assert(commandType == CMD_SELECT);

	/*
	 * FIXME, figure out which restrictions we exactly want. FOR UPDATE seems
	 * to make no sense, given the transactional behaviour.
	 */
	if (query->hasForUpdate)
	{
		return false;
	}

	foreach(relationRestrictionContextCell, restrictionContext->relationRestrictionList)
	{
		RelationRestriction *relationRestriction =
			(RelationRestriction *) lfirst(relationRestrictionContextCell);
		RangeTblEntry *rte = relationRestriction->rte;
		if (rte->rtekind == RTE_RELATION)
		{
			/* only hash partitioned tables are supported */
			Oid distributedTableId = rte->relid;
			char partitionMethod = PartitionMethod(distributedTableId);

			if (partitionMethod != DISTRIBUTE_BY_HASH)
			{
				return false;
			}
		}
	}

	return true;
}

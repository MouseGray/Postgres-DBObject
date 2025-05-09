#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "commands/defrem.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

typedef enum AttributeType { atCOLUMN, atEXPRESSION } AttributeType;

typedef enum SortOrder { soNONE, soASC, soDESC } SortOrder;

typedef enum NullsOrder { noNONE, noNULLS_FIRST, noNULLS_LAST } NullsOrder;

typedef struct PGIndexAttribute {
  AttributeType type;
  const char *value;
  const char *collation;
  const char *operator_class;
  SortOrder sortOrder;
  NullsOrder nullsOrder;
} PGIndexAttribute;

typedef struct PGIndexOption {
  char *name;
  char *value;
} PGIndexOption;

typedef struct PGIndex {
  const char *name;
  const char *table;
  const char *schema;
  const char *method;
  bool isUnique;
  PGIndexAttribute *attributes;
  int nattributes;
  PGIndexOption *options;
  int noptions;
  const char *predicate;
} PGIndex;

static char *get_relation_name(Oid relid) {
  char *relname = get_rel_name(relid);

  if (!relname)
    elog(ERROR, "cache lookup failed for relation %u", relid);
  return relname;
}

static char *get_opclass_name(Oid opclass, Oid actual_datatype) {
  HeapTuple ht_opc;
  Form_pg_opclass opcrec;
  char *opcname;
  char *nspname;

  ht_opc = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
  if (!HeapTupleIsValid(ht_opc))
    elog(ERROR, "cache lookup failed for opclass %u", opclass);
  opcrec = (Form_pg_opclass)GETSTRUCT(ht_opc);

  if (!OidIsValid(actual_datatype) ||
      GetDefaultOpClass(actual_datatype, opcrec->opcmethod) != opclass) {
    /* Okay, we need the opclass name.  Do we need to qualify it? */
    return NameStr(opcrec->opcname);
  }
  ReleaseSysCache(ht_opc);
}

static void parse_option(PGIndexOption *option, const char *data) {
  const char *sep;

  sep = strchr(data, '=');
  if (sep) {
    option->name = palloc0(sep - data + 1);
    strncpy(option->name, data, sep - data);
    option->value = pstrdup(sep + 1);
  } else {
    option->name = pstrdup(data);
    option->value = NULL;
  }
}

static PGIndexOption *parse_options(Datum reloptions, int *noptions) {
  PGIndexOption *result;
  Datum *options;

  deconstruct_array(DatumGetArrayTypeP(reloptions), TEXTOID, -1, false, 'i',
                    &options, NULL, noptions);

  result = palloc(sizeof(PGIndexOption) * *noptions);

  for (int i = 0; i < *noptions; i++) {
    char *option = TextDatumGetCString(options[i]);

    parse_option(result + i, option);

    pfree(option);
  }

  return result;
}

static List *get_indexprs(HeapTuple ht_idx) {
  Datum datum;
  char *string;
  List *exprs;

  if (heap_attisnull(ht_idx, Anum_pg_index_indexprs, NULL))
    return NIL;

  datum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx, Anum_pg_index_indexprs);
  string = TextDatumGetCString(datum);
  exprs = (List *)stringToNode(string);
  pfree(string);

  return exprs;
}

static Node *get_indpred(HeapTuple ht_idx) {
  Datum datum;
  char *string;
  Node *pred;

  if (heap_attisnull(ht_idx, Anum_pg_index_indexprs, NULL))
    return NULL;

  datum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx, Anum_pg_index_indexprs);
  string = TextDatumGetCString(datum);
  pred = (Node *)stringToNode(string);
  pfree(string);

  return pred;
}

static PGIndexAttribute *get_attributes(HeapTuple ht_idx, List *context,
                                        IndexAmRoutine *am_routine) {
  Form_pg_index idxrec;

  Datum indcollDatum;
  Datum indclassDatum;
  Datum indoptionDatum;

  oidvector *indcollation;
  oidvector *indclass;
  int2vector *indoption;
  List *indexprs;

  PGIndexAttribute *result;

  idxrec = (Form_pg_index)GETSTRUCT(ht_idx);

  indcollDatum =
      SysCacheGetAttrNotNull(INDEXRELID, ht_idx, Anum_pg_index_indcollation);
  indcollation = (oidvector *)DatumGetPointer(indcollDatum);

  indclassDatum =
      SysCacheGetAttrNotNull(INDEXRELID, ht_idx, Anum_pg_index_indclass);
  indclass = (oidvector *)DatumGetPointer(indclassDatum);

  indoptionDatum =
      SysCacheGetAttrNotNull(INDEXRELID, ht_idx, Anum_pg_index_indoption);
  indoption = (int2vector *)DatumGetPointer(indoptionDatum);

  indexprs = get_indexprs(ht_idx);

  result = palloc0(sizeof(PGIndexAttribute) * idxrec->indnatts);

  ListCell *indexpr_item = list_head(indexprs);

  for (int attno = 0; attno < idxrec->indnatts; attno++) {
    PGIndexAttribute *attr = result + attno;
    AttrNumber attnum = idxrec->indkey.values[attno];
    int16 opt = indoption->values[attno];
    Oid coll = indcollation->values[attno];
    Oid keycoltype;
    Oid keycolcollation;

    if (attnum != 0) {
      int32 keycoltypmod;

      get_atttypetypmodcoll(idxrec->indrelid, attnum, &keycoltype,
                            &keycoltypmod, &keycolcollation);

      attr->type = atCOLUMN;
      attr->value = get_attname(idxrec->indrelid, attnum, false);
    } else {
      Node *indexkey;

      if (indexpr_item == NULL)
        elog(ERROR, "too few entries in indexprs list");

      indexkey = (Node *)lfirst(indexpr_item);
      indexpr_item = lnext(indexprs, indexpr_item);

      keycoltype = exprType(indexkey);
      keycolcollation = exprCollation(indexkey);

      attr->type = atEXPRESSION;
      attr->value = deparse_expression(indexkey, context, false, false);
    }

    if (OidIsValid(coll) && coll != keycolcollation)
      attr->collation = generate_collation_name((coll));

    attr->operator_class =
        get_opclass_name(indclass->values[attno], keycoltype);

    if (am_routine->amcanorder) {
      if (opt & INDOPTION_DESC) {
        attr->sortOrder = soDESC;
      } else {
        attr->sortOrder = soASC;
      }

      if (opt & INDOPTION_NULLS_FIRST) {
        attr->nullsOrder = noNULLS_FIRST;
      } else {
        attr->nullsOrder = noNULLS_LAST;
      }
    }
  }

  return result;
}

char *get_predicate(HeapTuple ht_idx, List *context) {
  Node *indpred = get_indpred(ht_idx);

  if (!indpred)
    return NULL;

  return deparse_expression(indpred, context, false, false);
}

PGIndexOption *get_options(HeapTuple ht_idxrel, List *context, int *noptions) {
  Datum reloptions;
  bool isnull;

  reloptions =
      SysCacheGetAttr(RELOID, ht_idxrel, Anum_pg_class_reloptions, &isnull);

  if (isnull)
    return NULL;

  return parse_options(reloptions, noptions);
}

PGIndex *foo(Oid indexrelid) {
  HeapTuple ht_idx;
  HeapTuple ht_idxrel;
  HeapTuple ht_rel;
  HeapTuple ht_ns;
  HeapTuple ht_am;

  Form_pg_index idxrec;
  Form_pg_class idxrelrec;
  Form_pg_class relrec;
  Form_pg_namespace nsrec;
  Form_pg_am amrec;

  IndexAmRoutine *am_routine;

  List *context;

  PGIndex *result;

  bool is_null;

  // pg_index
  ht_idx = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexrelid));
  if (!HeapTupleIsValid(ht_idx)) {
    return NULL;
  }
  idxrec = (Form_pg_index)GETSTRUCT(ht_idx);

  // pg_class (index)
  ht_idxrel = SearchSysCache1(RELOID, ObjectIdGetDatum(indexrelid));
  if (!HeapTupleIsValid(ht_idxrel)) {
    elog(ERROR, "cache lookup failed for index relation %u", indexrelid);
  }
  idxrelrec = (Form_pg_class)GETSTRUCT(ht_idxrel);

  // pg_class (table)
  ht_rel = SearchSysCache1(RELOID, ObjectIdGetDatum(idxrec->indrelid));
  if (!HeapTupleIsValid(ht_rel)) {
    elog(ERROR, "cache lookup failed for relation %u", idxrec->indrelid);
  }
  relrec = (Form_pg_class)GETSTRUCT(ht_rel);

  // pg_namespace (table)
  ht_ns = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(relrec->relnamespace));
  if (!HeapTupleIsValid(ht_ns)) {
    elog(ERROR, "cache lookup failed for namespace %u", relrec->relnamespace);
  }
  nsrec = (Form_pg_namespace)GETSTRUCT(ht_ns);

  // pg_namespace (table)
  ht_am = SearchSysCache1(AMOID, ObjectIdGetDatum(idxrelrec->relam));
  if (!HeapTupleIsValid(ht_ns)) {
    elog(ERROR, "cache lookup failed for access method %u", relrec->relam);
  }
  amrec = (Form_pg_am)GETSTRUCT(ht_am);

  context = deparse_context_for(NameStr(relrec->relname), idxrec->indrelid);

  am_routine = GetIndexAmRoutine(amrec->amhandler);

  result = palloc(sizeof(PGIndex));
  result->name = pstrdup(NameStr(idxrelrec->relname));
  result->schema = pstrdup(NameStr(nsrec->nspname));
  result->table = pstrdup(NameStr(relrec->relname));
  result->isUnique = idxrec->indisunique;
  result->method = pstrdup(NameStr(amrec->amname));
  result->nattributes = idxrec->indnatts;
  result->attributes = get_attributes(ht_idx, context, am_routine);
  result->options = get_options(ht_idxrel, context, &result->noptions);
  result->predicate = get_predicate(ht_idx, context);

  ReleaseSysCache(ht_idx);
  ReleaseSysCache(ht_idxrel);
  ReleaseSysCache(ht_rel);
  ReleaseSysCache(ht_ns);
  ReleaseSysCache(ht_am);

  return result;
}

PG_FUNCTION_INFO_V1(pg_dbobject);

Datum pg_dbobject(PG_FUNCTION_ARGS) {
  Oid indexrelid = PG_GETARG_OID(0);

  struct PGIndex *index;

  index = foo(indexrelid);

  StringInfoData buf;

  initStringInfo(&buf);

  appendStringInfo(&buf, "{");

  bool isUnique;
  PGIndexAttribute *attributes;
  PGIndexOption *options;
  const char *predicate;

  appendStringInfo(&buf, "\"name\": \"%s\",", index->name);
  appendStringInfo(&buf, "\"table\": \"%s\",", index->table);
  appendStringInfo(&buf, "\"schema\": \"%s\",", index->schema);
  appendStringInfo(&buf, "\"method\": \"%s\",", index->method);
  appendStringInfo(&buf, "\"is unique\": %s",
                   index->isUnique ? "true" : "false");

  if (index->attributes) {
    appendStringInfo(&buf, ",\"attributes\": [");
    for (int i = 0; i < index->nattributes; ++i) {
      PGIndexAttribute *att = index->attributes + i;
      if (i != 0)
        appendStringInfo(&buf, ",");
      appendStringInfo(&buf, "{");
      appendStringInfo(&buf, "\"type\": \"%s\",",
                       att->type == atCOLUMN ? "column" : "expr");
      appendStringInfo(&buf, "\"value\": \"%s\"", att->value);
      if (att->collation)
        appendStringInfo(&buf, ",\"collation\": \"%s\"", att->collation);
      if (att->operator_class)
        appendStringInfo(&buf, ",\"operator class\": \"%s\"",
                         att->operator_class);
      if (att->sortOrder != soNONE)
        appendStringInfo(&buf, ",\"sort order\": %d", (int)att->sortOrder);
      if (att->nullsOrder != noNONE)
        appendStringInfo(&buf, ",\"nulls order\": %d", (int)att->nullsOrder);
      appendStringInfo(&buf, "}");
    }
    appendStringInfo(&buf, "]");
  }

  if (index->options) {
    appendStringInfo(&buf, ",\"options\": [");
    for (int i = 0; i < index->noptions; ++i) {
      PGIndexOption *opt = index->options + i;
      if (i != 0)
        appendStringInfo(&buf, ",");
      appendStringInfo(&buf, "{");
      appendStringInfo(&buf, "\"name\": \"%s\",", opt->name);
      appendStringInfo(&buf, "\"value\": \"%s\"", opt->value);
      appendStringInfo(&buf, "}");
    }
    appendStringInfo(&buf, "]");
  }

  if (index->predicate)
    appendStringInfo(&buf, ",\"predicate\": \"%s\"", index->predicate);

  appendStringInfo(&buf, "}");

  PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

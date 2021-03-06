/*
   Copyright (c) 2013 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/*

== EXPLAIN/ANALYZE architecture ==

=== [SHOW] EXPLAIN data ===
Query optimization produces two data structures:
1. execution data structures themselves (eg. JOINs, JOIN_TAB, etc, etc)
2. Explain data structures.

#2 are self contained set of data structures that has sufficient info to
produce output of SHOW EXPLAIN, EXPLAIN [FORMAT=JSON], or 
ANALYZE [FORMAT=JSON], without accessing the execution data structures.

(the only exception is that Explain data structures keep Item* pointers,
and we require that one might call item->print(QT_EXPLAIN) when printing
FORMAT=JSON output)

=== ANALYZE data ===
EXPLAIN data structures have embedded ANALYZE data structures. These are 
objects that are used to track how the parts of query plan were executed:
how many times each part of query plan was invoked, how many rows were
read/returned, etc.

Each execution data structure keeps a direct pointer to its ANALYZE data
structure. It is needed so that execution code can quickly increment the
counters.

(note that this increases the set of data that is frequently accessed 
during the execution. What is the impact of this?)

Since ANALYZE/EXPLAIN data structures are separated from execution data
structures, it is easy to have them survive until the end of the query,
where we can return ANALYZE [FORMAT=JSON] output to the user, or print 
it into the slow query log.

*/

class String_list: public List<char>
{
public:
  const char *append_str(MEM_ROOT *mem_root, const char *str);
};

class Json_writer;

/*
  A class for collecting read statistics.
  
  The idea is that we run several scans. Each scans gets rows, and then filters
  some of them out.  We count scans, rows, and rows left after filtering.

  (note: at the moment, the class is not actually tied to a physical table. 
   It can be used to track reading from files, buffers, etc).
*/

class Table_access_tracker 
{
public:
  Table_access_tracker() :
    r_scans(0), r_rows(0), /*r_rows_after_table_cond(0),*/
    r_rows_after_where(0)
  {}

  ha_rows r_scans; /* How many scans were ran on this join_tab */
  ha_rows r_rows; /* How many rows we've got after that */
  ha_rows r_rows_after_where; /* Rows after applying attached part of WHERE */

  bool has_scans() { return (r_scans != 0); }
  ha_rows get_loops() { return r_scans; }
  double get_avg_rows()
  {
    return r_scans ? ((double)r_rows / r_scans): 0;
  }

  double get_filtered_after_where()
  {
    double r_filtered;
    if (r_rows > 0)
      r_filtered= (double)r_rows_after_where / r_rows;
    else
      r_filtered= 1.0;

    return r_filtered;
  }
  
  inline void on_scan_init() { r_scans++; }
  inline void on_record_read() { r_rows++; }
  inline void on_record_after_where() { r_rows_after_where++; }
};

#if 0
/*
  A class to track operations (currently, row reads) on a PSI_table.
*/
class Table_op_tracker
{
  PSI_table *psi_table;

  /* Table counter values at start. Sum is in picoseconds */
  ulonglong start_sum;
  ulonglong start_count;

  /* Table counter values at end */
  ulonglong end_sum;
  ulonglong end_count;
public:
  void start_tracking(TABLE *table);
  // At the moment, print_json will call end_tracking.
  void end_tracking();

  // this may print nothing if the table was not tracked.
  void print_json(Json_writer *writer);
};
#endif

#define ANALYZE_START_TRACKING(tracker) \
  if (tracker) \
  { (tracker)->start_tracking(); }

#define ANALYZE_STOP_TRACKING(tracker) \
  if (tracker) \
  { (tracker)->stop_tracking(); }


/**************************************************************************************
 
  Data structures for producing EXPLAIN outputs.

  These structures
  - Can be produced inexpensively from query plan.
  - Store sufficient information to produce tabular EXPLAIN output (the goal is 
    to be able to produce JSON also)

*************************************************************************************/


const int FAKE_SELECT_LEX_ID= (int)UINT_MAX;

class Explain_query;

/* 
  A node can be either a SELECT, or a UNION.
*/
class Explain_node : public Sql_alloc
{
public:
  Explain_node(MEM_ROOT *root)
    :children(root)
  {}
  /* A type specifying what kind of node this is */
  enum explain_node_type 
  {
    EXPLAIN_UNION, 
    EXPLAIN_SELECT,
    EXPLAIN_BASIC_JOIN,
    EXPLAIN_UPDATE,
    EXPLAIN_DELETE, 
    EXPLAIN_INSERT
  };
  
  /* How this node is connected */
  enum explain_connection_type {
    EXPLAIN_NODE_OTHER,
    EXPLAIN_NODE_DERIVED, /* Materialized derived table */
    EXPLAIN_NODE_NON_MERGED_SJ /* aka JTBM semi-join */
  };

  Explain_node() : connection_type(EXPLAIN_NODE_OTHER) {}

  virtual enum explain_node_type get_type()= 0;
  virtual int get_select_id()= 0;

  /*
    How this node is connected to its parent.
    (NOTE: EXPLAIN_NODE_NON_MERGED_SJ is set very late currently)
  */
  enum explain_connection_type connection_type;

  /* 
    A node may have children nodes. When a node's explain structure is 
    created, children nodes may not yet have QPFs. This is why we store ids.
  */
  Dynamic_array<int> children;
  void add_child(int select_no)
  {
    children.append(select_no);
  }

  virtual int print_explain(Explain_query *query, select_result_sink *output, 
                            uint8 explain_flags, bool is_analyze)=0;
  virtual void print_explain_json(Explain_query *query, Json_writer *writer, 
                                  bool is_analyze)= 0;

  int print_explain_for_children(Explain_query *query, select_result_sink *output, 
                                 uint8 explain_flags, bool is_analyze);
  void print_explain_json_for_children(Explain_query *query,
                                       Json_writer *writer, bool is_analyze);
  virtual ~Explain_node(){}
};


class Explain_table_access;


/* 
  A basic join. This is only used for SJ-Materialization nests.

  Basic join doesn't have ORDER/GROUP/DISTINCT operations. It also cannot be
  degenerate.

  It has its own select_id.
*/
class Explain_basic_join : public Explain_node
{
public:
  enum explain_node_type get_type() { return EXPLAIN_BASIC_JOIN; }
  
  Explain_basic_join(MEM_ROOT *root) : Explain_node(root), join_tabs(NULL) {}
  ~Explain_basic_join();

  bool add_table(Explain_table_access *tab, Explain_query *query);

  int get_select_id() { return select_id; }

  int select_id;

  int print_explain(Explain_query *query, select_result_sink *output,
                    uint8 explain_flags, bool is_analyze);
  void print_explain_json(Explain_query *query, Json_writer *writer, 
                          bool is_analyze);

  void print_explain_json_interns(Explain_query *query, Json_writer *writer,
                                  bool is_analyze);

  /* A flat array of Explain structs for tables. */
  Explain_table_access** join_tabs;
  uint n_join_tabs;
};


/*
  EXPLAIN structure for a SELECT.
  
  A select can be:
  1. A degenerate case. In this case, message!=NULL, and it contains a 
     description of what kind of degenerate case it is (e.g. "Impossible 
     WHERE").
  2. a non-degenrate join. In this case, join_tabs describes the join.

  In the non-degenerate case, a SELECT may have a GROUP BY/ORDER BY operation.

  In both cases, the select may have children nodes. class Explain_node
  provides a way get node's children.
*/

class Explain_select : public Explain_basic_join
{
public:
  enum explain_node_type get_type() { return EXPLAIN_SELECT; }

  Explain_select(MEM_ROOT *root) : 
  Explain_basic_join(root),
    message(NULL),
    using_temporary(false), using_filesort(false)
  {}

  /*
    This is used to save the results of "late" test_if_skip_sort_order() calls
    that are made from JOIN::exec
  */
  void replace_table(uint idx, Explain_table_access *new_tab);

public:
  const char *select_type;

  /*
    If message != NULL, this is a degenerate join plan, and all subsequent
    members have no info 
  */
  const char *message;
  
  /* Expensive constant condition */
  Item *exec_const_cond;
  
  /* Global join attributes. In tabular form, they are printed on the first row */
  bool using_temporary;
  bool using_filesort;

  /* ANALYZE members */
  Exec_time_tracker time_tracker;
  
  int print_explain(Explain_query *query, select_result_sink *output, 
                    uint8 explain_flags, bool is_analyze);
  void print_explain_json(Explain_query *query, Json_writer *writer, 
                          bool is_analyze);
  
  Table_access_tracker *get_using_temporary_read_tracker()
  {
    return &using_temporary_read_tracker;
  }
private:
  Table_access_tracker using_temporary_read_tracker;
};


/* 
  Explain structure for a UNION.

  A UNION may or may not have "Using filesort".
*/

class Explain_union : public Explain_node
{
public:
  Explain_union(MEM_ROOT *root) : 
  Explain_node(root)
  {}

  enum explain_node_type get_type() { return EXPLAIN_UNION; }

  int get_select_id()
  {
    DBUG_ASSERT(union_members.elements() > 0);
    return union_members.at(0);
  }
  /*
    Members of the UNION.  Note: these are different from UNION's "children".
    Example:

      (select * from t1) union 
      (select * from t2) order by (select col1 from t3 ...)

    here 
      - select-from-t1 and select-from-t2 are "union members",
      - select-from-t3 is the only "child".
  */
  Dynamic_array<int> union_members;

  void add_select(int select_no)
  {
    union_members.append(select_no);
  }
  int print_explain(Explain_query *query, select_result_sink *output, 
                    uint8 explain_flags, bool is_analyze);
  void print_explain_json(Explain_query *query, Json_writer *writer, 
                          bool is_analyze);

  const char *fake_select_type;
  bool using_filesort;
  bool using_tmp;

  Table_access_tracker *get_fake_select_lex_tracker()
  {
    return &fake_select_lex_tracker;
  }
  Table_access_tracker *get_tmptable_read_tracker()
  {
    return &tmptable_read_tracker;
  }
private:
  uint make_union_table_name(char *buf);
  
  Table_access_tracker fake_select_lex_tracker;
  /* This one is for reading after ORDER BY */
  Table_access_tracker tmptable_read_tracker; 
};


class Explain_update;
class Explain_delete;
class Explain_insert;


/*
  Explain structure for a query (i.e. a statement).

  This should be able to survive when the query plan was deleted. Currently, 
  we do not intend for it survive until after query's MEM_ROOT is freed. It
  does surivive freeing of query's items.
   
  For reference, the process of post-query cleanup is as follows:

    >dispatch_command
    | >mysql_parse
    | |  ...
    | | lex_end()
    | |  ...
    | | >THD::cleanup_after_query
    | | | ...
    | | | free_items()
    | | | ...
    | | <THD::cleanup_after_query
    | |
    | <mysql_parse
    |
    | log_slow_statement()
    | 
    | free_root()
    | 
    >dispatch_command
  
  That is, the order of actions is:
    - free query's Items
    - write to slow query log 
    - free query's MEM_ROOT
    
*/

class Explain_query : public Sql_alloc
{
public:
  Explain_query(THD *thd, MEM_ROOT *root);
  ~Explain_query();

  /* Add a new node */
  void add_node(Explain_node *node);
  void add_insert_plan(Explain_insert *insert_plan_arg);
  void add_upd_del_plan(Explain_update *upd_del_plan_arg);

  /* This will return a select, or a union */
  Explain_node *get_node(uint select_id);

  /* This will return a select (even if there is a union with this id) */
  Explain_select *get_select(uint select_id);
  
  Explain_union *get_union(uint select_id);
 
  /* Produce a tabular EXPLAIN output */
  int print_explain(select_result_sink *output, uint8 explain_flags, 
                    bool is_analyze);
  
  /* Send tabular EXPLAIN to the client */
  int send_explain(THD *thd);
  
  /* Return tabular EXPLAIN output as a text string */
  bool print_explain_str(THD *thd, String *out_str, bool is_analyze);

  void print_explain_json(select_result_sink *output, bool is_analyze);

  /* If true, at least part of EXPLAIN can be printed */
  bool have_query_plan() { return insert_plan || upd_del_plan|| get_node(1) != NULL; }

  void query_plan_ready();

  MEM_ROOT *mem_root;

  Explain_update *get_upd_del_plan() { return upd_del_plan; }
private:
  /* Explain_delete inherits from Explain_update */
  Explain_update *upd_del_plan;

  /* Query "plan" for INSERTs */
  Explain_insert *insert_plan;

  Dynamic_array<Explain_union*> unions;
  Dynamic_array<Explain_select*> selects;
  
  THD *thd; // for APC start/stop
  bool apc_enabled;
  /* 
    Debugging aid: count how many times add_node() was called. Ideally, it
    should be one, we currently allow O(1) query plan saves for each
    select or union.  The goal is not to have O(#rows_in_some_table), which 
    is unacceptable.
  */
  longlong operations;
};


/* 
  Some of the tags have matching text. See extra_tag_text for text names, and 
  Explain_table_access::append_tag_name() for code to convert from tag form to text
  form.
*/
enum explain_extra_tag
{
  ET_none= 0, /* not-a-tag */
  ET_USING_INDEX_CONDITION,
  ET_USING_INDEX_CONDITION_BKA,
  ET_USING, /* For quick selects of various kinds */
  ET_RANGE_CHECKED_FOR_EACH_RECORD,
  ET_USING_WHERE_WITH_PUSHED_CONDITION,
  ET_USING_WHERE,
  ET_NOT_EXISTS,

  ET_USING_INDEX,
  ET_FULL_SCAN_ON_NULL_KEY,
  ET_SKIP_OPEN_TABLE,
  ET_OPEN_FRM_ONLY,
  ET_OPEN_FULL_TABLE,

  ET_SCANNED_0_DATABASES,
  ET_SCANNED_1_DATABASE,
  ET_SCANNED_ALL_DATABASES,

  ET_USING_INDEX_FOR_GROUP_BY,

  ET_USING_MRR, // does not print "Using mrr". 

  ET_DISTINCT,
  ET_LOOSESCAN,
  ET_START_TEMPORARY,
  ET_END_TEMPORARY,
  ET_FIRST_MATCH,
  
  ET_USING_JOIN_BUFFER,

  ET_CONST_ROW_NOT_FOUND,
  ET_UNIQUE_ROW_NOT_FOUND,
  ET_IMPOSSIBLE_ON_CONDITION,

  ET_total
};


/*
  Explain data structure describing join buffering use.
*/

class EXPLAIN_BKA_TYPE
{
public:
  EXPLAIN_BKA_TYPE() : join_alg(NULL) {}

  bool incremental;

  /* 
    NULL if no join buferring used.
    Other values: BNL, BNLH, BKA, BKAH.
  */
  const char *join_alg;

  /* Information about MRR usage.  */
  StringBuffer<64> mrr_type;
  
  bool is_using_jbuf() { return (join_alg != NULL); }
};


/*
  Data about how an index is used by some access method
*/
class Explain_index_use : public Sql_alloc
{
  char *key_name;
  uint key_len;
public:
  String_list key_parts_list;
  
  Explain_index_use()
  {
    clear();
  }

  void clear()
  {
    key_name= NULL;
    key_len= (uint)-1;
  }
  void set(MEM_ROOT *root, KEY *key_name, uint key_len_arg);
  void set_pseudo_key(MEM_ROOT *root, const char *key_name);

  inline const char *get_key_name() const { return key_name; }
  inline uint get_key_len() const { return key_len; }
};


/*
  QPF for quick range selects, as well as index_merge select
*/
class Explain_quick_select : public Sql_alloc
{
public:
  Explain_quick_select(int quick_type_arg) : quick_type(quick_type_arg) 
  {}

  const int quick_type;

  bool is_basic() 
  {
    return (quick_type == QUICK_SELECT_I::QS_TYPE_RANGE || 
            quick_type == QUICK_SELECT_I::QS_TYPE_RANGE_DESC ||
            quick_type == QUICK_SELECT_I::QS_TYPE_GROUP_MIN_MAX);
  }
  
  /* This is used when quick_type == QUICK_SELECT_I::QS_TYPE_RANGE */
  Explain_index_use range;
  
  /* Used in all other cases */
  List<Explain_quick_select> children;
  
  void print_extra(String *str);
  void print_key(String *str);
  void print_key_len(String *str);

  void print_json(Json_writer *writer);

  void print_extra_recursive(String *str);
private:
  const char *get_name_by_type();
};


/*
  Data structure for "range checked for each record". 
  It's a set of keys, tabular explain prints hex bitmap, json prints key names.
*/

typedef const char* NAME;

class Explain_range_checked_fer : public Sql_alloc
{
public:
  String_list key_set;
  key_map keys_map;
private:
  ha_rows full_scan, index_merge;
  ha_rows *keys_stat;
  NAME *keys_stat_names;
  uint keys;

public:
  Explain_range_checked_fer()
    :Sql_alloc(), full_scan(0), index_merge(0),
    keys_stat(0), keys_stat_names(0), keys(0)
  {}

  int append_possible_keys_stat(MEM_ROOT *alloc,
                                TABLE *table, key_map possible_keys);
  void collect_data(QUICK_SELECT_I *quick);
  void print_json(Json_writer *writer, bool is_analyze);
};

/*
  EXPLAIN data structure for a single JOIN_TAB.
*/

class Explain_table_access : public Sql_alloc
{
public:
  Explain_table_access(MEM_ROOT *root) :
    derived_select_number(0),
    non_merged_sjm_number(0),
    extra_tags(root),
    range_checked_fer(NULL),
    full_scan_on_null_key(false),
    start_dups_weedout(false),
    end_dups_weedout(false),
    where_cond(NULL),
    cache_cond(NULL),
    pushed_index_cond(NULL),
    sjm_nest(NULL)
  {}
  ~Explain_table_access() { delete sjm_nest; }

  void push_extra(enum explain_extra_tag extra_tag);

  /* Internals */

  /* id and 'select_type' are cared-of by the parent Explain_select */
  StringBuffer<32> table_name;
  StringBuffer<32> used_partitions;
  // valid with ET_USING_MRR
  StringBuffer<32> mrr_type;
  StringBuffer<32> firstmatch_table_name;

  /* 
    Non-zero number means this is a derived table. The number can be used to
    find the query plan for the derived table
  */
  int derived_select_number;
  /* TODO: join with the previous member. */
  int non_merged_sjm_number;

  enum join_type type;

  bool used_partitions_set;
  
  /* Empty means "NULL" will be printed */
  String_list possible_keys;

  bool rows_set; /* not set means 'NULL' should be printed */
  bool filtered_set; /* not set means 'NULL' should be printed */
  // Valid if ET_USING_INDEX_FOR_GROUP_BY is present
  bool loose_scan_is_scanning;
  
  /*
    Index use: key name and length.
    Note: that when one is accessing I_S tables, those may show use of 
    non-existant indexes.

    key.key_name == NULL means 'NULL' will be shown in tabular output.
    key.key_len == (uint)-1 means 'NULL' will be shown in tabular output.
  */
  Explain_index_use key;
  
  /*
    when type==JT_HASH_NEXT, 'key' stores the hash join pseudo-key.
    hash_next_key stores the table's key.
  */
  Explain_index_use hash_next_key;
  
  String_list ref_list;

  ha_rows rows;
  double filtered;

  /* 
    Contents of the 'Extra' column. Some are converted into strings, some have
    parameters, values for which are stored below.
  */
  Dynamic_array<enum explain_extra_tag> extra_tags;

  // Valid if ET_USING tag is present
  Explain_quick_select *quick_info;
  
  /* Non-NULL value means this tab uses "range checked for each record" */
  Explain_range_checked_fer *range_checked_fer;
 
  bool full_scan_on_null_key;

  // valid with ET_USING_JOIN_BUFFER
  EXPLAIN_BKA_TYPE bka_type;

  bool start_dups_weedout;
  bool end_dups_weedout;
  
  /*
    Note: lifespan of WHERE condition is less than lifespan of this object.
    The below two are valid if tags include "ET_USING_WHERE".
    (TODO: indexsubquery may put ET_USING_WHERE without setting where_cond?)
  */
  Item *where_cond;
  Item *cache_cond;

  Item *pushed_index_cond;

  Explain_basic_join *sjm_nest;

  /* ANALYZE members */

  /* Tracker for reading the table */
  Table_access_tracker tracker;
  Exec_time_tracker op_tracker;
  Table_access_tracker jbuf_tracker;

  int print_explain(select_result_sink *output, uint8 explain_flags, 
                    bool is_analyze,
                    uint select_id, const char *select_type,
                    bool using_temporary, bool using_filesort);
  void print_explain_json(Explain_query *query, Json_writer *writer,
                          bool is_analyze);

private:
  void append_tag_name(String *str, enum explain_extra_tag tag);
  void fill_key_str(String *key_str, bool is_json) const;
  void fill_key_len_str(String *key_len_str) const;
  double get_r_filtered();
  void tag_to_json(Json_writer *writer, enum explain_extra_tag tag);
};


/*
  EXPLAIN structure for single-table UPDATE. 
  
  This is similar to Explain_table_access, except that it is more restrictive.
  Also, it can have UPDATE operation options, but currently there aren't any.
*/

class Explain_update : public Explain_node
{
public:

  Explain_update(MEM_ROOT *root) : 
  Explain_node(root)
  {}

  virtual enum explain_node_type get_type() { return EXPLAIN_UPDATE; }
  virtual int get_select_id() { return 1; /* always root */ }

  const char *select_type;

  StringBuffer<32> used_partitions;
  bool used_partitions_set;

  bool impossible_where;
  bool no_partitions;
  StringBuffer<64> table_name;

  enum join_type jtype;
  String_list possible_keys;

  /* Used key when doing a full index scan (possibly with limit) */
  Explain_index_use key;

  /* 
    MRR that's used with quick select. This should probably belong to the
    quick select
  */
  StringBuffer<64> mrr_type;
  
  Explain_quick_select *quick_info;

  bool using_where;
  Item *where_cond;

  ha_rows rows;

  bool using_filesort;
  bool using_io_buffer;

  /* ANALYZE members and methods */
  Table_access_tracker tracker;
  Exec_time_tracker time_tracker;
  //psergey-todo: io-tracker here.

  virtual int print_explain(Explain_query *query, select_result_sink *output, 
                            uint8 explain_flags, bool is_analyze);
  virtual void print_explain_json(Explain_query *query, Json_writer *writer,
                                  bool is_analyze);
};


/*
  EXPLAIN data structure for an INSERT.
  
  At the moment this doesn't do much as we don't really have any query plans
  for INSERT statements.
*/

class Explain_insert : public Explain_node
{
public:
  Explain_insert(MEM_ROOT *root) : 
  Explain_node(root)
  {}

  StringBuffer<64> table_name;

  enum explain_node_type get_type() { return EXPLAIN_INSERT; }
  int get_select_id() { return 1; /* always root */ }

  int print_explain(Explain_query *query, select_result_sink *output, 
                    uint8 explain_flags, bool is_analyze);
  void print_explain_json(Explain_query *query, Json_writer *writer, 
                          bool is_analyze);
};


/* 
  EXPLAIN data of a single-table DELETE.
*/

class Explain_delete: public Explain_update
{
public:
  Explain_delete(MEM_ROOT *root) : 
  Explain_update(root)
  {}

  /*
    TRUE means we're going to call handler->delete_all_rows() and not read any
    rows.
  */
  bool deleting_all_rows;

  virtual enum explain_node_type get_type() { return EXPLAIN_DELETE; }
  virtual int get_select_id() { return 1; /* always root */ }

  virtual int print_explain(Explain_query *query, select_result_sink *output, 
                            uint8 explain_flags, bool is_analyze);
  virtual void print_explain_json(Explain_query *query, Json_writer *writer,
                                  bool is_analyze);
};



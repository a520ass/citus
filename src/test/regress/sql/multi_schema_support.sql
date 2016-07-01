--
-- MULTI_SCHEMA_SUPPORT
--

ALTER SEQUENCE pg_catalog.pg_dist_shardid_seq RESTART 1190000;
ALTER SEQUENCE pg_catalog.pg_dist_jobid_seq RESTART 1190000;

-- create schema to test schema support
CREATE SCHEMA test_schema_support;


-- test master_append_table_to_shard with schema
-- create local table to append
CREATE TABLE public.nation_local(
    n_nationkey integer not null,
    n_name char(25) not null,
    n_regionkey integer not null,
    n_comment varchar(152)
);

\COPY public.nation_local FROM STDIN with delimiter '|';
0|ALGERIA|0| haggle. carefully final deposits detect slyly agai
1|ARGENTINA|1|al foxes promise slyly according to the regular accounts. bold requests alon
2|BRAZIL|1|y alongside of the pending deposits. carefully special packages are about the ironic forges. slyly special 
3|CANADA|1|eas hang ironic, silent packages. slyly regular packages are furiously over the tithes. fluffily bold
4|EGYPT|4|y above the carefully unusual theodolites. final dugouts are quickly across the furiously regular d
5|ETHIOPIA|0|ven packages wake quickly. regu
\.

CREATE TABLE test_schema_support.nation_append(
    n_nationkey integer not null,
    n_name char(25) not null,
    n_regionkey integer not null,
    n_comment varchar(152)
);
SELECT master_create_distributed_table('test_schema_support.nation_append', 'n_nationkey', 'append');
SELECT master_create_empty_shard('test_schema_support.nation_append');

-- append table to shard
SELECT master_append_table_to_shard(1190000, 'public.nation_local', 'localhost', :master_port);

-- verify table actually appended to shard
SELECT COUNT(*) FROM test_schema_support.nation_append;

-- test with shard name contains special characters
CREATE TABLE test_schema_support."nation._'append" (
    n_nationkey integer not null,
    n_name char(25) not null,
    n_regionkey integer not null,
    n_comment varchar(152));

SELECT master_create_distributed_table('test_schema_support."nation._''append"', 'n_nationkey', 'append');
SELECT master_create_empty_shard('test_schema_support."nation._''append"');

SELECT master_append_table_to_shard(1190001, 'nation_local', 'localhost', :master_port);

-- verify table actually appended to shard
SELECT COUNT(*) FROM test_schema_support."nation._'append";

-- test with search_path is set
SET search_path TO test_schema_support;

SELECT master_append_table_to_shard(1190000, 'public.nation_local', 'localhost', :master_port);

-- verify table actually appended to shard
SELECT COUNT(*) FROM nation_append;

-- test with search_path is set and shard name contains special characters
SELECT master_append_table_to_shard(1190001, 'nation_local', 'localhost', :master_port);

-- verify table actually appended to shard
SELECT COUNT(*) FROM "nation._'append";


-- test shard creation when search_path is set
SET search_path TO test_schema_support;

-- create shard with COPY on append distributed table
CREATE TABLE nation_append_search_path(
    n_nationkey integer not null,
    n_name char(25) not null,
    n_regionkey integer not null,
    n_comment varchar(152)
);
SELECT master_create_distributed_table('nation_append_search_path', 'n_nationkey', 'append');

\COPY nation_append_search_path FROM STDIN with delimiter '|';
0|ALGERIA|0| haggle. carefully final deposits detect slyly agai
1|ARGENTINA|1|al foxes promise slyly according to the regular accounts. bold requests alon
2|BRAZIL|1|y alongside of the pending deposits. carefully special packages are about the ironic forges. slyly special 
3|CANADA|1|eas hang ironic, silent packages. slyly regular packages are furiously over the tithes. fluffily bold
4|EGYPT|4|y above the carefully unusual theodolites. final dugouts are quickly across the furiously regular d
5|ETHIOPIA|0|ven packages wake quickly. regu
\.

-- create shard with master_create_empty_shard
SELECT master_create_empty_shard('nation_append_search_path');

-- create shard with master_create_worker_shards
CREATE TABLE test_schema_support.nation_hash(
    n_nationkey integer not null,
    n_name char(25) not null,
    n_regionkey integer not null,
    n_comment varchar(152)
);
SELECT master_create_distributed_table('test_schema_support.nation_hash', 'n_nationkey', 'hash');
SELECT master_create_worker_shards('test_schema_support.nation_hash', 4, 1);


-- test cursors
SET search_path TO public;
BEGIN;
DECLARE test_cursor CURSOR FOR 
    SELECT *
        FROM test_schema_support.nation_append
        WHERE n_nationkey = 1;
        FETCH test_cursor;
FETCH test_cursor;
END;

-- test with search_path is set
SET search_path TO test_schema_support;
BEGIN;
DECLARE test_cursor CURSOR FOR 
    SELECT *
        FROM nation_append
        WHERE n_nationkey = 1;
        FETCH test_cursor;
FETCH test_cursor;
END;


-- test inserting to table in different schema
SET search_path TO public;

INSERT INTO test_schema_support.nation_hash(n_nationkey, n_name, n_regionkey) VALUES (6, 'FRANCE', 3);

-- verify insertion
SELECT * FROM test_schema_support.nation_hash WHERE n_nationkey = 6;

-- test with search_path is set
SET search_path TO test_schema_support;

INSERT INTO nation_hash(n_nationkey, n_name, n_regionkey) VALUES (7, 'GERMANY', 3);

-- verify insertion
SELECT * FROM nation_hash WHERE n_nationkey = 7;


-- test UDFs with schemas
SET search_path TO public;

\COPY test_schema_support.nation_hash FROM STDIN with delimiter '|';
0|ALGERIA|0| haggle. carefully final deposits detect slyly agai
1|ARGENTINA|1|al foxes promise slyly according to the regular accounts. bold requests alon
2|BRAZIL|1|y alongside of the pending deposits. carefully special packages are about the ironic forges. slyly special 
3|CANADA|1|eas hang ironic, silent packages. slyly regular packages are furiously over the tithes. fluffily bold
4|EGYPT|4|y above the carefully unusual theodolites. final dugouts are quickly across the furiously regular d
5|ETHIOPIA|0|ven packages wake quickly. regu
\.

-- create UDF in master node
CREATE OR REPLACE FUNCTION dummyFunction(theValue integer)
    RETURNS text AS
$$
DECLARE
    strresult text;
BEGIN
    RETURN theValue * 3 / 2 + 1;
END;
$$
LANGUAGE 'plpgsql' IMMUTABLE;

-- create UDF in worker node 1
\c - - - :worker_1_port
CREATE OR REPLACE FUNCTION dummyFunction(theValue integer)
    RETURNS text AS
$$
DECLARE
    strresult text;
BEGIN
    RETURN theValue * 3 / 2 + 1;
END;
$$
LANGUAGE 'plpgsql' IMMUTABLE;

-- create UDF in worker node 2
\c - - - :worker_2_port
CREATE OR REPLACE FUNCTION dummyFunction(theValue integer)
    RETURNS text AS
$$
DECLARE
    strresult text;
BEGIN
    RETURN theValue * 3 / 2 + 1;
END;
$$
LANGUAGE 'plpgsql' IMMUTABLE;

\c - - - :master_port

-- UDF in public, table in schema, search_path is not set
SELECT dummyFunction(n_nationkey) FROM test_schema_support.nation_hash GROUP BY 1;

-- UDF in public, table in schema, search_path is set
SET search_path TO test_schema_support;
SELECT public.dummyFunction(n_nationkey) FROM test_schema_support.nation_hash GROUP BY 1;

-- create UDF in master node in schema
SET search_path TO test_schema_support;
CREATE OR REPLACE FUNCTION dummyFunction2(theValue integer)
    RETURNS text AS
$$
DECLARE
    strresult text;
BEGIN
    RETURN theValue * 3 / 2 + 1;
END;
$$
LANGUAGE 'plpgsql' IMMUTABLE;

-- create UDF in worker node 1 in schema
\c - - - :worker_1_port
SET search_path TO test_schema_support;
CREATE OR REPLACE FUNCTION dummyFunction2(theValue integer)
    RETURNS text AS
$$
DECLARE
    strresult text;
BEGIN
    RETURN theValue * 3 / 2 + 1;
END;
$$
LANGUAGE 'plpgsql' IMMUTABLE;

-- create UDF in worker node 2 in schema
\c - - - :worker_2_port
SET search_path TO test_schema_support;
CREATE OR REPLACE FUNCTION dummyFunction2(theValue integer)
    RETURNS text AS
$$
DECLARE
    strresult text;
BEGIN
    RETURN theValue * 3 / 2 + 1;
END;
$$
LANGUAGE 'plpgsql' IMMUTABLE;

\c - - - :master_port

-- UDF in schema, table in schema, search_path is not set
SET search_path TO public;
SELECT test_schema_support.dummyFunction2(n_nationkey) FROM test_schema_support.nation_hash  GROUP BY 1;

-- UDF in schema, table in schema, search_path is set
SET search_path TO test_schema_support;
SELECT dummyFunction2(n_nationkey) FROM nation_hash  GROUP BY 1;


-- test operators with schema
SET search_path TO public;

-- create operator in master
CREATE OPERATOR test_schema_support.=== (
    LEFTARG = int,
    RIGHTARG = int,
    PROCEDURE = int4eq,
    COMMUTATOR = ===,
    NEGATOR = !==,
    HASHES, MERGES
);

-- create operator in worker node 1
\c - - - :worker_1_port
CREATE OPERATOR test_schema_support.=== (
    LEFTARG = int,
    RIGHTARG = int,
    PROCEDURE = int4eq,
    COMMUTATOR = ===,
    NEGATOR = !==,
    HASHES, MERGES
);

-- create operator in worker node 2
\c - - - :worker_2_port
CREATE OPERATOR test_schema_support.=== (
    LEFTARG = int,
    RIGHTARG = int,
    PROCEDURE = int4eq,
    COMMUTATOR = ===,
    NEGATOR = !==,
    HASHES, MERGES
);

\c - - - :master_port

-- test with search_path is not set
SELECT * FROM test_schema_support.nation_hash  WHERE n_nationkey OPERATOR(test_schema_support.===) 1;

-- test with search_path is set
SET search_path TO test_schema_support;
SELECT * FROM nation_hash  WHERE n_nationkey OPERATOR(===) 1;


-- test with master_modify_multiple_shards
SET search_path TO public;
SELECT master_modify_multiple_shards('UPDATE test_schema_support.nation_hash SET n_regionkey = n_regionkey + 1');

--verify master_modify_multiple_shards
SELECT * FROM test_schema_support.nation_hash;

--test with search_path is set
SET search_path TO test_schema_support;
SELECT master_modify_multiple_shards('UPDATE nation_hash SET n_regionkey = n_regionkey + 1');

--verify master_modify_multiple_shards
SELECT * FROM nation_hash;


--test COLLATION with schema
SET search_path TO public;
CREATE COLLATION test_schema_support.english FROM "en_US";

-- create COLLATION in worker node 1 in schema
\c - - - :worker_1_port
CREATE COLLATION test_schema_support.english FROM "en_US";

-- create COLLATION in worker node 2 in schema
\c - - - :worker_2_port
CREATE COLLATION test_schema_support.english FROM "en_US";

\c - - - :master_port

SELECT n_name = 'Turkey' COLLATE test_schema_support.english FROM test_schema_support.nation_hash;

--test with search_path is set
SET search_path TO test_schema_support;
SELECT n_name = 'Turkey' COLLATE english FROM nation_hash;


--test composite types with schema
SET search_path TO public;
CREATE TYPE test_schema_support.new_composite_type as (key1 text, key2 text);

-- create type in worker node 1 in schema
\c - - - :worker_1_port
CREATE TYPE test_schema_support.new_composite_type as (key1 text, key2 text);

-- create type in worker node 2 in schema
\c - - - :worker_2_port
CREATE TYPE test_schema_support.new_composite_type as (key1 text, key2 text);

\c - - - :master_port
CREATE TABLE test_schema_support.nation_hash_search_path(
    n_nationkey integer not null,
    n_name char(25) not null,
    n_regionkey integer not null,
    n_comment varchar(152),
    test_col test_schema_support.new_composite_type
);
SELECT master_create_distributed_table('test_schema_support.nation_hash_search_path', 'n_nationkey', 'hash');
SELECT master_create_worker_shards('test_schema_support.nation_hash_search_path', 4, 1);

SELECT * FROM test_schema_support.nation_hash_search_path  WHERE test_col = '(a,a)'::test_schema_support.new_composite_type;

--test with search_path is set
SET search_path TO test_schema_support;
SELECT * FROM nation_hash_search_path WHERE test_col = '(a,a)'::new_composite_type;


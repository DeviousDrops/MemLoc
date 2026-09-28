-- Allocation-heavy, deterministic sqlite3 workload for tools/preload_demo.sh.
-- Output must be byte-identical with and without libmemloc.so.
PRAGMA cache_size = -65536;
CREATE TABLE t(id INTEGER PRIMARY KEY, k INTEGER, s TEXT, b BLOB);
WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 200000)
INSERT INTO t
  SELECT x, (x * 2654435761) % 1000003,
         printf('row-%08d-%s', x, substr('abcdefghijklmnopqrstuvwxyz', 1 + x % 26)),
         zeroblob(x % 2000)
  FROM c;
CREATE INDEX t_k ON t(k);
CREATE INDEX t_s ON t(s);
SELECT count(*), sum(k), sum(length(s)), sum(length(b)) FROM t;
SELECT k, s FROM t WHERE k BETWEEN 1000 AND 1100 ORDER BY k LIMIT 5;
SELECT id % 7, count(*), max(s), min(k) FROM t GROUP BY 1 ORDER BY 1;
UPDATE t SET s = s || '-updated', b = zeroblob(length(b) * 2) WHERE id % 3 = 0;
DELETE FROM t WHERE id % 5 = 0;
SELECT count(*), sum(length(s)), sum(length(b)) FROM t;
SELECT length(group_concat(s, ',')) FROM (SELECT s FROM t WHERE id < 50000 ORDER BY s DESC);
CREATE TABLE u AS SELECT k % 1000 AS g, group_concat(id) AS ids FROM t GROUP BY g;
SELECT count(*), sum(length(ids)) FROM u;
VACUUM;
PRAGMA integrity_check;

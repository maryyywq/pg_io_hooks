# Test pg_upgrade of a cluster encrypted with pg_page_crypt from an older
# PostgreSQL version to the one under test.
#
# pg_upgrade copies the relation files of user data as they are on disk,
# bypassing the hooks, and keeps their file numbers.  The ciphertext of a
# block depends only on the key, the file number and the block number, so
# the copied files can be read by the new cluster if it uses the same key.
# The new cluster is created with initdb and converted (while still empty)
# before pg_upgrade, like any cluster that pg_page_crypt is to be used with.
#
# OLD_INSTALL names the installation of the older version; pg_page_crypt must
# be installed in both.  scripts/run-upgrade-test.sh sets this up.

use strict;
use warnings FATAL => 'all';
use Cwd;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $old_install = $ENV{OLD_INSTALL};
plan skip_all => 'OLD_INSTALL is not set' unless $old_install;

my $marker = 'pg-page-crypt-plaintext-marker';

my $tmp = PostgreSQL::Test::Utils::tempdir;
my $key = "$tmp/key.hex";
{
	my $hex = join '', map { sprintf '%02x', int(rand(256)) } 1 .. 64;
	open my $fh, '>', $key or die "could not write $key: $!";
	print $fh "$hex\n";
	close $fh;
	chmod 0600, $key;
}

my $module_conf = qq{
shared_preload_libraries = 'pg_page_crypt'
pg_page_crypt.key_file = '$key'
};

sub raw_has_marker
{
	my ($node, $rel) = @_;
	my $path = $node->safe_psql('postgres', "SELECT pg_relation_filepath('$rel')");
	return index(slurp_file($node->data_dir . "/$path"), $marker) >= 0;
}

# A digest of all data that the test creates, to compare before and after.
my $digest_query = q{
SELECT md5(string_agg(x, '|' ORDER BY x)) FROM (
  SELECT 't:' || md5(string_agg(t::text, ',' ORDER BY id)) AS x FROM t
  UNION ALL SELECT 'big:' || md5(string_agg(md5(b), ',' ORDER BY id)) FROM big
  UNION ALL SELECT 'mv:' || md5(string_agg(mv::text, ',' ORDER BY n)) FROM mv
  UNION ALL SELECT 'seq:' || last_value::text FROM s
  UNION ALL SELECT 'lo:' || md5(lo_get(4242))
) d
};

# The old cluster, encrypted, with several kinds of relations.
my $old = PostgreSQL::Test::Cluster->new('old', install_path => $old_install);
$old->init(extra => ['--data-checksums']);
$old->start;
$old->safe_psql('postgres', qq{
CREATE TABLE t (id int PRIMARY KEY, v text);
INSERT INTO t SELECT g, '$marker ' || g FROM generate_series(1, 5000) g;
CREATE INDEX t_v ON t (v);
CREATE TABLE big (id int, b text);
ALTER TABLE big ALTER COLUMN b SET STORAGE EXTERNAL;
INSERT INTO big SELECT g, repeat('$marker', 1000) || g FROM generate_series(1, 20) g;
CREATE SEQUENCE s;
SELECT nextval('s') FROM generate_series(1, 17);
CREATE MATERIALIZED VIEW mv AS SELECT id % 100 AS n, count(*) AS c FROM t GROUP BY 1;
SELECT lo_from_bytea(4242, convert_to(repeat('$marker', 500), 'UTF8'));
});
# pg_largeobject is a catalog, so its file starts as plaintext; VACUUM FULL
# gives it a file number of a user relation, so that it is encrypted, and
# pg_upgrade keeps that file.
$old->safe_psql('postgres', 'VACUUM FULL pg_largeobject; CHECKPOINT');
# The check of encryption below can only fail if the plaintext is visible
# in the files to begin with.
ok(raw_has_marker($old, 't') && raw_has_marker($old, 'pg_largeobject'),
	'before the conversion, the marker is visible in the files');
$old->stop;
command_ok(["$old_install/bin/pg_page_crypt_convert", '--encrypt', '-D', $old->data_dir, '--key-file', $key],
	'encrypt the old cluster');
$old->append_conf('postgresql.conf', $module_conf);
$old->start;
# Changes made while encrypted.
$old->safe_psql('postgres', qq{
UPDATE t SET v = v || ' updated' WHERE id % 3 = 0;
DELETE FROM t WHERE id % 11 = 0;
INSERT INTO t SELECT g, '$marker ' || g FROM generate_series(5001, 6000) g;
REFRESH MATERIALIZED VIEW mv;
});
my $digest = $old->safe_psql('postgres', $digest_query);
my $old_lo_file = $old->safe_psql('postgres', "SELECT pg_relation_filenode('pg_largeobject')");
$old->safe_psql('postgres', 'CHECKPOINT');
ok(!raw_has_marker($old, 't'), 'the old cluster stores t encrypted');
ok(!raw_has_marker($old, 'pg_largeobject'), 'the old cluster stores pg_largeobject encrypted');
$old->stop;

# The new cluster: created, converted while empty, configured.
my $new = PostgreSQL::Test::Cluster->new('new');
$new->init(extra => ['--data-checksums']);
command_ok(['pg_page_crypt_convert', '--encrypt', '-D', $new->data_dir, '--key-file', $key],
	'encrypt the new, empty cluster');
$new->append_conf('postgresql.conf', $module_conf);

# pg_upgrade writes its logs into the current directory.
my $cwd = getcwd;
chdir $tmp;
my @upgrade = (
	'pg_upgrade', '--no-sync',
	'--old-datadir', $old->data_dir, '--new-datadir', $new->data_dir,
	'--old-bindir', $old->config_data('--bindir'), '--new-bindir', $new->config_data('--bindir'),
	'--socketdir', $new->host,
	'--old-port', $old->port, '--new-port', $new->port);
command_ok([ @upgrade, '--check' ], 'pg_upgrade --check');
command_ok(\@upgrade, 'pg_upgrade');
chdir $cwd;

$new->start;
is($new->safe_psql('postgres', $digest_query), $digest, 'the upgraded cluster has the same data');
is($new->safe_psql('postgres', "SELECT pg_relation_filenode('pg_largeobject')"), $old_lo_file,
	'pg_upgrade kept the file number of pg_largeobject');
is($new->safe_psql('postgres', "SELECT count(*) FROM t WHERE v = '$marker 778'"), '1',
	'the upgraded cluster reads t through its index');

$new->safe_psql('postgres', 'CREATE EXTENSION amcheck');
$new->safe_psql('postgres', q{
SELECT bt_index_check(c.oid, true) FROM pg_class c
WHERE c.relkind = 'i' AND c.relam = (SELECT oid FROM pg_am WHERE amname = 'btree')
  AND c.relnamespace = 'public'::regnamespace
});
is($new->safe_psql('postgres', q{
SELECT count(*) FROM pg_class c, verify_heapam(c.oid)
WHERE c.relkind IN ('r', 'm') AND c.relnamespace = 'public'::regnamespace
}), '0', 'amcheck finds no problems in the upgraded cluster');

# The new cluster keeps writing encrypted data.
$new->safe_psql('postgres', qq{
INSERT INTO t SELECT g, '$marker ' || g FROM generate_series(6001, 7000) g;
CHECKPOINT;
});
ok(!raw_has_marker($new, 't'), 'the upgraded cluster stores t encrypted');
ok(!raw_has_marker($new, 'pg_largeobject'), 'the upgraded cluster stores pg_largeobject encrypted');
$new->restart;
is($new->safe_psql('postgres', 'SELECT count(*) FROM t'), '6546', 'the upgraded cluster survives a restart');

$new->stop;

done_testing();

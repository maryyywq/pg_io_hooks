# Test pg_page_crypt with pg_rewind: after a failover, the old primary has
# changes that the new one does not have, and pg_rewind makes it a standby of
# the new primary again.  pg_rewind copies changed blocks between the data
# directories as they are on disk, bypassing the hooks; this works because
# both clusters use the same key and the ciphertext of a block depends only on
# the relation file number and the block number.
#
# Both ways of reaching the source are tested: its data directory
# (--source-pgdata, source shut down) and a connection (--source-server,
# source running).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Copy;

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

sub raw_has_marker
{
	my ($node, $rel) = @_;
	my $path = $node->safe_psql('postgres', "SELECT pg_relation_filepath('$rel')");
	return index(slurp_file($node->data_dir . "/$path"), $marker) >= 0;
}

# The number of problems amcheck finds in the tables and B-tree indexes of
# the public schema (bt_index_check raises an error instead).
sub amcheck_problems
{
	my ($node) = @_;
	$node->safe_psql('postgres', q{
SELECT bt_index_check(c.oid, true) FROM pg_class c
WHERE c.relkind = 'i' AND c.relam = (SELECT oid FROM pg_am WHERE amname = 'btree')
  AND c.relnamespace = 'public'::regnamespace
});
	return $node->safe_psql('postgres', q{
SELECT count(*) FROM pg_class c, verify_heapam(c.oid)
WHERE c.relkind = 'r' AND c.relnamespace = 'public'::regnamespace
});
}

sub run_test
{
	my ($mode) = @_;

	# Primary: converted before the module is first loaded.
	my $primary = PostgreSQL::Test::Cluster->new("primary_$mode");
	$primary->init(allows_streaming => 1, extra => ['--data-checksums']);
	$primary->start;
	$primary->safe_psql('postgres', qq{
CREATE EXTENSION amcheck;
CREATE TABLE t (id int PRIMARY KEY, v text);
INSERT INTO t SELECT g, '$marker ' || g FROM generate_series(1, 5000) g;
});
	$primary->stop;
	command_ok(['pg_page_crypt_convert', '--encrypt', '-D', $primary->data_dir, '--key-file', $key],
		"$mode: encrypt the primary");
	# pg_rewind needs the WAL of the old primary from the point where the
	# servers diverged, as in the tests of pg_rewind itself.
	$primary->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'pg_page_crypt'
pg_page_crypt.key_file = '$key'
wal_keep_size = 320MB
});
	$primary->start;

	my $standby = PostgreSQL::Test::Cluster->new("standby_$mode");
	$primary->backup('b1');
	$standby->init_from_backup($primary, 'b1', has_streaming => 1);
	$standby->start;
	$primary->wait_for_catchup($standby);

	# Failover.  The new primary must have a checkpoint on its new timeline
	# for pg_rewind to see it.
	$standby->promote;
	$standby->safe_psql('postgres', 'CHECKPOINT');
	my $new = $standby;
	my $old = $primary;

	# Diverging changes.  On the new primary: rows changed in place, a new
	# table.  On the old primary: other rows changed, a table that must
	# disappear, a table rewritten into a new file.
	$new->safe_psql('postgres', qq{
UPDATE t SET v = 'new ' || id WHERE id % 10 = 0;
CREATE TABLE only_new AS SELECT g AS id, '$marker ' || g AS v FROM generate_series(1, 3000) g;
CREATE INDEX only_new_v ON only_new (v);
CHECKPOINT;
});
	$old->safe_psql('postgres', qq{
UPDATE t SET v = 'old ' || id WHERE id % 7 = 0;
DELETE FROM t WHERE id > 4000;
CREATE TABLE only_old AS SELECT g AS id FROM generate_series(1, 3000) g;
VACUUM FULL t;
});
	$old->stop;

	my @source;
	if ($mode eq 'local')
	{
		$new->stop;
		@source = ('--source-pgdata', $new->data_dir);
	}
	else
	{
		# --write-recovery-conf puts this connection string into the
		# configuration of the target.  Before PostgreSQL 19 the standby
		# does not name itself after cluster_name, so give it the name
		# that wait_for_catchup looks for.
		@source = ('--source-server',
			$new->connstr('postgres') . ' application_name=' . $old->name,
			'--write-recovery-conf');
	}
	# pg_rewind copies the configuration files of the source, which include
	# its port; keep those of the target, as the tests of pg_rewind do.
	my $conf = $old->data_dir . '/postgresql.conf';
	copy($conf, "$tmp/postgresql.conf.$mode") or die "could not copy $conf: $!";
	command_ok(['pg_rewind', '--target-pgdata', $old->data_dir, @source],
		"$mode: pg_rewind the old primary");
	copy("$tmp/postgresql.conf.$mode", $conf) or die "could not restore $conf: $!";

	if ($mode eq 'local')
	{
		$new->start;
		# What --write-recovery-conf does in the remote mode.
		$old->enable_streaming($new);
	}
	$old->start;
	$new->wait_for_catchup($old);

	is($old->safe_psql('postgres', 'SELECT count(*) FROM t'), '5000',
		"$mode: rows deleted only on the old primary are back");
	is($old->safe_psql('postgres',
			"SELECT count(*) FROM t WHERE v LIKE 'new %'"), '500',
		"$mode: rows changed on the new primary are taken over");
	is($old->safe_psql('postgres',
			"SELECT count(*) FROM t WHERE v LIKE 'old %'"), '0',
		"$mode: rows changed only on the old primary are gone");
	is($old->safe_psql('postgres',
			"SELECT to_regclass('only_old') IS NULL"), 't',
		"$mode: the table created only on the old primary is gone");
	is($old->safe_psql('postgres',
			"SELECT count(*) FROM only_new WHERE v = '$marker 777'"), '1',
		"$mode: the table created on the new primary is read through its index");
	is(amcheck_problems($old), '0', "$mode: amcheck finds no problems on the rewound server");

	$old->safe_psql('postgres', 'CHECKPOINT');
	ok(!raw_has_marker($old, 't'), "$mode: the rewound server stores t encrypted");
	ok(!raw_has_marker($old, 'only_new'), "$mode: the rewound server stores only_new encrypted");

	# The rewound server keeps working as a standby.
	$new->safe_psql('postgres', 'INSERT INTO t VALUES (100000, \'after rewind\')');
	$new->wait_for_catchup($old);
	is($old->safe_psql('postgres', 'SELECT v FROM t WHERE id = 100000'), 'after rewind',
		"$mode: the rewound server replays new changes");

	$old->stop;
	$new->stop;
}

run_test('local');
run_test('remote');

done_testing();

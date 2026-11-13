# Test pg_page_crypt with physical replication and base backups: a standby
# built from a base backup needs the same module and key, replays WAL through
# the hooks and can be promoted.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

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

# Primary: converted before the module is first loaded.
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1, extra => ['--data-checksums']);
$primary->start;
$primary->safe_psql('postgres', qq{
CREATE TABLE t (id int, v text);
INSERT INTO t SELECT g, '$marker ' || g FROM generate_series(1, 3000) g;
});
$primary->stop;
command_ok(['pg_page_crypt_convert', '--encrypt', '-D', $primary->data_dir, '--key-file', $key],
	'encrypt the primary');
$primary->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'pg_page_crypt'
pg_page_crypt.key_file = '$key'
});
$primary->start;
is($primary->safe_psql('postgres', 'SELECT count(*) FROM t'), '3000', 'primary reads its data');

# Standby from a base backup (pg_basebackup verifies checksums by default).
$primary->backup('b1');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'b1', has_streaming => 1);
ok(-f $standby->data_dir . '/pg_page_crypt.state', 'the backup contains the state file');
$standby->start;
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'), '3000', 'standby reads the data from the backup');

# Changes made after the backup are replayed through the hooks.
$primary->safe_psql('postgres', qq{
INSERT INTO t SELECT g, '$marker ' || g FROM generate_series(3001, 6000) g;
CREATE TABLE t2 AS SELECT g, '$marker ' || g AS v FROM generate_series(1, 2000) g;
CREATE INDEX t2_v ON t2 (v);
DELETE FROM t WHERE id % 2 = 0;
VACUUM t;
});
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'), '3000', 'standby replays inserts and deletes');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t2 WHERE v = \'' . $marker . ' 777\''), '1',
	'standby reads a table created after the backup, through its index');
$standby->safe_psql('postgres', 'CHECKPOINT');
ok(!raw_has_marker($standby, 't'), 'the standby stores the table encrypted');
ok(!raw_has_marker($standby, 't2'), 'the standby stores the new table encrypted');

# Standby crash and restart: recovery reads and writes encrypted pages.
$standby->stop('immediate');
$standby->start;
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t2'), '2000', 'standby survives a crash');

# Promotion.
$primary->stop;
$standby->promote;
$standby->safe_psql('postgres', "INSERT INTO t VALUES (100000, 'after promotion')");
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t WHERE id = 100000'), '1',
	'the promoted standby accepts writes');
$standby->stop;

# A standby without the key cannot start.
my $nokey = PostgreSQL::Test::Cluster->new('nokey');
$nokey->init_from_backup($primary, 'b1', has_streaming => 1);
$nokey->append_conf('postgresql.conf', "pg_page_crypt.key_file = ''");
ok(!$nokey->start(fail_ok => 1), 'a copy without the key does not start');
like(slurp_file($nokey->logfile), qr/pg_page_crypt\.key_file is not set/, 'the error mentions the key file');

done_testing();

# Test pg_page_crypt and pg_page_crypt_convert: conversion, transparent
# access, crash recovery, wrong key, missing conversion and decryption.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $marker = 'pg-page-crypt-plaintext-marker';

sub make_key
{
	my ($path) = @_;
	my $hex = join '', map { sprintf '%02x', int(rand(256)) } 1 .. 64;
	open my $fh, '>', $path or die "could not write $path: $!";
	print $fh "$hex\n";
	close $fh;
	chmod 0600, $path;
}

sub raw_has_marker
{
	my ($datadir, $path) = @_;
	return index(slurp_file("$datadir/$path"), $marker) >= 0;
}

my $tmp = PostgreSQL::Test::Utils::tempdir;
my $key = "$tmp/key.hex";
my $other_key = "$tmp/other.hex";
make_key($key);
make_key($other_key);

# ---- A cluster with data checksums ----
my $node = PostgreSQL::Test::Cluster->new('crypt');
$node->init(extra => ['--data-checksums']);
$node->start;
$node->safe_psql('postgres', qq{
CREATE TABLE t (id int, v text);
INSERT INTO t SELECT g, '$marker ' || g FROM generate_series(1, 3000) g;
CREATE INDEX t_v ON t (v);
CREATE UNLOGGED TABLE u (id int, v text);
INSERT INTO u SELECT g, '$marker ' || g FROM generate_series(1, 200) g;
});
my $t_path = $node->safe_psql('postgres', "SELECT pg_relation_filepath('t')");
my $i_path = $node->safe_psql('postgres', "SELECT pg_relation_filepath('t_v')");
my $datadir = $node->data_dir;
$node->stop;

ok(raw_has_marker($datadir, $t_path), 'baseline: the table file is plaintext');

# The tool refuses to run on a running or unclean cluster is covered by the
# checks below; first a wrong invocation.
command_fails(['pg_page_crypt_convert', '--encrypt', '-D', $datadir],
	'the tool needs a key file');

command_ok(['pg_page_crypt_convert', '--encrypt', '-D', $datadir, '--key-file', $key],
	'encrypt the cluster');
ok(!raw_has_marker($datadir, $t_path), 'the table file is encrypted');
ok(!raw_has_marker($datadir, $i_path), 'the index file is encrypted');
ok(-f "$datadir/pg_page_crypt.state", 'the state file exists');
command_fails(['pg_page_crypt_convert', '--encrypt', '-D', $datadir, '--key-file', $key],
	'a second encryption is refused');

# The server cannot start without the key.
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_page_crypt'");
ok(!$node->start(fail_ok => 1), 'the server refuses to start without a key file');
like(slurp_file($node->logfile), qr/pg_page_crypt\.key_file is not set/,
	'the error mentions the key file');

# A wrong key is detected.
$node->append_conf('postgresql.conf', "pg_page_crypt.key_file = '$other_key'");
ok(!$node->start(fail_ok => 1), 'the server refuses to start with a wrong key');
like(slurp_file($node->logfile), qr/does not match the key this cluster was encrypted with/,
	'the error reports a key mismatch');

# The right key.
$node->append_conf('postgresql.conf', "pg_page_crypt.key_file = '$key'");
$node->start;
is($node->safe_psql('postgres', "SELECT count(*) FROM t WHERE v LIKE '$marker%'"),
	'3000', 'encrypted data is read back correctly');
is($node->safe_psql('postgres', "SELECT sum(id) FROM t"), '4501500', 'sum is correct');
is($node->safe_psql('postgres', "SELECT count(*) FROM t WHERE v = '$marker 1234'"),
	'1', 'the index is usable');

# New data goes through the hooks, including relation extension.
$node->safe_psql('postgres', qq{
INSERT INTO t SELECT g, '$marker ' || g FROM generate_series(3001, 6000) g;
CREATE TABLE t2 AS SELECT g, '$marker ' || g AS v FROM generate_series(1, 2000) g;
CHECKPOINT;
});
my $t2_path = $node->safe_psql('postgres', "SELECT pg_relation_filepath('t2')");
ok(!raw_has_marker($datadir, $t_path), 'the table file is still encrypted');
ok(!raw_has_marker($datadir, $t2_path), 'a new table is encrypted');

# Unlogged relations: the init fork is copied over the main fork at crash
# recovery, so it must decrypt the same way in every fork.
$node->safe_psql('postgres', "INSERT INTO u VALUES (1000, 'x')");
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', "SELECT count(*) FROM t"), '6000', 'rows survive crash recovery');
is($node->safe_psql('postgres', "SELECT count(*) FROM u"), '0', 'unlogged table is reset and usable');
$node->safe_psql('postgres', "INSERT INTO u VALUES (1, 'after reset')");
is($node->safe_psql('postgres', "SELECT count(*) FROM u"), '1', 'unlogged table accepts new rows');

# Other operations that move data around.
$node->safe_psql('postgres', "VACUUM FULL t; CLUSTER t USING t_v; REINDEX TABLE t");
is($node->safe_psql('postgres', "SELECT count(*) FROM t"), '6000', 'VACUUM FULL, CLUSTER and REINDEX work');
$node->safe_psql('postgres', "CREATE DATABASE copy_db TEMPLATE postgres STRATEGY file_copy");
is($node->safe_psql('copy_db', "SELECT count(*) FROM t"), '6000', 'CREATE DATABASE with file_copy works');
$node->safe_psql('postgres', "CREATE DATABASE copy_db2 TEMPLATE postgres STRATEGY wal_log");
is($node->safe_psql('copy_db2', "SELECT count(*) FROM t"), '6000', 'CREATE DATABASE with wal_log works');
$node->safe_psql('postgres', "CHECKPOINT");

# The tool refuses to touch a running cluster.
command_fails(['pg_page_crypt_convert', '--decrypt', '-D', $datadir, '--key-file', $key],
	'the tool refuses a running cluster');
$node->stop;

# Without the module the encrypted pages are rejected.
my $saved = slurp_file("$datadir/postgresql.conf");
$node->append_conf('postgresql.conf', "shared_preload_libraries = ''");
$node->start;
my ($ret, $out, $err) = $node->psql('postgres', 'SELECT count(*) FROM t');
isnt($ret, 0, 'encrypted data cannot be read without the module');
like($err, qr/invalid page|page verification failed|could not read/, 'the failure is a bad page');
$node->stop;

# Decrypt: the cluster is plain again.
command_fails(['pg_page_crypt_convert', '--decrypt', '-D', $datadir, '--key-file', $other_key],
	'decryption with a wrong key is refused');
command_ok(['pg_page_crypt_convert', '--decrypt', '-D', $datadir, '--key-file', $key],
	'decrypt the cluster');
ok(!-e "$datadir/pg_page_crypt.state", 'the state file is gone');
$node->start;
is($node->safe_psql('postgres', "SELECT count(*) FROM t"), '6000', 'data is readable after decryption');
$node->stop;

# ---- Clusters the module must refuse ----
my $nochk = PostgreSQL::Test::Cluster->new('nochecksums');
# Since PostgreSQL 18, initdb enables data checksums by default.
$nochk->init(extra => $nochk->pg_version >= 18 ? ['--no-data-checksums'] : []);
make_key("$tmp/k3.hex");
command_fails(['pg_page_crypt_convert', '--encrypt', '-D', $nochk->data_dir, '--key-file', "$tmp/k3.hex"],
	'the tool refuses a cluster without data checksums');

my $plain = PostgreSQL::Test::Cluster->new('notconverted');
$plain->init(extra => ['--data-checksums']);
$plain->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_page_crypt'\npg_page_crypt.key_file = '$key'");
ok(!$plain->start(fail_ok => 1), 'the server refuses to start on a cluster that was not converted');
like(slurp_file($plain->logfile), qr/has not been converted/, 'the error says the cluster was not converted');

done_testing();

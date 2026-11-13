# Test that pg_io_audit (which only observes) and pg_page_crypt (which
# transforms data) work together, whichever is loaded first.
#
# Hooks are chained in load order: the module loaded last is called first
# and passes the data on to the previous one.  With the audit module first,
# on a write the encryption module calls the audit module before encrypting
# (the audit sees plaintext); with the encryption module first, the audit
# module is outermost and sees what the encryption module returned.  Either
# way, data must stay correct and the audit must count the I/O.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $marker = 'hook-chain-plaintext-marker';

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

foreach my $order ('pg_io_audit,pg_page_crypt', 'pg_page_crypt,pg_io_audit')
{
	my $name = $order;
	$name =~ s/[^a-z_]+/_/g;
	note "load order: $order";

	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init(extra => ['--data-checksums']);
	$node->start;
	$node->safe_psql('postgres', qq{
CREATE TABLE t (id int, v text);
INSERT INTO t SELECT g, '$marker ' || g FROM generate_series(1, 3000) g;
});
	$node->stop;
	command_ok(['pg_page_crypt_convert', '--encrypt', '-D', $node->data_dir, '--key-file', $key],
		"$order: encrypt the cluster");

	$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$order'
pg_page_crypt.key_file = '$key'
pg_io_audit.trace_size = 100000
});
	$node->start;
	$node->safe_psql('postgres', 'CREATE EXTENSION pg_io_audit');
	$node->safe_psql('postgres', 'SELECT pg_io_audit_reset()');

	# Reads from disk go through both hooks.
	$node->restart;
	my $filenode = $node->safe_psql('postgres', "SELECT pg_relation_filenode('t')");
	is($node->safe_psql('postgres', "SELECT count(*) FROM t WHERE v LIKE '$marker%'"),
		'3000', "$order: data is read back correctly");
	is($node->safe_psql('postgres',
			"SELECT count(*) > 0 FROM pg_io_audit_trace WHERE op = 'read' AND relfilenode = $filenode"),
		't', "$order: the audit sees the reads of the table");

	# Writes, extension and checkpoint.
	$node->safe_psql('postgres', qq{
INSERT INTO t SELECT g, '$marker ' || g FROM generate_series(3001, 6000) g;
CHECKPOINT;
});
	is($node->safe_psql('postgres', 'SELECT count(*) FROM t'), '6000', "$order: new rows are readable");
	ok(!raw_has_marker($node, 't'), "$order: the table file is encrypted");
	is($node->safe_psql('postgres',
			"SELECT count(*) > 0 FROM pg_io_audit_trace WHERE op = 'write' AND relfilenode = $filenode"),
		't', "$order: the audit sees the writes of the table");
	is($node->safe_psql('postgres',
			"SELECT count(*) > 0 FROM pg_io_audit_trace WHERE op = 'zeroextend' AND relfilenode = $filenode"),
		't', "$order: the audit sees the extension of the table");

	# Index build uses the extend hook (bulk write).
	$node->safe_psql('postgres', 'CREATE INDEX t_v ON t (v)');
	my $idx = $node->safe_psql('postgres', "SELECT pg_relation_filenode('t_v')");
	is($node->safe_psql('postgres',
			"SELECT count(*) > 0 FROM pg_io_audit_trace WHERE op = 'extend' AND relfilenode = $idx"),
		't', "$order: the audit sees the extend calls of the index build");
	ok(!raw_has_marker($node, 't_v'), "$order: the index file is encrypted");
	is($node->safe_psql('postgres', "SELECT count(*) FROM t WHERE v = '$marker 4242'"),
		'1', "$order: the index works");

	# Crash recovery with both hooks active.
	$node->safe_psql('postgres', "INSERT INTO t VALUES (7000, 'after checkpoint')");
	$node->stop('immediate');
	$node->start;
	is($node->safe_psql('postgres', 'SELECT count(*) FROM t'), '6001', "$order: rows survive crash recovery");

	$node->stop;
}

done_testing();

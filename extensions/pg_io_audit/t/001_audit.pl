# Test pg_io_audit: counters, trace, reset and privileges.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('audit');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_io_audit'
pg_io_audit.trace_size = 100000
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_io_audit');

sub calls
{
	my ($op, $fork) = @_;
	$fork //= 'main';
	return $node->safe_psql('postgres',
		"SELECT coalesce(sum(calls), 0) FROM pg_io_audit_stats WHERE op = '$op' AND fork = '$fork'");
}

$node->safe_psql('postgres', 'SELECT pg_io_audit_reset()');
$node->safe_psql('postgres', 'CREATE TABLE t (id int, v text)');
$node->safe_psql('postgres',
	"INSERT INTO t SELECT g, repeat('x', 100) FROM generate_series(1, 5000) g");
$node->safe_psql('postgres', 'CHECKPOINT');

my $filenode = $node->safe_psql('postgres', "SELECT pg_relation_filenode('t')");

cmp_ok(calls('create'), '>=', 1, 'create is counted');
cmp_ok(calls('zeroextend'), '>=', 1, 'zeroextend is counted');
cmp_ok(calls('write'), '>=', 1, 'write is counted after a checkpoint');
is($node->safe_psql('postgres',
	"SELECT sum(blocks) >= 1 FROM pg_io_audit_stats WHERE op = 'write' AND fork = 'main'"),
	't', 'blocks are counted');

# The trace identifies the relation.
is($node->safe_psql('postgres',
	"SELECT count(*) > 0 FROM pg_io_audit_trace WHERE op = 'zeroextend' AND relfilenode = $filenode"),
	't', 'trace contains the zeroextend of the new table');
is($node->safe_psql('postgres',
	"SELECT count(*) > 0 FROM pg_io_audit_trace WHERE op = 'write' AND relfilenode = $filenode"),
	't', 'trace contains the checkpoint write of the new table');

# Reads happen from disk after a restart.  Shared memory is recreated, so the
# trace is empty; connecting reads catalog pages, but never the new table.
sub table_reads
{
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_io_audit_trace WHERE op = 'read' AND relfilenode = $filenode");
}

$node->restart;
is(table_reads(), '0', 'no read of the table before it is queried');
$node->safe_psql('postgres', 'SELECT count(*) FROM t');
cmp_ok(table_reads(), '>=', 1, 'read of the table is traced');
cmp_ok(calls('read'), '>=', 1, 'read is counted');

# unlink at commit of DROP
$node->safe_psql('postgres', 'DROP TABLE t');
cmp_ok(calls('unlink'), '>=', 1, 'unlink is counted');

# reset
$node->safe_psql('postgres', 'SELECT pg_io_audit_reset()');
is($node->safe_psql('postgres', "SELECT count(*) FROM pg_io_audit_stats WHERE op IN ('create', 'unlink')"),
	'0', 'reset zeroes the counters');

# privileges
$node->safe_psql('postgres', 'CREATE ROLE app LOGIN');
my ($ret, $out, $err) = $node->psql('postgres', 'SELECT * FROM pg_io_audit_stats', extra_params => ['-U', 'app']);
isnt($ret, 0, 'an ordinary role cannot read the statistics');
like($err, qr/permission denied/, 'the error is a permission error');
$node->safe_psql('postgres', 'GRANT pg_monitor TO app');
($ret, $out, $err) = $node->psql('postgres', 'SELECT * FROM pg_io_audit_stats', extra_params => ['-U', 'app']);
is($ret, 0, 'pg_monitor can read the statistics');
($ret, $out, $err) = $node->psql('postgres', 'SELECT pg_io_audit_reset()', extra_params => ['-U', 'app']);
isnt($ret, 0, 'pg_monitor cannot reset');

$node->stop;
done_testing();

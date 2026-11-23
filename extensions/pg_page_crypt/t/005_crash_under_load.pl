# Test pg_page_crypt with crashes in the middle of a write workload: pgbench
# runs against an encrypted cluster whose data does not fit into shared
# buffers, so pages are encrypted, evicted, read back and decrypted all the
# time, and the server is crashed while it does so.  After each crash, crash
# recovery must leave consistent data:
#
# - the balances of pgbench: every transaction adds the same amount to one
#   account, teller and branch, and records it in the history, so the four
#   sums are equal whatever transactions committed before the crash;
# - amcheck finds no problems in the tables and indexes;
# - no page fails its checksum.
#
# Two kinds of crash alternate: killing a backend with SIGKILL (the server
# restarts all processes and reinitializes shared memory by itself) and an
# immediate shutdown (as after a power failure, without a final checkpoint).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use Time::HiRes qw(usleep);

my $rounds = 4;

my $tmp = PostgreSQL::Test::Utils::tempdir;
my $key = "$tmp/key.hex";
{
	my $hex = join '', map { sprintf '%02x', int(rand(256)) } 1 .. 64;
	open my $fh, '>', $key or die "could not write $key: $!";
	print $fh "$hex\n";
	close $fh;
	chmod 0600, $key;
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

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(extra => ['--data-checksums']);
# Data of about 30 MB against 2 MB of shared buffers, and frequent
# checkpoints, so that most reads and writes go through the hooks.
$node->append_conf('postgresql.conf', q{
shared_buffers = 2MB
checkpoint_timeout = 30s
max_wal_size = 32MB
restart_after_crash = on
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION amcheck');
$node->command_ok(['pgbench', '--initialize', '--scale', '2', '--quiet', '-d', $node->connstr('postgres')],
	'initialize the pgbench tables');
$node->stop;

# The tables are converted together with the rest of the cluster.
command_ok(['pg_page_crypt_convert', '--encrypt', '-D', $node->data_dir, '--key-file', $key],
	'encrypt the cluster');
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'pg_page_crypt'
pg_page_crypt.key_file = '$key'
});
$node->start;

my $history = 0;
for my $round (1 .. $rounds)
{
	my $kind = $round % 2 ? 'kill -9 of a backend' : 'immediate shutdown';

	# The workload runs until the crash ends it.
	my ($out, $err) = ('', '');
	my $pgbench = IPC::Run::start(
		[ 'pgbench', '--client', '4', '--time', '300', '--no-vacuum',
		  '-d', $node->connstr('postgres') . ' application_name=load' ],
		'>', \$out, '2>', \$err);

	# Let it run a few seconds, with some checkpoints in between.
	$node->poll_query_until('postgres',
		"SELECT count(*) > $history + 2000 FROM pgbench_history")
	  or die "pgbench made no progress in round $round";
	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->poll_query_until('postgres',
		"SELECT count(*) > $history + 4000 FROM pgbench_history")
	  or die "pgbench made no progress in round $round";

	if ($kind eq 'kill -9 of a backend')
	{
		my $pid = $node->safe_psql('postgres',
			"SELECT pid FROM pg_stat_activity WHERE application_name = 'load' LIMIT 1");
		kill 'KILL', $pid or die "could not kill backend $pid: $!";
		# The server restarts by itself; wait until it accepts connections.
		$pgbench->finish;
		$node->poll_query_until('postgres', 'SELECT true')
		  or die "the server did not restart after round $round";
	}
	else
	{
		$node->stop('immediate');
		$pgbench->finish;
		$node->start;
	}

	my $sums = $node->safe_psql('postgres', q{
SELECT (SELECT sum(abalance) FROM pgbench_accounts) = (SELECT sum(bbalance) FROM pgbench_branches)
   AND (SELECT sum(bbalance) FROM pgbench_branches) = (SELECT sum(tbalance) FROM pgbench_tellers)
   AND (SELECT sum(tbalance) FROM pgbench_tellers) = (SELECT coalesce(sum(delta), 0) FROM pgbench_history)
});
	is($sums, 't', "round $round ($kind): the pgbench balances agree");

	my $count = $node->safe_psql('postgres', 'SELECT count(*) FROM pgbench_history');
	cmp_ok($count, '>', $history, "round $round ($kind): committed transactions survived");
	$history = $count;

	is(amcheck_problems($node), '0', "round $round ($kind): amcheck finds no problems");
}

# Statistics are reset by crash recovery, so the log is the record of
# checksum failures.
unlike(slurp_file($node->logfile), qr/invalid page|checksum verification failed/,
	'the server log reports no damaged pages');

$node->stop;

done_testing();

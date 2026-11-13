# Test pg_page_crypt_convert in less common situations: a shared catalog
# rewritten before the conversion, tablespaces (including a directory of
# another server version in the same location), a data directory path that
# would need quoting in a shell, and moving encrypted data between
# tablespaces.

use strict;
use warnings FATAL => 'all';
use File::Path qw(make_path);
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
	my ($datadir, $path) = @_;
	return index(slurp_file("$datadir/$path"), $marker) >= 0;
}

my $tsdir = PostgreSQL::Test::Utils::tempdir;
my $node = PostgreSQL::Test::Cluster->new('tool');
$node->init(extra => ['--data-checksums']);
$node->start;

# A table in a tablespace, and a shared catalog with a new file number.
$node->safe_psql('postgres', "CREATE TABLESPACE ts LOCATION '$tsdir'");
$node->safe_psql('postgres', qq{
CREATE TABLE in_ts (id int, v text) TABLESPACE ts;
INSERT INTO in_ts SELECT g, '$marker ' || g FROM generate_series(1, 2000) g;
});
$node->safe_psql('postgres', 'VACUUM FULL pg_database');
my $pgdb_path = $node->safe_psql('postgres', "SELECT pg_relation_filepath('pg_database')");
like($pgdb_path, qr{^global/\d+$}, 'pg_database has a new file in global/');
cmp_ok((split m{/}, $pgdb_path)[1], '>=', 16384,
	'the new file of pg_database has a file number that gets encrypted');
my $ts_path = $node->safe_psql('postgres', "SELECT pg_relation_filepath('in_ts')");
my $datadir = $node->data_dir;
$node->stop;

ok(raw_has_marker($datadir, $ts_path), 'baseline: the table in the tablespace is plaintext');
my $pgdb_before = slurp_file("$datadir/$pgdb_path");

# Another cluster's directory in the same tablespace location must be left
# alone (for example after pg_upgrade from an older version).
my $foreign_dir = "$tsdir/PG_16_202307071/16384";
make_path($foreign_dir);
my $foreign_file = "$foreign_dir/16999";
{
	open my $fh, '>', $foreign_file or die "could not write $foreign_file: $!";
	print $fh substr($marker x 1000, 0, 8192);
	close $fh;
}
my $foreign_before = slurp_file($foreign_file);

# Reach the data directory through a path with a quote and a space, which
# would break a command built for a shell.
my $odd_path = "$tmp/it's data";
symlink($datadir, $odd_path) or die "could not create symlink: $!";

command_ok(['pg_page_crypt_convert', '--encrypt', '-D', $odd_path, '--key-file', $key],
	'encrypt through a path that needs quoting');
ok(!raw_has_marker($datadir, $ts_path), 'the table in the tablespace is encrypted');
isnt(slurp_file("$datadir/$pgdb_path"), $pgdb_before,
	'the file of pg_database in global/ is encrypted');
is(slurp_file($foreign_file), $foreign_before,
	"another server version's tablespace directory is not touched");

$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'pg_page_crypt'
pg_page_crypt.key_file = '$key'
});
$node->start;

is($node->safe_psql('postgres', 'SELECT count(*) > 0 FROM pg_database'), 't',
	'a shared catalog rewritten before the conversion is readable');
is($node->safe_psql('postgres', "SELECT count(*) FROM in_ts WHERE v LIKE '$marker%'"), '2000',
	'the table in the tablespace is readable');

# Moving encrypted data between tablespaces.
$node->safe_psql('postgres', 'ALTER TABLE in_ts SET TABLESPACE pg_default');
is($node->safe_psql('postgres', 'SELECT sum(id) FROM in_ts'), '2001000',
	'ALTER TABLE ... SET TABLESPACE works');

$node->safe_psql('postgres', 'CREATE DATABASE db2');
$node->safe_psql('db2', qq{
CREATE TABLE t (id int, v text);
INSERT INTO t SELECT g, '$marker ' || g FROM generate_series(1, 1000) g;
});
# ALTER DATABASE ... SET TABLESPACE copies the files without the storage
# manager, so the encryption must not depend on the tablespace.
$node->safe_psql('postgres', 'ALTER DATABASE db2 SET TABLESPACE ts');
is($node->safe_psql('db2', 'SELECT sum(id) FROM t'), '500500',
	'ALTER DATABASE ... SET TABLESPACE works');
my $db2_path = $node->safe_psql('db2', "SELECT pg_relation_filepath('t')");
like($db2_path, qr{^pg_tblspc/}, 'the database is now in the tablespace');

$node->restart;
is($node->safe_psql('db2', 'SELECT sum(id) FROM t'), '500500', 'data is readable after a restart');
$node->safe_psql('postgres', 'CHECKPOINT');
ok(!raw_has_marker($datadir, $db2_path), 'the moved database is stored encrypted');
$node->stop;

# Decrypt everything, including the tablespace and global/.
command_ok(['pg_page_crypt_convert', '--decrypt', '-D', $odd_path, '--key-file', $key],
	'decrypt through a path that needs quoting');
is(slurp_file($foreign_file), $foreign_before,
	"another server version's tablespace directory is still not touched");

$node->append_conf('postgresql.conf', "shared_preload_libraries = ''");
$node->start;
is($node->safe_psql('postgres', 'SELECT count(*) > 0 FROM pg_database'), 't',
	'the shared catalog is readable after decryption');
is($node->safe_psql('db2', 'SELECT sum(id) FROM t'), '500500',
	'the database in the tablespace is readable after decryption');
ok(raw_has_marker($datadir, $db2_path), 'the files are plaintext again');
$node->stop;

done_testing();

--TEST--
hook pg_send_query
--SKIPIF--
<?php
$plugin = <<<EOF
plugin.register('sql', params => {
    assert(params.query == 'SELECT a FROM b')
    assert(params.server == 'pgsql')
    return block
})
EOF;
include(__DIR__.'/../skipif.inc');
if (!extension_loaded("pgsql")) die("Skipped: pgsql extension required.");
@$con = pg_connect('host=127.0.0.1 port=5432 user=postgres');
if (!$con) die("Skipped: can not connect to postgresql");
pg_close($con);
?>
--INI--
openrasp.root_dir=/tmp/openrasp
openrasp.enforce_policy=Off
--FILE--
<?php
@$con = pg_connect('host=127.0.0.1 port=5432 user=postgres');
pg_send_query($con, 'SELECT a FROM b');
pg_close($con);
?>
--EXPECTREGEX--
<\/script><script>location.href="http[s]?:\/\/.*?request_id=[0-9a-f]{32}"<\/script>
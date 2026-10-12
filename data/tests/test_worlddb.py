"""DB credentials stay inside the realm container; SQL remains an argument."""
import subprocess
import unittest
from unittest.mock import patch
from data import worlddb


class WorldDatabaseCredentials(unittest.TestCase):
    def test_query_uses_container_credentials_and_preserves_sql(self):
        sql = "SELECT 'a; $(not_a_shell_command)' AS note"
        with patch.object(worlddb.subprocess, 'run', return_value=subprocess.CompletedProcess(
                args=[], returncode=0, stdout='entry\tnote\n42\tfixture\n')) as run:
            self.assertEqual(worlddb.query(sql), [{'entry': 42, 'note': 'fixture'}])
        argv = run.call_args.args[0]
        self.assertEqual(argv[-3:], ['world', '-e', sql])
        self.assertIn('MYSQL_PWD="$MYSQL_PASSWORD"', argv[7])
        self.assertIn('--user="$MYSQL_USER"', argv[7])
        self.assertIn('"$@"', argv[7])
        self.assertFalse(any(arg.startswith('-p') for arg in argv))
        self.assertEqual(run.call_args.kwargs['cwd'], worlddb.REALM)

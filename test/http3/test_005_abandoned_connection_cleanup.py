import re
import subprocess
import time

import pytest


class TestAbandonedConnectionCleanup:
    """Test that connection is cleaned up if client process is terminated."""

    def test_001_abandoned_connection_is_reaped(self, env):
        from .env import H3Conf

        H3Conf(env).add_vhost_test1().install()
        assert env.apache_restart() == 0

        url = env.mkurl("https", "test1", "/index.html")
        args = [env.curl, "-s", "-k", "--http3-only", "--max-time", "60"]
        args.extend(env.curl_resolve_args(url, insecure=True))
        args.append(url)

        # Start a connection and kill client without a clean CLOSE.
        proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            time.sleep(0.3)
        finally:
            proc.kill()
            proc.wait(timeout=5)

        # Server should reclaim the connection within timeout.
        pattern = re.compile(r".*\[http3:info].*connection servicing done.*")
        env.httpd_error_log.scan_recent(pattern, timeout=15)

        # Server must remain live and functional.
        assert env.is_live()
        r = env.curl_get(url, options=["--http3-only", "-k"])
        assert r.exit_code == 0, r.stderr + r.stdout
        assert r.response["status"] == 200
        assert r.response["protocol"] == "HTTP/3"

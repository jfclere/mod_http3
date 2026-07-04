from concurrent.futures import ThreadPoolExecutor

import pytest


class TestConcurrency:
    """Test mod_http3 stability under highly concurrent requests."""

    @pytest.fixture(autouse=True, scope="class")
    def _class_scope(self, env):
        from .env import H3Conf

        H3Conf(env).add_vhost_test1().install()
        assert env.apache_restart() == 0

    def test_001_concurrent_connections_survive(self, env):
        get_url = env.mkurl("https", "test1", "/index.html")
        post_url = env.mkurl("https", "test1", "/")
        body = b"x" * 512

        def do_get(_i):
            return env.curl_get(get_url, options=["--http3-only", "-k"])

        def do_post(_i):
            return env.curl_raw(
                post_url,
                options=["--http3-only", "-k", "-X", "POST", "--data-binary", body.decode()],
            )

        def do_put(_i):
            return env.curl_raw(
                post_url,
                options=["--http3-only", "-k", "-X", "PUT", "--data-binary", body.decode()],
            )

        # Interleave requests concurrently to trigger potential race conditions.
        rounds = 4
        per_round = 6
        for _ in range(rounds):
            with ThreadPoolExecutor(max_workers=per_round * 3) as pool:
                futures = []
                for i in range(per_round):
                    futures.append(("get", pool.submit(do_get, i)))
                    futures.append(("post", pool.submit(do_post, i)))
                    futures.append(("put", pool.submit(do_put, i)))
                results = [(kind, f.result()) for kind, f in futures]

            for kind, r in results:
                assert r.exit_code == 0, f"{kind}: " + r.stderr + r.stdout
                if kind == "get":
                    assert r.response["status"] == 200
                else:
                    # POST/PUT to a static docroot will 405, which is acceptable here.
                    assert r.response["status"] in (200, 404, 405)

        # Verify server is still alive and serving.
        assert env.is_live()
        r = env.curl_get(get_url, options=["--http3-only", "-k"])
        assert r.exit_code == 0, r.stderr + r.stdout
        assert r.response["status"] == 200
        assert r.response["protocol"] == "HTTP/3"

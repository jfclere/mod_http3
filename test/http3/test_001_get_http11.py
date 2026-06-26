import json
import re

import pytest

from .env import H3Conf


class TestGetHttp11:
    @pytest.fixture(autouse=True, scope="class")
    def _class_scope(self, env):
        H3Conf(env).add_vhost_test1().install()
        assert env.apache_restart() == 0

    def test_001_alive_json(self, env):
        url = env.mkurl("https", "test1", "/alive.json")
        r = env.curl_get(url, options=["--http1.1", "-k"])
        assert r.exit_code == 0, r.stderr + r.stdout
        assert r.response is not None
        assert r.response["status"] == 200
        assert json.loads(r.response["body"]) == {"alive": True, "protocol": "HTTP/3"}

    def test_002_index_html(self, env):
        url = env.mkurl("https", "test1", "/index.html")
        r = env.curl_get(url, options=["--http1.1", "-k"])
        assert r.exit_code == 0, r.stderr + r.stdout
        assert r.response is not None
        assert r.response["status"] == 200
        body = r.response["body"]
        if isinstance(body, bytes):
            body = body.decode("utf-8", errors="replace")
        assert "<title>mod_http3 test</title>" in body

    def test_003_404_for_missing_file(self, env):
        url = env.mkurl("https", "test1", "/does-not-exist")
        r = env.curl_get(url, options=["--http1.1", "-k"])
        assert r.exit_code == 0, r.stderr + r.stdout
        assert r.response is not None
        assert r.response["status"] == 404

    def test_004_head_request(self, env):
        url = env.mkurl("https", "test1", "/alive.json")
        r = env.curl_get(url, options=["--http1.1", "-k", "-I"])
        assert r.exit_code == 0, r.stderr + r.stdout
        assert r.response is not None
        assert r.response["status"] == 200
        assert r.response.get("json") in (None, {})

    def test_005_user_agent_header(self, env):
        url = env.mkurl("https", "test1", "/alive.json")
        r = env.curl_get(
            url,
            options=[
                "--http1.1",
                "-k",
                "-H",
                "User-Agent: mod_http3-test/1.0",
            ],
        )
        assert r.exit_code == 0, r.stderr + r.stdout
        assert r.response is not None
        assert r.response["status"] == 200

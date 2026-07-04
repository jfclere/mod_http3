"""Test automatic RFC 7838 Alt-Svc header injection."""

from .env import H3Conf


class TestAltSvc:
    def test_001_default_on_over_http1(self, env):
        H3Conf(env).add_vhost_test1().install()
        assert env.apache_restart() == 0

        url = env.mkurl("https", "test1", "/index.html")
        r = env.curl_get(url, options=["--http1.1", "-k"])
        assert r.exit_code == 0, r.stderr + r.stdout
        assert r.response["status"] == 200
        alt_svc = r.response["header"].get("alt-svc")
        assert alt_svc is not None, "Alt-Svc header missing; browsers cannot discover HTTP/3 without it"
        assert alt_svc == f'h3=":{env.https_port}"; ma=86400; persist=1'

    def test_002_custom_max_age(self, env):
        H3Conf(env).add_vhost_test1(extra_lines=["H3AltSvcMaxAge 30"]).install()
        assert env.apache_restart() == 0

        url = env.mkurl("https", "test1", "/index.html")
        r = env.curl_get(url, options=["--http1.1", "-k"])
        assert r.exit_code == 0, r.stderr + r.stdout
        assert r.response["header"].get("alt-svc") == f'h3=":{env.https_port}"; ma=30; persist=1'

    def test_003_disabled_via_directive(self, env):
        H3Conf(env).add_vhost_test1(extra_lines=["H3AltSvc off"]).install()
        assert env.apache_restart() == 0

        url = env.mkurl("https", "test1", "/index.html")
        r = env.curl_get(url, options=["--http1.1", "-k"])
        assert r.exit_code == 0, r.stderr + r.stdout
        assert "alt-svc" not in r.response["header"]

    def test_004_not_injected_for_vhost_without_h3_directives(self, env):
        # No Alt-Svc header for vhost without mod_http3 configured.
        conf = H3Conf(env).add_vhost_test1()
        conf.add_vhost_test2()
        conf.install()
        assert env.apache_restart() == 0

        url = env.mkurl("https", "test2", "/index.html")
        r = env.curl_get(url, options=["--http1.1", "-k"])
        assert r.exit_code == 0, r.stderr + r.stdout
        assert "alt-svc" not in r.response["header"]

    def test_004b_h3_port_defaults_to_the_vhosts_own_tls_port(self, env):
        # H3Port defaults to the vhost's own TLS port when omitted.
        H3Conf(env).add_vhost_test1(h3_port=False).install()
        assert env.apache_restart() == 0

        url = env.mkurl("https", "test1", "/index.html")
        r = env.curl_get(url, options=["--http1.1", "-k"])
        assert r.exit_code == 0, r.stderr + r.stdout
        assert r.response["header"].get("alt-svc") == f'h3=":{env.https_port}"; ma=86400; persist=1'

    def test_005_present_on_actual_http3_responses_too(self, env):
        # Alt-Svc header should also be present on HTTP/3 responses.
        H3Conf(env).add_vhost_test1().install()
        assert env.apache_restart() == 0

        url = env.mkurl("https", "test1", "/index.html")
        r = env.curl_get(url, options=["--http3-only", "-k"])
        assert r.exit_code == 0, r.stderr + r.stdout
        assert r.response["status"] == 200
        assert r.response["protocol"] == "HTTP/3"
        assert r.response["header"].get("alt-svc") == f'h3=":{env.https_port}"; ma=86400; persist=1'

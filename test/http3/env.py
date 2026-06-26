import os

from pyhttpd.env import HttpdTestEnv, HttpdTestSetup
from pyhttpd.conf import HttpdConf
from pyhttpd.certs import Credentials


class H3TestSetup(HttpdTestSetup):
    """Loads mod_http3 in addition to the standard test modules."""

    def __init__(self, env: "H3TestEnv"):
        super().__init__(env)
        self.add_modules(["http3"])

    def _make_modules_conf(self):
        super()._make_modules_conf()
        path = self.env.mod_http3_path
        if not os.path.isfile(path):
            raise RuntimeError(
                f"mod_http3.so not found at {path} - run `cmake --build build` first"
            )
        with open(os.path.join(self.env.server_dir, "conf/modules.conf"), "a") as fd:
            fd.write(f'LoadModule http3_module "{path}"\n')


class H3TestEnv(HttpdTestEnv):
    def __init__(self, pytestconfig=None):
        super().__init__(pytestconfig=pytestconfig)
        self._mod_http3_path = self.config.get("test", "mod_http3_path")
        self._test_cert_file = self.config.get("test", "test_cert_file")
        self._test_key_file = self.config.get("test", "test_key_file")

        self._static_creds = Credentials(
            name="localhost",
            cert=open(self._test_cert_file, "rb").read(),
            pkey=open(self._test_key_file, "rb").read(),
        )
        self._static_creds._cert_file = self._test_cert_file
        self._static_creds._pkey_file = self._test_key_file
        self._httpd_log_modules = ["http3", "ssl"]
        self.add_httpd_conf(["Protocols h3 http/1.1"])

    @property
    def mod_http3_path(self) -> str:
        return self._mod_http3_path

    @property
    def test_cert_file(self) -> str:
        return self._test_cert_file

    @property
    def test_key_file(self) -> str:
        return self._test_key_file

    def issue_certs(self) -> None:
        return

    def get_credentials_for_name(self, dns_name):
        return [self._static_creds]

    def get_ca_pem_file(self, hostname):
        if len(self.get_credentials_for_name(hostname)) > 0:
            return self._test_cert_file
        return None

    def setup_httpd(self, setup=None):
        if setup is None:
            setup = H3TestSetup(env=self)
        return super().setup_httpd(setup=setup)


class H3Conf(HttpdConf):
    def __init__(self, env: H3TestEnv):
        super().__init__(env)

    def add_vhost_test1(self, proxy_self=False, h2proxy_self=False, h3_port=True):
        self.start_vhost(
            [f"test1.{self.env.http_tld}"],
            doc_root="htdocs",
            with_ssl=True,
        )
        if h3_port:
            self.add(f"H3Port {self.env.https_port}")
        self.add("H3CertificatePath " + self.env.test_cert_file)
        self.add("H3CertificateKeyPath " + self.env.test_key_file)
        self.add("Protocols h3 http/1.1")
        self.end_vhost()
        return self

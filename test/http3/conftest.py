import pytest

from .env import H3TestEnv, H3Conf


@pytest.fixture(scope="package")
def env(pytestconfig) -> H3TestEnv:
    e = H3TestEnv(pytestconfig)
    e.setup_httpd()
    H3Conf(e).add_vhost_test1().install()
    assert e.apache_restart() == 0
    yield e
    e.apache_stop()
